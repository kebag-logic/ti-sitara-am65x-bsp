# U-Boot (v2026.04) on the MYIR AM6254 — port, control DTB, recoverable flash

The board's bootloader is **official U-Boot v2026.04** with the MYIR board ported in
(`u-boot-official/`, branch `myir-myc-am62x-v2026.04`). This replaces the as-shipped
MYIR U-Boot 2023.04. Read `README.board-facts.md` for the hardware.

## Boot chain (TI K3)
ROM → **`tiboot3.bin`** (R5 SPL + sysfw) → **`tispl.bin`** (A53 SPL + TF-A `bl31` +
OP-TEE + TI DM) → **`u-boot.img`** (U-Boot proper + **control DTB**, packaged as a
FIT by binman). All three live on the SD FAT boot partition (`mmcblk1p1`).

## Why the control DTB is *inside* `u-boot.img` (it is not a bug)
On K3 the control DTB **must** ship inside the U-Boot FIT. `mach-k3` has no
`board_fdt_blob_setup`, so there is no `OF_BOARD` path that could pull a DTB from
memory/storage at runtime — the modern K3 standard is `OF_UPSTREAM` + `MULTI_DTB_FIT`
with the DTB baked into `u-boot.img`. So "the device tree is in the binary blob" is
**correct and required** here; removing it would leave U-Boot with no control DTB.

## One shared board DTS
`k3-myc-am62x-dev.dts` is the **single board DTS**, and it builds in **both** trees
over the same **kernel-synced** SoC base (`u-boot-official/dts/upstream/...` mirrors
the kernel `k3-am625.dtsi`):
- U-Boot: it is the **control DTB** (`CONFIG_DEFAULT_DEVICE_TREE="k3-myc-am62x-dev"`).
- Kernel: registered in `arch/arm64/boot/dts/ti/Makefile`, builds `…-dev.dtb`.

It was a generic SK-derived template; it is now **corrected to the real board**
(verified live): `eth0` PHY at **MDIO addr 5**, `eth1` at **addr 1**, both reset by
the **PCA9555** on `main_i2c1` (pins 5/6), **generic PHY** (the bogus DP83867 props
were dropped). The built DTB is **semantically equal** to the proven on-SD
`k3-am625x-myd-6254.dtb` for serial/MMC/CPSW/MDIO/PHY/I2C — so it is a safe drop-in.

## Build
```sh
./build.sh MYIR     # TF-A bl31 -> R5 (tiboot3) -> OP-TEE -> A53 (tispl, u-boot.img)
```
Artifacts: `u-boot-official/out_myir/r5/tiboot3-am62x-gp-myc-am62x.bin`,
`u-boot-official/out_myir/a53/{tispl.bin,u-boot.img}` (GP, unsigned). Host deps:
aarch64 + arm-none-eabi toolchains, python (setuptools/swig/yaml/pyelftools/
cryptography/yamllint/jsonschema), `bc`, `dtc`. Only the **A53** carries the board
control DTB; after a DTS edit, rebuilding just the A53 step repackages it.

## Validate first on a spare microSD (recommended, full-chain, no brick risk)
A RAM-jump only re-tests U-Boot *proper*; the ROM + R5/A53 SPL (DDR init, sysfw,
bl31, OP-TEE, DM) — the real brick risk — run *before* you can touch RAM. So the
safe full-chain test is a **second SD**: `build-spare-sd.sh` makes a `dd`-able image
that mirrors today's working SD (kernels/DTB/extlinux/rootfs) with the **new v2026.04
chain** swapped in. Boot it; if it fails, pop the golden SD back in — nothing on the
running system was touched.
```sh
./build-spare-sd.sh                      # -> res/spare-sd/spare-am62x-v2026.img
sudo dd if=res/spare-sd/spare-am62x-v2026.img of=/dev/sdX bs=4M conv=fsync status=progress
```
It boots to the same Linux 7.1.0 (ssh + eth1 static `.10` + gPTP auto-start; bring up
AVB manually via `/opt/pipewire-helper`). Inputs are staged under `res/spare-sd/`
(board rootfs tar + boot files); rebuild the chain first if the DTS/U-Boot changed.

## Flash the golden SD — recoverable, SD stays bootable (`flash-uboot-sd.sh`)
No DFU/snagboot is set up, so a bad bootloader recovers **only** via SD-card reader
or serial. The tool is additive until you `activate`:
```sh
./flash-uboot-sd.sh backup    # live blobs -> on-SD boot-backup/ + host res/sd-boot-backup/
./flash-uboot-sd.sh stage     # new blobs -> SD as *.v2026 (inert; ROM still loads the live names)
./flash-uboot-sd.sh verify    # staged *.v2026 == built artifacts (md5)
./flash-uboot-sd.sh activate  # IRREVERSIBLE next boot: live <- *.v2026  (do at serial, SD reader on hand)
./flash-uboot-sd.sh rollback  # live <- boot-backup/  (recover a bad flash)
```
**Do `activate` only with a serial console open and an SD reader available** — if the
new U-Boot fails, reboot won't recover it remotely. Then `rollback` (or copy
`boot-backup/*` over the live names from a card reader) restores the working boot.
