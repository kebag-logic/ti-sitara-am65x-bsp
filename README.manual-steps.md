# Remaining manual steps (physical access — do these later)

Everything build/verify/document is done and committed. The steps below are the only ones
that need you at the bench (a card reader, the board, or the boot switch). Design detail is
in `README.safe-update.md`; recovery in `README.uboot.md` + `dfu-recover.sh`.

Why a reflash is needed at all: the board's **current SD bootloader predates P0** (running
`u-boot 2026.04-...g186fd234`, no persistent env / no bootchooser) and its rootfs is the old
clone (no rauc). The A/B image below carries the **P0 bootloader** (persistent redundant env
+ bootcount + `boot.scr` bootchooser) and the **rauc rootfs** in both slots, so once it is
flashed the rest is remote and safe.

---

## 1. Finish P2b — RAUC A↔B failover  (≈10 min, then fully validated)
The rauc rootfs reproduces the board's reachability: `eth1` is static `192.168.1.10` — the
`ssh board` path (`ssh -J jump-host root@192.168.1.10`) — with your `user@host` +
`user@host` keys in `/root/.ssh/authorized_keys`. It also brings up `usb0 = 192.168.7.10`
(composite UAC2+ECM USB gadget) as a second link. A bare Buildroot rootfs boots unreachable
without these, so they were added to the overlay.

1. **Flash the A/B card** (dev host; microSD in a reader — find it with `lsblk`):
   ```sh
   sudo dd if=res/spare-sd/spare-am62x-ab.img of=/dev/sdX bs=4M conv=fsync status=progress; sync
   ```
2. **Boot it:** put the card in the board, boot switch = **SD (B4 OFF)**, power on. It boots
   slot A (`mmcblk1p2`). Give it ~30 s for `usb0` to come up.
3. **Sanity check** (dev host):
   ```sh
   ssh board 'rauc status; rauc --version; pipewire --version; uname -r'
   ```
4. **Validate failover** (dev host):
   ```sh
   ./validate-ab.sh           # installs the bundle to slot B, reboots, confirms switch, marks good
   ```
   Expect `SLOT SWITCHED mmcblk1p2 -> mmcblk1p3` then `PASS`. The previous slot stays as the
   fallback, so this is safe.
5. **Optional rollback test:** `rauc install` a deliberately-broken bundle to the inactive
   slot, reboot, and confirm the boot-loop reverts to the good slot (then `mark-good` the good one).

---

## 2. P3 — FWU firmware A/B  (built, NOT flashed — brick risk)
The FWU U-Boot is built at `u-boot-official/out_fwu/a53/` (separate from the validated
`out_myir`). Flashing `tiboot3`/`tispl` can brick the board, so:
1. **Arm DFU recovery first:** board USB-C → serial-host, then on the dev host `./dfu-recover.sh host-setup`.
2. The bank layout + `dfu_string` offsets in `board/myir/myc_am62x/som.c` are a starting point —
   validate on the **spare** card, never the only good one. eMMC `boot0`/`boot1` (raw) are the
   intended A/B firmware home (no SD-FAT gotcha).
3. **Recover a bad flash:** boot switch **B4 ON (USB-DFU)**, `./dfu-recover.sh recover`, then B4 OFF.

---

## 3. P4 — eMMC-primary field layout  (boot-switch + eMMC flash)
Board eMMC = `mmcblk0` (8 GB, `boot0`/`boot1` = 31 MB HW boot partitions; currently factory
layout, not booted).
1. **Boot switch → eMMC/MMC1** (DEVSTAT bootmode `0x09`, nibble `1001`) — confirm exact switch
   positions against the MYIR table.
2. **Flash eMMC:** bootloaders to `boot0` raw (`echo 0 > /sys/block/mmcblk0boot0/force_ro` then
   `dd`), rootfs A/B to the user area; select the active boot partition with `mmc bootpart enable`.
3. Keep the **SD as the golden recovery** (switch back to SD to recover).

---

## Recovery (always available)
- **Serial console:** serial-host `/dev/ttyACM0`, 115200 8N1.
- **USB-DFU brick recovery:** B4 ON + `./dfu-recover.sh recover`.
- **A/B safety net:** the bootchooser (`bootcount`/`BOOT_ORDER`) auto-reverts a failed slot to
  the previous good one, so a bad rootfs/kernel update never strands the board.
