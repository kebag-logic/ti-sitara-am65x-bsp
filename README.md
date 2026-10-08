#PocketBeagle 2 sources to build from scratch

This is the repository base used to work with the pocketbeagle 2(PB2). The goal
of this repository is to track the list of information and repository that make
the PB2 work.

Additionally, it will provide the support for the caps PB ethernet cap.
The first main supported cap for this repository will be the 
[Ethernet Cap](https://github.com/kebag-logic/pocketbeagle2_ethernet_cap)

# Resources

* [TI SDK for Linux](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/U-Boot/UG-DFU.html)
Only supports for the Development Kit officially provided by TI
* [TI Migration Kit (security info)](https://software-dl.ti.com/processor-sdk-linux-rt/esd/AM62X/08_06_00_42/exports/docs/linux/Foundational_Components_Migration_Guide.html#device-types)
* [TI Forum mentioning the duf And more](https://forum.beagleboard.org/t/pocketbeagle-2-boot-with-snagboot/41236)
* [TI K3 U-boot Denx' Documentation](https://docs.u-boot.org/en/latest/board/ti/k3.html)


# Useless yet interesting resources

## Baremetal

[Resources from TI to do RTOS/NORTOS](
https://software-dl.ti.com/mcu-plus-sdk/esd/AM62X/11_01_00_16/exports/docs/api_guide_am62x/index.html)

## Machine learning

[Different solution only valid for GPU/specialized hw accel) based SOPC the AM625x](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components_Machine_Learning.html)
[GStreamer doing some funnly stuffs](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/Machine_Learning/tflite.html#example-applications)

## Virtualization


### The Jailhouse hypervisor

The Jailhouse partitions a system and allows a system to run barelmetal
application beside a linux OS more details:
 
* [Jailhouse from TI and the AM62x perspective](https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/latest/exports/docs/linux/Foundational_Components/Hypervisor/Jailhouse.html#enabling-hypervisor-on-part-family-device-names-platform)
* [JailHouse Hypervisor](https://github.com/siemens/jailhouse)

# Supported boards

`build.sh` builds the bootloader chain; the argument picks the board and the
`u-boot-official/out_<board>/` directory it lands in.

```bash
./build.sh MYIR   # MYIR MYD-YM62X / MYC-YM6254  -> out_myir  (the default)
./build.sh SK     # TI SK-AM62B-P1 (AM625 SK)    -> out_sk
./build.sh PB2    # PocketBeagle 2               -> out_bp2
```

If no parameter is passed, `build.sh` selects MYIR.

## Guides

* `README.prereq.md` — **build-host prerequisites**: toolchains, `dtc`/`bc`,
  `mtools` for the SD images, RAUC/DFU extras, and the loop-module gotcha
* `README.board-facts.md` — the MYIR MYD-YM62X / MYC-YM62X hardware fact sheet
* `README.uboot.md` / `README.sd-card.md` — bootloaders and SD layout
* `README.silcons` — **GP vs HS-FS vs HS-SE**: which `tiboot3` / `tispl.bin` /
  `u-boot.img` each board takes, why HS-FS is secure boot with no encryption and
  no keys of yours, and `res/uboot/check-bootloader.sh` to prove a built chain
  before it reaches a card
* `README.install-kernel.md` - build + deploy a mainline kernel, with a fallback;
  also how this BSP's kernel patch series (`res/tdm8/patches/`) is staged into
  `linux/` by `res/tdm8/apply-tdm8-kernel.sh`, which every kernel build runs
* `README.avb-am62x.md` — PipeWire AVB / Milan bring-up over `eth1`
* `README.safe-update.md` — A/B, RAUC and the USB-C DFU brick-net
* `README.tdm8-uac2.md` — **TDM8 8×8 on McASP1/J11 → USB Audio Class 2.0 gadget**:
  the J11 pinout and cable, the FPGA clock/framing contract, the codec shim,
  the device tree, the alsaloop bridge, and how to build it all into an image
  (rootfs, spare SD, A/B SD, RAUC bundle)
* `README.tdm8-sk-am62b.md` — **the same TDM8 → UAC2 function on the TI
  SK-AM62B-P1**: why that board needs two device trees (McASP1 8×8 vs McASP0 on
  the 40-pin header J3, capture only), the connector tables, and its build path
* `README.tdm8-pb2.md` — **the same TDM8 → UAC2 function on the PocketBeagle 2**
  (AM6254, quad A53): the board that brings a whole McASP0 out to P1/P2, so the
  8×8 duplex link is four jumper wires and no soldering — header/ball/pad
  tables, the two-balls-per-header-pin trap, and its build path
* `README.pb2-ethcap.md` — **the PocketBeagle 2 with the Kebag-Logic Ethernet
  Cap rev B**: DP83867IR on CPSW3G port 2 (RGMII2) as `eth0`, with the CPTS PTP
  clock and the TSN qdiscs; the header-pin map, the device tree, and the
  `ethcap` image
* `README.pb2-tsn.md` — **TSN on the PocketBeagle 2 Ethernet Cap**, the
  foundation of the USB-to-Milan bridge: the `PREEMPT_RT` kernel and the
  isolated core, gPTP on the CPTS clock, the VLAN and the am65-cpsw class A/B
  shaper, the `S95avb` service, the bridge's UAC2 gadget profile, and the
  validation kit (`validation/pb2-tsn/`)
* `README.tdm8-validation.md` — bench log of the FPGA ↔ McASP1 link: what is
  proven, what is not, and the reproducible procedure

# License

Copyright 2025-2026 Alexandre Malki <alexandremalki89@gmail.com>

Unless a file says otherwise, this repository is licensed under the
[Apache License 2.0](LICENSE); see also [NOTICE](NOTICE). A file's own
`SPDX-License-Identifier` header takes precedence:

| Files | License |
|---|---|
| build and helper scripts already carrying an `MIT` header (`build-*.sh`, `res/uboot/*.sh`, `res/tdm8*/apply-*.sh`, `tdm8-uac2.sh`, `S05growrootfs`, ...) | MIT, as they were published before this repository had a license |
| device trees (`res/tdm8-pb2/*.dts`, `res/tdm8-sk/*.dts`) | `GPL-2.0-only OR MIT`, the kernel's device-tree convention |
| Linux kernel code: `res/tdm8/kl-tdm8-dummy.c`, `res/tdm8/patches/*.patch` | GPL-2.0-only |

Third-party material in this repository is **not** covered by the Apache
license and keeps its own terms:

* `res/ethernetgadget.pdf` — *USB Ethernet Gadget: A Beginner's Guide*,
  copyright 2021-2024 its author, CC BY-NC-SA 4.0
* `res/bootprocess.png`, `sram_layout_R5.png` — screenshots of the U-Boot K3
  and TI Processor SDK documentation, copyright their owners
* `res/k3-am625x-myd-6254.dts` — decompiled from MYIR's MYD-YM62X vendor
  device tree

The `linux` submodule, and the U-Boot, TF-A, OP-TEE, TI firmware and Buildroot
trees that `fetch.sh` clones, keep their own licenses. So does everything a
release image is built from; each release's notes list those sources.
