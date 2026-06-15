# MYIR MYD-YM62X / MYC-YM62X (AM6254) — board fact sheet

Sourced from the manufacturer PDFs (docs.tar.gz). SoM under test:
MYC-YM6254-8E2D (quad Cortex-A53 @ 1.4 GHz, 2 GB DDR4, 8 GB eMMC).

## Ethernet (AVB-relevant)
- **2× RJ45, both gigabit** (10/100/1000) off the CPSW3g MAC over RGMII.
- CPSW3g MAC, **RGMII1 → ENET1 (J22)** and **RGMII2 → ENET2 (J23)**; MAC supports
  **IEEE 1588**. The DT gives the PHYs **no vendor `compatible`**, so the running
  kernel binds the **generic PHY** driver (verified on-board: `MOTORCOMM_PHY` is not
  even built; the YT8531 named in the MYD PDF is driven generically). **MDIO
  addresses: `eth0` = 5, `eth1` = 1.** Both PHYs are held in reset by an **NXP
  PCA9555** (`nxp,pca9555` @ I2C `0x20` on `main_i2c1`, pins 5/6 → needs
  `CONFIG_GPIO_PCA953X`, which the board config has).
- **Role split (confirmed by the project owner)**: **`eth1` = AVB network**
  (to AVB Switch 0; eth1's MAC is **U-Boot-random per boot** — DT `port@2` has no
  efuse MAC — so the AVB entity_id churns), **`eth0` = management/deploy net**
  (`XX:XX:XX:XX:XX:XX`, stable efuse MAC). Bind AVB to `eth1`; ssh/TFTP/NFS over `eth0`.
  `eth0` is currently **down (cable not seated)**, so the board is reached via the
  jump-host jump: `ssh -J jump-host root@192.168.1.10` (eth1 carries a temporary
  `192.168.1.10/24`, dhcpcd static). The physical RJ45→ethN mapping is not in
  the docs — verify with `ip -o addr`/`ethtool -P` on the board if needed.

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
- U-Boot supports **USB DFU** (the basis for `snagboot` no-JTAG recovery) — but
  **DFU/snagboot is not currently set up**, so a bad bootloader recovers only via an
  SD-card reader or serial. Flash bootloaders recoverably (see `README.uboot.md`).
- **Security variant**: docs do not state GP vs HS. The project targets **GP**
  (build uses `gp-evm` + unsigned `tispl`; `README.silcons` notes GP/old silicon).

## As-shipped software baseline (porting reference only)
- Kernel **6.1.46** (`MYIR-TI/myir-ti-linux`, branch `myd-am62x-linux-6.1.46`).
- U-Boot **2023.04** (`MYIR-TI/myir-ti-uboot`).
- **Yocto / Arago** (not Buildroot). Root login: user `root`, **empty password**.
  Hostname `myd-am62x`. Network: DHCP / random MAC (set static via
  `/etc/network/interfaces`).
- **Now running:** a rebuilt **mainline `7.1.0 PREEMPT`** kernel (cross-built from
  the board's own config — see `README.install-kernel.md`) + Buildroot rootfs, with
  the prior `6.16.0-…-dirty` mainline kept as an extlinux fallback.
