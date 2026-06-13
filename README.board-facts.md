# MYIR MYD-YM62X / MYC-YM62X (AM6254) — board fact sheet

Sourced from the manufacturer PDFs (docs.tar.gz). SoM under test:
MYC-YM6254-8E2D (quad Cortex-A53 @ 1.4 GHz, 2 GB DDR4, 8 GB eMMC).

## Ethernet (AVB-relevant)
- **2× RJ45, both gigabit** (10/100/1000), each via a **Motorcomm YT8531** PHY.
- CPSW3g MAC, **RGMII1 → ENET1 (J22)** and **RGMII2 → ENET2 (J23)**; MAC supports
  **IEEE 1588**. PHY driver: `drivers/net/phy/motorcomm.c`.
- eth0/eth1 → RJ45 mapping is **not documented** — confirm on the running board
  (`ethtool -P`, `dmesg`, DT alias). AVB binds one interface; default to ENET1.

## USB (UAC2 audio endpoint)
- **2× USB 2.0 Type-A host** (J16, double-stacked) + **1× USB-C OTG** (J15/J10).
- **No USB3.** Plug the USB Audio Class 2.0 interface into a Type-A host port.

## Audio (on-board, not used by the USB-audio demo)
- **SGTL5000XNAA3** codec on **McASP**, broken out on header **J14**
  (stereo line/HP out + mic in; no line-in). ALSA in the FULL image only.

## Serial console
- SoC **UART0 / WKUP_UART0**, **115200 8N1**, no flow control.
- Two options: 3-pin 3V3 header **J26** (RX/TX/GND) or **USB-C debug J12**
  (CH342 bridge → "USB-Enhanced-SERIAL-A CH342" COM port). MYIR recommends J12.
- The project boots with `console=ttyS7,115200n8` (see `res/uEnv.txt`); the ttySx
  index is not in the docs — it is the project-determined mapping.

## Boot
- Ships booting from **eMMC** (factory image). Boot switch (B3..B9, ON=1):
  - **SD (MMCSD):** `0 0 0 1 / 0 0 1`
  - **eMMC:** `1 0 0 1 / 0 0 0`
  - **OSPI:** `0 1 1 1 / 0 0 1`
- U-Boot supports **USB DFU** (used by `snagboot` for no-JTAG recovery).
- **Security variant**: docs do not state GP vs HS. The project targets **GP**
  (build uses `gp-evm` + unsigned `tispl`; `README.silcons` notes GP/old silicon).

## As-shipped software baseline (porting reference only)
- Kernel **6.1.46** (`MYIR-TI/myir-ti-linux`, branch `myd-am62x-linux-6.1.46`).
- U-Boot **2023.04** (`MYIR-TI/myir-ti-uboot`).
- **Yocto / Arago** (not Buildroot). Root login: user `root`, **empty password**.
  Hostname `myd-am62x`. Network: DHCP / random MAC (set static via
  `/etc/network/interfaces`).
- The target build moves to mainline kernel + Buildroot rootfs (see README.md).
