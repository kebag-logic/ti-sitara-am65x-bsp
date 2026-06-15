# Safe field updates on the MYIR AM6254 — full-stack design (incl. SPL)

Goal: update **any** layer (rootfs, kernel, U-Boot, even `tiboot3`) over the wire and
**never brick** the board — a failed update always rolls back to a known-good state
without a serial console or SD-card reader. Today there is none of this (see the gap
below); this is the target architecture and a safe, phased way to get there.

## The three layers (each fails differently)
| Layer | Recover via | Net |
|---|---|---|
| rootfs + kernel | A/B slot + `bootcount`/`altbootcmd` auto-revert | U-Boot |
| U-Boot (`u-boot.img`/`tispl`) | A/B bank, **FWU multi-bank** (EBBR) | SPL/U-Boot |
| `tiboot3` (R5 SPL) | runs before us → **K3 ROM backup boot mode** or eMMC boot0/1 A/B | ROM |

Downstream logic cannot rescue an upstream failure, so each layer needs its own net.

## Starting gap (2026-06-15) and what closed it
- **RESOLVED (P0):** was `CONFIG_ENV_IS_NOWHERE=y` (volatile env, `saveenv` a no-op —
  blocked every fallback) + `BOOTCOUNT_LIMIT` unset → now redundant raw MMC env +
  `BOOTCOUNT_ENV` + `fw_setenv` in the rootfs.
- **RESOLVED (P2a/P2b):** was no A/B layout, no update framework, `mmc-utils`/`rauc` not
  in the rootfs → now MBR A/B card + RAUC 1.15.2 userspace + signed bundle (verified).
- **Still open (P3):** SD is **MBR** but FWU multi-bank wants **GPT**, while the K3 ROM
  needs an MBR-readable FAT for `tiboot3` — so firmware A/B needs a GPT *data* medium or a
  ROM-compatible layout (see P3). FWU runtime still unbuilt.
- Assets present: K3 ROM **primary/backup boot modes**, eMMC **boot0/boot1**, DFU R5+A53
  configs, `SPL_FIT`, manual recoverable flow (`flash-uboot-sd.sh`), USB-DFU net
  (`dfu-recover.sh`).

## Recovery foundations (bottom-up)
1. **SPL/ROM net — the key one.** Set the board's **backup boot mode = USB DFU** (the
   `/`-nibble in the boot switch; confirm the pin map in the AM62x TRM). A bad primary
   `tiboot3` then auto-falls to DFU → recover with `snagboot`/`snagrecover` over USB, no
   media swap. (eMMC `boot0/boot1` A/B is a second option but the ROM won't auto-swap
   between them — the backup-mode net is the one that self-recovers.)
2. **Firmware A/B — FWU multi-bank.** Two banks of `{tiboot3,tispl,u-boot.img}` on a
   **GPT** medium with FWU metadata; rebuild U-Boot with `FWU_MDATA`,
   `FWU_MULTI_BANK_UPDATE`, `CMD_FWU`, `FWU_MDATA_GPT_BLK`. Updater writes the inactive
   bank, marks it *trial*; `bootcount` confirms or reverts the bank.
3. **OS A/B — RAUC.** A/B rootfs+kernel; U-Boot `bootcount`/`altbootcmd`; the OS resets
   the count only after a health check, so a boot-loop reverts the slot. RAUC is the
   clean Buildroot+U-Boot fit (`BR2_PACKAGE_RAUC`).
4. **Persistent redundant env** (prereq for all): `ENV_IS_IN_MMC`/`ENV_IS_IN_FAT` +
   `SYS_REDUNDAND_ENVIRONMENT`.

## Phased plan — prototype on the SPARE SD, golden SD stays the escape hatch
Every phase ends with a **deliberate-failure test** (corrupt the new slot, confirm the
net recovers) before it is trusted; nothing risky touches the golden SD first.
- **P0 (prereq) — DONE 2026-06-15, validated:** redundant raw MMC env on the SD
  (`ENV_IS_IN_MMC` dev 1, `0x80000`/`0xC0000` in the empty pre-partition gap;
  `ENV_REDUNDANT`; `ENV_MMC_DEVICE_INDEX=1`), `BOOTCOUNT_ENV`, `fw_setenv` from Linux
  (`libubootenv` + `/etc/fw_env.config`), and `S99bootgood` (commits the trial on a
  healthy boot). Validated: marker+bootcount persist across reboot; commit path clears
  `upgrade_available`; fallback path (`bootcount>bootlimit`) runs `altbootcmd` and boots.
  **Gotchas:** `altbootcmd` MUST mirror `bootcmd` (`run envboot; run distro_bootcmd`) —
  bare `run distro_bootcmd` lands at the extlinux menu without auto-selecting and strands
  the board; `bootcount_env` only saves when `upgrade_available!=0` (RAUC trial gate).
