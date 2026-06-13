# br2-external: KL AM62x AVB rootfs

A Buildroot external tree (`BR2_EXTERNAL`) skeleton for the MYIR AM6254 AVB
rootfs. It is rootfs-only — the kernel and bootloaders are built by the BSP
`build.sh`, not by Buildroot.

## Layout

```
br2-external/
  external.desc                         # tree name: KL_AM62X
  external.mk                           # pulls in package/*/*.mk
  Config.in                             # sources package Config.in
  configs/myir_am62x_avb_defconfig      # the AVB rootfs defconfig
  package/pipewire-milan/               # clean recipe for the Milan PipeWire fork
  board/myir-am62x/
    post-build.sh                       # marks the AVB init script executable
    rootfs-overlay/etc/avb/avb.env      # AVB_INTERFACE / VLAN id / helper dir
    rootfs-overlay/etc/init.d/S95avb    # boot-time VLAN + shaper + gPTP
```

## Build

```sh
cd <buildroot>
make BR2_EXTERNAL=/path/to/ti-sitara-am65x-bsp/br2-external myir_am62x_avb_defconfig
make
```

Output: `output/images/rootfs.ext4` (+ `rootfs.tar.gz`). Deploy per the BSP
`README.sd-card.md` / TFTP-NFS instructions.

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
