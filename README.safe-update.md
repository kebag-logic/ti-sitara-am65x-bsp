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

## Current gap (verified 2026-06-15)
- **`CONFIG_ENV_IS_NOWHERE=y`** — U-Boot env is volatile; `saveenv` is a no-op. **This
  blocks every fallback mechanism** and must be fixed first.
- `BOOTCOUNT_LIMIT` unset, FWU runtime unset (only `FWU_NUM_BANKS=2` default), no A/B
  layout, no update framework, SD is MBR (FWU wants GPT), `mmc-utils` not in the rootfs.
- Assets present: K3 ROM **primary/backup boot modes**, eMMC **boot0/boot1**, DFU R5+A53
  configs, `SPL_FIT`, and the manual recoverable flow (`flash-uboot-sd.sh`).

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
- **P0 (prereq):** persistent+redundant env + `bootcount`/`altbootcmd`; add `mmc-utils`.
  Test: break the default boot entry → confirm `altbootcmd` falls back. *(lowest risk)*
- **P1 (SPL net):** set backup boot mode = DFU; wire `snagboot` recovery. Test: zero the
  primary `tiboot3` on the spare → confirm ROM drops to DFU and `snagrecover` restores it.
- **P2 (OS A/B):** GPT layout, A/B rootfs+kernel, RAUC + bootcount. Test: deploy a
  deliberately-panicking kernel to slot B → confirm auto-revert to A.
- **P3 (firmware A/B):** rebuild U-Boot with FWU; two firmware banks. Test: write a bad
  `u-boot.img` to the inactive bank → confirm trial-boot reverts the bank.
- **P4 (field layout):** make eMMC primary + SD/DFU backup (or keep SD primary), document
  the OTA flow end-to-end.

## Recovery cheat-sheet (until P1 lands)
No DFU yet → a bad bootloader recovers only via SD reader (restore `boot-backup/*`) or
serial. So do bootloader changes on the spare SD, never the golden one, until P1.
