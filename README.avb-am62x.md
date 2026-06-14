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

## 2. Kernel (mainline + RT + AVB)
```sh
cd linux
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig   # or the BSP res/defconfig
scripts/kconfig/merge_config.sh -m .config ../res/kl-avb-rt.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image dtbs
```
`res/kl-avb-rt.config` adds `CONFIG_PREEMPT_RT`, `CONFIG_TI_AM65_CPSW_QOS`
(EST/TAS offload), `CONFIG_MOTORCOMM_PHY` (YT8531), pins the CPSW/MDIO/PHY
symbols and the AVB qdiscs/PTP/VLAN/USB-audio. Verify on boot:
`cat /sys/kernel/realtime` → `1`.

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
- **Network (target path)**: deploy over **`eth1` / 192.168.1.0/24** (agent
  `enp7s0` = .1; `ssh root@<board>` — MYIR Yocto ships ssh, root/empty password).
  Have U-Boot `tftp` the `Image`+`*.dtb` and keep the SD `uEnv.txt`/`extlinux` as
  fail-over (never overwrite the only good SD boot — no JTAG). Keep `eth0` clean
  for AVB. See `README.load.md`.
- **Recovery**: `snagboot` over USB DFU (`deploy.sh` has the `snagrecover` stub).

## 5. Bring up AVB
On the board (or via the deployed `/opt/pipewire-helper`):
```sh
ip -br addr                 # eth0 = AVB net, eth1 = mgmt (192.168.1.x)
setup-vlan.sh eth0          # VLAN id 2 on the AVB port
prepare-traffic-shaper-am62x.sh eth0   # mqprio bw_rlimit (NOT tc cbs offload)
AVB_INTERFACE=eth0 ptp-start.sh eth0   # ptp4l + phc2sys (gPTP)
# then start the Milan pipewire (helper start script / pipewire-avb)
```
Never shape/VLAN `eth1` — it is the deploy/ssh path.
The `S95avb` init script in the rootfs overlay automates steps 1-3 at boot.

## 6. Validate
Use the milan-tests-avb methodology (the `milan-avb-validate` skill): AVB
counters, AAF wire-payload extraction, and consume-path THD+N, against the DS20
and jump-host, captured with the ProfiTap on capture-host.
