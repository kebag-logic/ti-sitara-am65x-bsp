# br2-external: KL AM62x AVB rootfs

A Buildroot external tree (`BR2_EXTERNAL`) for the AM62x AVB / TDM8 rootfs. It
is rootfs-only — the kernel and bootloaders are built by the BSP `build.sh` and
`build-tdm8-uac2*.sh`, not by Buildroot.

Two boards are supported; they share everything that is not actually
board-specific.

## Layout

```
br2-external/
  external.desc                         # tree name: KL_AM62X
  external.mk                           # pulls in package/*/*.mk
  Config.in                             # sources package Config.in
  configs/myir_am62x_avb_defconfig      # MYIR MYD-YM62X / MYC-YM6254
  configs/ti_sk_am62b_avb_defconfig     # TI SK-AM62B-P1
  package/pipewire-milan/               # clean recipe for the Milan PipeWire fork
  package/pipewire-upstream/            # official master (module-avb)
  board/common/rootfs-overlay/          # BOARD-INDEPENDENT, applied to both
    usr/sbin/tdm8-uac2.sh               # UAC2 gadget + alsaloop TDM8 bridge
    etc/init.d/S99usb_gadgets           # dispatches on TDM8_ENABLE
    root/setup_gadgets.sh               # the plain composite gadget (TDM8_ENABLE=no)
    root/remove_usb.sh
  board/myir-am62x/
    post-build.sh                       # chmods + bakes .kstage-tdm8 modules in
    rootfs-overlay/etc/avb/avb.env      # AVB_INTERFACE / VLAN id / helper dir
    rootfs-overlay/etc/init.d/S95avb    # boot-time VLAN + shaper + gPTP
    rootfs-overlay/etc/init.d/S99bootgood
    rootfs-overlay/etc/rauc/, etc/fw_env.config   # safe-update A/B
    rootfs-overlay/etc/tdm8/tdm8.env    # TDM8 tunables for this board
  board/ti-sk-am62b/
    post-build.sh                       # chmods + bakes .kstage-tdm8-sk modules in
    rootfs-overlay/etc/tdm8/tdm8.env    # TDM8 tunables for this board
    rootfs-overlay/etc/network/interfaces
```

`BR2_ROOTFS_OVERLAY` in each defconfig lists **two** directories — `common`
first, the board's own second, so a board can override a shared file. If you
have an older Buildroot `.config` lying around it still points at the single
`board/myir-am62x/rootfs-overlay` path and will silently produce a rootfs with
no `tdm8-uac2.sh`: re-run the defconfig target, do not just `make`.

## Build

```sh
cd <buildroot>
make BR2_EXTERNAL=/path/to/ti-sitara-am65x-bsp/br2-external myir_am62x_avb_defconfig
# or:                                                       ti_sk_am62b_avb_defconfig
make
```

Output: `output/images/rootfs.ext4` (+ `rootfs.tar.gz`). Deploy per the BSP
`README.sd-card.md` / TFTP-NFS instructions, or feed `rootfs.tar.gz` to
`build-spare-sd.sh` (see `README.tdm8-uac2.md` §5 / `README.tdm8-sk-am62b.md` §5).

Build one board at a time: the two defconfigs share `output/` unless you pass
`O=`.

## PipeWire integration — two options

1. **Bake it in** (this defconfig): `BR2_PACKAGE_PIPEWIRE_MILAN=y` builds the
   Milan fork (pinned to the helper's submodule commit) with `module-avb`. It
   conflicts with the stock `BR2_PACKAGE_PIPEWIRE`, so only one is selected.
2. **Deploy at runtime**: keep stock Buildroot PipeWire and cross-build + push
   the Milan build with the pipewire-helper `build-and-install` script. Then
   `S95avb` and the helper run scripts drive it from `/opt/pipewire-helper`.

## Notes

- `S95avb` only does the network prep (VLAN + `prepare-traffic-shaper-am62x.sh`
  + `ptp-start.sh`); it expects the helper scripts under `/opt/pipewire-helper`.
- Confirm `AVB_INTERFACE` (eth0 vs eth1) on the running board before relying on
  the default.
