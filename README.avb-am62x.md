# Getting started — AVB/Milan on the MYIR AM6254

End-to-end build/deploy/bring-up for PipeWire AVB (Milan) on this board. Read
`README.board-facts.md` first for the hardware, and the milan-tests-avb `ARM`
branch for the full porting plan and rationale.

## 0. Prerequisites (on the agent)
- aarch64 cross toolchain (or let Buildroot build its own).
- The sources fetched by `./fetch.sh` (mainline linux, TF-A, OP-TEE,
  ti-linux-firmware, Buildroot, snagboot). Supply the MYIR U-Boot tree as
  `u-boot-official` (referenced by `build.sh`, not fetched).

## 1. Bootloaders (GP, unsigned)
```sh
./build.sh MYIR        # TF-A -> U-Boot R5 (tiboot3) -> OP-TEE -> U-Boot A53 (tispl, u-boot.img)
```
Artifacts: `out_myir/r5/tiboot3-am62x-gp-myc-am62x.bin`,
`out_myir/a53/tispl.bin_unsigned`, `out_myir/a53/u-boot.img`.

## 2. Kernel (mainline, reuse the board config)
The proven path is **`./build-am62-kernel.sh all`** — clone mainline **v7.1**,
**reuse the board's own `/proc/config.gz`**, `olddefconfig`, cross-build
`Image.gz`+modules, and deploy via `extlinux` with the original kernel kept as a
serial-selectable fallback. Full step-by-step + caveats in
`README.install-kernel.md`. The board currently runs a rebuilt **`7.1.0 PREEMPT`**.

For a fully **PREEMPT_RT** kernel, merge the RT/AVB fragment after the config step:
`scripts/kconfig/merge_config.sh -m linux/.config res/kl-avb-rt.config`
(adds `CONFIG_PREEMPT_RT`, `TI_AM65_CPSW_QOS` EST/TAS, `MOTORCOMM_PHY` (YT8531),
pins CPSW/MDIO/PHY + AVB qdiscs/PTP/VLAN/USB-audio; verify `cat /sys/kernel/realtime`→1).

## 3. Root filesystem (Buildroot, AVB)
```sh
cd <buildroot>
make BR2_EXTERNAL=$(pwd)/../ti-sitara-am65x-bsp/br2-external myir_am62x_avb_defconfig
make
```
See `br2-external/README.md`. The Milan PipeWire can be baked in
(`pipewire-milan`) or deployed at runtime via the pipewire-helper.

## 4. Deploy
- **SD (current path)**: `./deploy_sd.sh` copies the bootloaders to the SD `BOOT`
  partition; put `Image`+`*.dtb` (or the FIT `fitimage`) there too; rootfs on
  `mmcblk1p2`. Boot env in `res/uEnv.txt` (console `ttyS7,115200n8`).
- **Network (target path)**: the management/deploy port is **`eth0`** (TFTP/NFS/
  ssh); have U-Boot `tftp` the `Image`+`*.dtb` and keep the SD `uEnv.txt`/
  `extlinux` as fail-over (never overwrite the only good SD boot — no JTAG).
  **While `eth0` is down (cable)**, reach the board via the jump-host jump:
  `ssh board` (= `ssh -J jump-host root@192.168.1.10`). See `README.load.md`.
- **Recovery**: `snagboot` over USB DFU (`deploy.sh` has the `snagrecover` stub).

## 5. Bring up AVB
The rootfs-overlay init services do it at boot: **`S50uac2gadget`** starts the USB
2.0 UAC2 audio gadget, then **`S95avb`** runs the full bring-up (VLAN + mqprio CBS
shaper + gPTP + `allmulti` + base pipewire + pipewire-avb) by delegating to the
deployed `/opt/pipewire-helper` scripts (tunable in `/etc/avb/avb.env`). Manually:
```sh
/opt/pipewire-helper/bringup-avb-am62x.sh eth1   # gPTP + allmulti + pipewire-avb (the whole stack)
```
Never shape/VLAN `eth0` — it is the management/deploy path. `allmulti` on the AVB
port is **required** for stream multicast RX (the `multicast` net stat reads 0 even
when it works — am65-cpsw never implements it; check `rx_packets`).

## 6. Validate
Use the milan-tests-avb methodology (the `milan-avb-validate` skill): AVB
counters, AAF wire-payload extraction, and consume-path THD+N, against the DS20
and jump-host, captured with the ProfiTap on capture-host.
