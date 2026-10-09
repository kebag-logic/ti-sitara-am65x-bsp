# PocketBeagle 2 + Kebag-Logic Ethernet Cap (rev B)

The [Ethernet Cap](https://github.com/kebag-logic/pocketbeagle2_ethernet_cap)
puts a TI **DP83867IR** gigabit PHY and an RJ45 magjack on the PocketBeagle 2's
P1/P2 headers. The PHY sits on the AM62x **CPSW3G port 2 (RGMII2)**, so the
board gets a real `eth0`, with the CPTS hardware PTP clock and the CPSW
TSN offloads (taprio/EST, mqprio, cbs) the cap exists to expose.

This targets **rev B**, the `revb-respin` merge (its
`documentation/handover_revb_respin.md` is the hardware side of this file).
Rev A had RX_CTRL strapped to the forbidden mode 1; rev B fixed that, so this
tree does **not** carry `ti,dp83867-rxctrl-strap-quirk`. Do not boot it on a
rev A board without adding that property.

Everything else (kernel, bootloader chain, rootfs, card layout) is the
PocketBeagle 2 build in `README.tdm8-pb2.md`. This file covers only what the
cap adds.

> **Status:** brought up on a rev B board. §5 is the bring-up check. TSN on
> top of it (gPTP, shaper, the USB-to-Milan bridge) is `README.pb2-tsn.md`.

---

## Prebuilt image

The [`pb2-ethcap-2026.10.07-r2`](https://github.com/kebag-logic/ti-sitara-am65x-bsp/releases/tag/pb2-ethcap-2026.10.07-r2)
release carries the card this file describes, xz-compressed:

```sh
sha256sum -c SHA256SUMS --ignore-missing
xzcat pocketbeagle2-ethcap-2026.10.07-r2.img.xz |
  sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

The first boot grows `/` to the whole card (`README.tdm8-pb2.md` §5,
"First boot fills the card"). Then log in as below.

---

## 0. The commands, in order

From the BSP root. Steps 1–3 of `README.tdm8-pb2.md` §0 (sources, `./build.sh
PB2`, `bootloader`) come first, once.

```sh
# kernel + all four PB2 device trees, staged with "ethcap" as the default label
PB2_DEFAULT_LABEL=ethcap ./build-tdm8-uac2-pb2.sh image

# rootfs (after the kernel: post-build bakes in .kstage-tdm8-pb2)
make -C ../buildroot O=$PWD/../buildroot-pb2 \
     BR2_EXTERNAL=$PWD/br2-external bb_pocketbeagle2_avb_defconfig
make -C ../buildroot O=$PWD/../buildroot-pb2 BR2_JLEVEL=32

# the card: named after the staged default label -> pocketbeagle2-ethcap.img
./build-tdm8-uac2-pb2.sh sdimage

sudo dd if=res/spare-sd-pb2/pocketbeagle2-ethcap.img of=/dev/sdX bs=4M conv=fsync status=progress
```

A board that already boots a card from this BSP needs only the kernel side:

```sh
PB2_DEFAULT_LABEL=ethcap ./build-tdm8-uac2-pb2.sh image
BOARD=pb2 PB2_DEFAULT_LABEL=ethcap ./build-tdm8-uac2-pb2.sh deploy
```

Then log in:

```sh
ssh root@192.168.1.12          # over the cap; password "root" (BR2_TARGET_GENERIC_ROOT_PASSWD)
ssh root@192.168.8.12          # over USB-C (ECM): the ethcap gadget profile's own subnet
```

---

## 1. Cap or TDM8, not both

The cap's RGMII2 and MDIO lines land on **P1.02, P1.04, P2.01 and P2.03**, the
four header pins the TDM8 link uses for McASP0. Each of them is two SoC balls
tied together on the PocketBeagle 2 (`README.tdm8-pb2.md` §1): the TDM8 trees
use one ball, this tree uses the other. So a board runs one function at a
time, chosen at the boot menu:

| Label | Tree | For |
|---|---|---|
| `ethcap` | `k3-am62-pocketbeagle2-ethcap.dtb` | a board wearing the cap |
| `tdm8` / `tdm8-async` | the TDM8 trees | FPGA jumper wires, no cap |
| `notdm8` | stock `k3-am62-pocketbeagle2.dtb` | fallback, nothing on the headers |

All four trees are on every card. `PB2_DEFAULT_LABEL` only picks the one that
boots when nobody touches the console. Booting a TDM8 label with the cap
fitted does no harm electrically: the cap's lines are PHY inputs, MDIO pulled
up, or the parked partner balls. But the TDM8 card does not work, and nor does
Ethernet.

The rootfs is the same for both. Under `ethcap`, `S99usb_gadgets` still builds
the UAC2+ECM gadget, so `usb0` stays up as a second way in. The alsaloop
bridge waits for a `TDM8` card that never appears, gives up after
`TDM8_WAIT_CARDS`, and nothing else is affected.

---

## 2. Header pins

Read from the cap's PCB pad nets (`U1` pad *k* = P1.*k*, pad 36+*k* = P2.*k*)
and checked against the PocketBeagle 2 SRM §4.2 tables:

| Header | Cap net | Ball | Pad | Mux | Function | Partner ball (parked) |
|---|---|---|---|---|---|---|
| P1.02 | `RGMII_TX_CTRL` | AA19 | `0x164` | 0 | `RGMII2_TX_CTL` | E18 `0x1a0` GPIO1_10 |
| P1.04 | `RGMII_TD0` | Y18 | `0x16c` | 0 | `RGMII2_TD0` | D20 `0x1a8` GPIO1_12 |
| P2.31 | `RGMII_TD1` | AA18 | `0x170` | 0 | `RGMII2_TD1` | A13 `0x1b4` GPIO1_15 |
| P2.10 | `RGMII_TD2` | AD21 | `0x174` | 0 | `RGMII2_TD2` | — |
| P2.19 | `RGMII_TD3` | AC20 | `0x178` | 0 | `RGMII2_TD3` | — |
| P1.35 | `RGMII_TX_CLK` | AE21 | `0x168` | 0 | `RGMII2_TXC` | — |
| P1.19 | `RGMII_RX_CTRL` | AD22 | `0x17c` | 0 | `RGMII2_RX_CTL` | (AIN0) |
| P1.27 | `RGMII_RD0` | AE23 | `0x184` | 0 | `RGMII2_RD0` | (AIN4) |
| P1.25 | `RGMII_RD1` | AB20 | `0x188` | 0 | `RGMII2_RD1` | (AIN3) |
| P1.23 | `RGMII_RD2` | AC21 | `0x18c` | 0 | `RGMII2_RD2` | (AIN2) |
| P1.21 | `RGMII_RD3` | AE22 | `0x190` | 0 | `RGMII2_RD3` | (AIN1) |
| P1.34 | `RGMII_RX_CLK` | AD23 | `0x180` | 0 | `RGMII2_RXC` | — |
| P2.01 | `MDIO_MDC` | AD24 | `0x160` | 0 | `MDIO0_MDC` | B20 `0x1a4` GPIO1_11 |
| P2.03 | `MDIO_DIO` | AB22 | `0x15c` | 0 | `MDIO0_MDIO` | B18 `0x19c` GPIO1_9 |
| P1.08 | `RESET_N` | A20 | `0x1b0` | 7 | `GPIO1_14`, PHY reset | — |
| P1.06 | `INT_PWDN_N` | E19 | `0x1ac` | 7 | `GPIO1_13`, input | AD18 `0x140` GPIO0_78 |
| P1.14, P2.23 | `VDD_3V3` | | | | feeds the cap | |

RGMII2 is in the PocketBeagle 2's 3.3 V `VDDSHV2` domain, which matches the
PHY's 3.3 V VDDIO. Every other P1/P2 pin passes straight through to the cap's
stacking headers (J2/J3) and is left exactly as the stock tree has it.

---

## 3. The device tree

`res/tdm8-pb2/k3-am62-pocketbeagle2-ethcap.dts` `#include`s the mainline
`k3-am62-pocketbeagle2.dts` like the TDM8 trees do, and `apply-tdm8-pb2-dts.sh`
installs and registers all three. The delta, and why:

* **CPSW3G on, port 1 off, port 2 = `rgmii-id`.** The PCB adds no clock
  delay, so `rgmii-id` is the honest description. `am65-cpsw-nuss` turns on
  the AM62x MAC's fixed internal TX delay and hands the PHY `rgmii-rxid`, so
  the DP83867 adds only the RX delay. `rgmii-rxid` in the DT (the handover's
  other suggestion) makes v7.1 warn that RGMII without a MAC TX delay is
  unsupported, which is why the SK trees use `rgmii-id` too.
* **`ti,rx-internal-delay = 1.75 ns`**, the handover's starting value. Rev B
  routes RX_CLK 14.1 mm (~95 ps) longer than RD0..3, so the receiver sees about
  1.85 ns. TX_CLK is 15.4 mm (~103 ps) long, on top of the MAC's fixed delay.
* **PHY at `reg = <0>`**: RX_D0/RX_D2/RX_D4 carry no strap resistors (§4).
  `compatible = "ethernet-phy-id2000.a231"` lets the PHY device be created
  without an MDIO read while its reset line is still asserted.
* **`reset-gpios` = GPIO1_14 (P1.08)**, assert 100 µs (≥ 1 µs needed), release
  then wait 1 ms (195 µs needed before the first MDIO access, SNLS484J). The
  straps latch on that edge, and by then the RGMII pads are already no-pull
  inputs. phylib also holds the PHY in reset while `eth0` is down.
* **No PHY interrupt.** INT/PWDN_N is an open-drain level interrupt, and the
  DaVinci GPIO controller takes only edge triggers, so phylib polls (about
  1 s to see a link change). E19 is a plain input, readable with `gpioget`.
* **`enet-phy-lane-swap`.** Rev B routes the MDI pairs mirrored (A↔D, B↔C)
  and turns on DP83867 port mirroring with the LED_0 strap. The driver writes
  the same bit again, so a marginal strap read cannot leave the swapped
  routing without the swap.
* **Partner balls parked** at mux 7, `PIN_INPUT` (no pull, receiver on): E18,
  D20, A13, B20, B18, AD18. This is the handover's "keep the second balls as
  inputs with no pull".
* **`&ad7291` disabled.** P1.19/21/23/25/27 are the PocketBeagle 2's AIN0..4,
  which the on-board MSPM0 samples and presents as an emulated AD7291. With
  the cap they carry the RGMII receive bus, so the readings mean nothing, and
  Linux should not ask for conversions on live 125 MHz lines.
* **`&main_uart1` disabled.** P1.06/P1.08 are UART1 RXD/TXD in the stock tree.
  U-Boot still muxes them before Linux runs. Disabling the node stops
  anything in Linux from muxing them back.
* **Stable MAC.** Port 2 has no eFuse MAC in `k3-am62-main.dtsi`. Port 1 is
  off, so port 2 takes its `ti,syscon-efuse`: the board's own TI address, not
  a random one on every boot.

The interface is `eth0`, because it is the only CPSW port enabled.

---

## 4. Rev B PHY straps (from the PCB, checked against SNLS484J Table 7-6)

| Pin | Resistors | Mode | Result |
|---|---|---|---|
| RX_D0, RX_D2 | none | 1 | PHY address 0 |
| RX_D4 | none | 1 | PHY_ADD4 = 0, ANEG_SEL1 = 0 |
| RX_D5, RX_D6 | none | 1 | auto-MDIX on, full duplex, **RGMII enabled** |
| RX_D7 | R5 2k49 up | 4 | speed optimisation on, **CLK_OUT off** |
| RX_CTRL | R30 5k76 up, R7 2k49 down | 3 | autoneg enabled; strap legal (no quirk) |
| LED_0 | R31 5k76 up, R32 2k49 down | 3 | **port mirroring on**, LED active high |
| LED_1, LED_2 | none | 1 | TX skew strap 2.0 ns (overridden by the driver), LEDs active high |

Mode 1 relies on the PHY's internal 9 kΩ pull-down, with nothing on the SoC
side pulling the other way. That holds because the RGMII pads are no-pull
inputs at power-up and again when Linux releases the reset.

---

## 5. Bring-up check

On the board, booted with the `ethcap` label:

```sh
tr -d '\0' < /proc/device-tree/model     # ... + Kebag-Logic Ethernet Cap rev B ...
dmesg | grep -iE 'cpsw|mdio|dp83867|eth0'
#   davinci_mdio 8000f00.mdio: phy[0]: device 8000f00.mdio:00, driver TI DP83867
#   am65-cpsw-nuss 8000000.ethernet eth0: PHY [8000f00.mdio:00] driver [TI DP83867]
#   ... eth0: Link is Up - 1Gbps/Full
ip -br addr show eth0                    # 192.168.1.12/24, UP
ethtool eth0 | grep -E 'Speed|Duplex|Link detected'
ethtool -T eth0                          # PTP Hardware Clock: 0, hardware-transmit/receive
```

Then load the link. This is the 1000 Mb/s check the handover asks for on the
shared header pins:

```sh
iperf3 -s                                # on the board
iperf3 -c 192.168.1.12 -t 30             # on the host, then add -R for the other way
ethtool -S eth0 | grep -iE 'err|crc|align|drop'   # must stay at 0
```

and the PTP clock:

```sh
ptp4l -i eth0 -2 -m --tx_timestamp_timeout=20   # gPTP needs an AVB switch or peer on the wire
```

| Symptom | Look at |
|---|---|
| No `phy[0]` line, `eth0` never gets a PHY | MDIO (P2.01/P2.03 seated? 2k2 pull-ups on the cap), PHY reset on P1.08, PHY address straps |
| Link up, but no traffic or CRC errors at 1000 | RGMII delays: change `ti,rx-internal-delay` in 0.25 ns steps (`DP83867_RGMIIDCTL_1_50_NS`, `_2_00_NS`, ...). Check 100 Mb/s with `ethtool -s eth0 speed 100 duplex full autoneg on` to separate timing from wiring |
| Link up at 100, never at 1000 | a pair problem (one of the four MDI pairs). Speed optimisation (RX_D7 strap) then downshifts on purpose |
| `Use random MAC address` in dmesg | the eFuse MAC read back as zero: set `local-mac-address` on `&cpsw_port2` |
| `eth0` missing entirely | booted a `tdm8*`/`notdm8` label; `cat /proc/device-tree/model` |
| `ethtool -T` shows no PHC | `CONFIG_TI_K3_AM65_CPTS` not in the running kernel; rebuild with `image` |

---

## 6. Files

| File | What the cap changed |
|---|---|
| `res/tdm8-pb2/k3-am62-pocketbeagle2-ethcap.dts` | new: the tree described in §3 |
| `res/tdm8-pb2/apply-tdm8-pb2-dts.sh` | installs and registers it with the two TDM8 trees |
| `res/kl-pb2-am62.config` | CPSW, DaVinci MDIO, GMII_SEL, DP83867 built in; CPTS, CPSW_QOS, VLAN, mqprio/cbs/etf/taprio |
| `build-tdm8-uac2-pb2.sh` | fourth DTB, `ethcap` label in `stage`/`deploy`, `PB2_DEFAULT_LABEL`, card named after the default label, config check for the Ethernet stack |
| `br2-external/configs/bb_pocketbeagle2_avb_defconfig` | `ethtool`, `iperf3` (`linuxptp` was already there) |
| `br2-external/board/bb-pocketbeagle2/rootfs-overlay/etc/network/interfaces` | `eth0` static `192.168.1.12/24`, the bench AVB subnet (`.10` MYIR, `.11` SK) |
| `br2-external/board/bb-pocketbeagle2/post-build.sh` | sshd: `PermitRootLogin yes`, `PasswordAuthentication yes` |

**SSH password login.** root logs in with the password `root`
(`BR2_TARGET_GENERIC_ROOT_PASSWD`), as well as with the keys in
`/root/.ssh/authorized_keys`. Everyone knows that password, and `eth0` puts the
board on a real network: run `passwd` before the board leaves the bench.

---

## 7. Updates through RAUC: the A/B card (#27)

The card of §0 has one root partition, so an update means reflashing it. The
A/B card holds two root slots instead, and is updated in place by RAUC: a
bundle is written to the slot not running, the board reboots into it, and it
stays there only once it is healthy. It is flashed **once**; every update after
that is `rauc install`.

| Partition | | |
|---|---|---|
| `mmcblk1p1` | FAT `BOOT` | `tiboot3.bin`, `tispl.bin`, `u-boot.img`, `boot.scr` (no `extlinux.conf`) |
| `mmcblk1p2` | ext4 `rootfs.A` | slot A: the rootfs, `/boot` (`Image.gz`, the four device trees), `/lib/modules` |
| `mmcblk1p3` | ext4 `rootfs.B` | slot B, the same |
| `mmcblk1p4` | ext4 `data` | `/data`: what survives a slot switch, the Milan saved-state journal among it; the first boot grows it to the end of the card |

How a boot picks its slot:

- **U-Boot** keeps a redundant environment in the gap before the first
  partition, at 0x80000 and 0xC0000 (`res/uboot/pb2-ab-env.sh`, on by default
  in `./build.sh PB2`). Linux reaches it with `fw_printenv`/`fw_setenv`
  (`/etc/fw_env.config`).
- **`boot.scr`** (`res/ab/pb2-boot.cmd.in`) is the bootchooser of RAUC's U-Boot
  backend. It takes one attempt of the first slot in `BOOT_ORDER` that has any
  (`BOOT_A_LEFT`, `BOOT_B_LEFT`, 3 each), then boots that slot's kernel with
  the `ethcap` label's arguments, `root=` the slot and `rauc.slot=`. A kernel
  that does not boot, or panics (`panic=5`), resets the board, which costs that
  slot an attempt.
- **`S99bootgood`** marks the booted slot good (`rauc status mark-good`, which
  gives it its attempts back) once the root file system is writable, `flexptpd`
  runs and, with `AVB_STACK=native`, so do the bridge's daemons. It leaves its
  verdict in `/run/bootgood`.
- **`S12watchdog`** starts the hardware watchdog, and the boot deadline:
  - the watchdog is RTI0, petted by busybox `watchdog`. A board that hangs,
    or loses the daemon, resets within 60 s. Once started, it cannot be
    stopped;
  - on an A/B slot, a slot that is not healthy 150 s into the boot
    (`BOOT_DEADLINE_S`) is rebooted.

  So a slot that never gets healthy, or hangs, is left after its 3 attempts,
  and the board is back on the other one, with no hands on it. When the
  other slot has no attempts left either, the board stays up rather than
  loop.

Build and flash, once:

```sh
./build.sh PB2                                   # bootloaders with the A/B environment
PB2_DEFAULT_LABEL=ethcap ./build-tdm8-uac2-pb2.sh image
make -C ../buildroot O=$PWD/../buildroot-pb2 BR2_JLEVEL=32
./build-tdm8-uac2-pb2.sh abcard                  # -> res/spare-sd-pb2/pocketbeagle2-ethcap-ab.img, no root needed
sudo dd if=res/spare-sd-pb2/pocketbeagle2-ethcap-ab.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Update, every time after:

```sh
./build-tdm8-uac2-pb2.sh bundle                  # -> res/rauc/pocketbeagle2-am62x.raucb, signed
scp res/rauc/pocketbeagle2-am62x.raucb root@192.168.8.12:/tmp/
ssh root@192.168.8.12 'rauc install /tmp/pocketbeagle2-am62x.raucb && reboot'
ssh root@192.168.8.12 'rauc status'              # booted from the other slot, marked good
```

The bundle is a tar of one complete slot (RAUC formats the inactive slot and
extracts it), signed with the BSP's development key (`res/rauc/`), compatible
`pocketbeagle2-am62x`: a MYIR bundle is refused, and so is this one on a MYIR
board.