- **P1 (SPL net) — DONE 2026-06-15, validated:** USB-C DFU recovery via `snagboot` on
  serial-host. `./dfu-recover.sh {build|host-setup|recover}` builds DFU-capable bootloaders
  (R5+A53 `am62x_*_usbdfu.config`), installs+patches snagboot, and pushes
  `tiboot3→tispl→u-boot` over USB-C. Validated: with the board in **USB-DFU boot mode
  (primary boot-switch B4 ON = bootmode `0x0A`)**, `snagrecover -s am625` recovered a
  board with no usable on-media bootloader → booted U-Boot over USB → kernel from SD.
  **Required fix:** snagboot's `get_string(dev, intf.iInterface)` must pass an explicit
  langid `0x0409` — the TI ROM DFU returns no langid list (`dfu-recover.sh host-setup`
  applies it). **Net options:** (1) *manual, proven now* — normal boot = SD (B4 OFF); to
  recover, flip B4 ON + `dfu-recover.sh recover`. (2) *automatic* — primary=SD +
  backup=USB-DFU; needs the MYIR **backup** boot-switch value (the primary nibble decodes
  cleanly — B3–B6 reversed → bootmode[6:3]: SD `0001`=0x08, eMMC `1001`=0x09, USB-DFU
  `0101`=0x0A — but the backup nibble B7–B9 does not map 1:1, so derive it from the MYIR
  table or by reading `devmem 0x43000030` while trying backup settings).
- **P2a (OS A/B boot) — DONE 2026-06-15, validated:** MBR A/B card (`build-spare-sd-ab.sh`):
  300M FAT boot + rootfs.A (`mmcblk1p2`) + rootfs.B (`p3`); RAUC-compatible U-Boot
  bootchooser (`res/ab/boot.cmd`→`boot.scr`, `BOOT_ORDER="A B"`/`BOOT_{A,B}_LEFT`). ROM-boots
  (FAT total-sectors 614400 via `mformat -R 6`; MBR not GPT — see `README.sd-card.md`).
- **P2b (RAUC userspace) — built + verified 2026-06-15:** real Buildroot 2026.05 rootfs
  (pipewire master + AVB, rauc 1.15.2, `fw_setenv`/`mmc`/`mkfs.ext4`, `S99bootgood`) in
  both slots + 643 kernel modules. `etc/rauc/system.conf` (compatible=`myir-am62x`,
  bootloader=`uboot`, `slot.rootfs.{0,1}`=p2/p3 bootname A/B, `statusfile=per-slot`) +
  `keyring.pem`. `build-rauc-bundle.sh` makes a signed `plain` bundle carrying a complete
  slot as a **tar** image (RAUC formats the inactive ext4 slot + extracts → no size limit).
  Verified on the host: `rauc info` confirms the signature (`O=Kebag-Logic,
  CN=myir-am62x-dev`), `Compatible: myir-am62x`, image `Type: tar (detected)`.
  **On-hardware step (needs the A/B card flashed + booted):** `./validate-ab.sh` →
  `rauc install` to the inactive slot, reboot, confirm the slot switched, `rauc status
  mark-good`; the prior slot stays as the fallback (safe). Test: install a bad slot →
  boot-loop reverts to the prior slot.
- **P3 (firmware A/B):** rebuild U-Boot with FWU; two firmware banks. Test: write a bad
  `u-boot.img` to the inactive bank → confirm trial-boot reverts the bank.
- **P4 (field layout):** make eMMC primary + SD/DFU backup (or keep SD primary), document
  the OTA flow end-to-end.

## Recovery cheat-sheet (until P1 lands)
No DFU yet → a bad bootloader/env recovers via the **serial console** or an SD reader.
The board's U-Boot console is on **serial-host `/dev/ttyACM0`** (CH342 `1a86:55d2` if0,
115200 8N1; `alex` is in `dialout`). Drive it headless, e.g.:
```sh
ssh serial-host 'stty -F /dev/ttyACM0 115200 cs8 -parenb raw -echo
  timeout 20 cat /dev/ttyACM0 & sleep .5
  printf "\003\r" >/dev/ttyACM0; sleep 1                 # Ctrl-C: abort a stuck extlinux menu -> => prompt
  printf "setenv upgrade_available 0\r" >/dev/ttyACM0
  printf "setenv bootlimit 0\r;saveenv\r" >/dev/ttyACM0; sleep 2
  printf "run bootcmd\r" >/dev/ttyACM0'                  # boots the normal path
```
Or via SD reader: copy `u-boot.img.v2026ok` over `u-boot.img` (reverts to the env-less
v2026.04 that boots regardless).

**USB-DFU recovery (P1, no media swap):** if the on-media bootloader is dead, set the
board to USB-DFU boot (primary boot-switch **B4 ON**), power-cycle, then on the dev host
`./dfu-recover.sh recover` (or `watch`) — snagboot on serial-host reflashes the whole chain
over USB-C and the board boots. Flip B4 OFF afterwards for normal SD boot.
