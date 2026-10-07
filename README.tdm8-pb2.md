# TDM8 (8 in / 8 out) duplex → USB Audio Class 2.0 gadget on the PocketBeagle 2

The same function `README.tdm8-uac2.md` does for the MYIR AM6254 and
`README.tdm8-sk-am62b.md` does for the TI SK-AM62B-P1: stream the Kebag-Logic
AVB **FPGA**'s TDM8 link into a **USB Audio Class 2.0 device** that the AM62x
presents on its USB-C port, so a host sees one multichannel UAC2 interface and
the FPGA's render/capture lanes can be validated against it.

```
  FPGA  ──8 slots──▶ McASP0 AXR1 ──▶ hw:TDM8 capture  ──alsaloop──▶ gadget playback ──USB IN ──▶ host
  FPGA  ◀─8 slots─── McASP0 AXR0 ◀── hw:TDM8 playback ◀─alsaloop─── gadget capture  ◀─USB OUT─── host
        ◀─BCLK 12.288 MHz ── FSYNC 48 kHz ──  (FPGA is bit-clock and frame master)
```

Everything above the device tree — the codec shim, the kernel fragment, the
gadget builder, the `alsaloop` bridge, `/etc/tdm8/tdm8.env` — is shared with the
other two boards. Read `README.tdm8-uac2.md` first: this file only documents
what is different, and §2, §7 and §10 there apply verbatim.

**Board revision.** This targets PocketBeagle 2 **rev A1**, the in-production
one, which carries the **AM6254 — quad Cortex-A53 @ 1.4 GHz**, 512 MB DDR
(SRM §1.1 / §1.2). Rev A0, out of production, carried the dual-core AM6232.
Both boot the same `k3-am62-pocketbeagle2.dts`, which `#include`s
`k3-am625.dtsi` and therefore always *describes* four cores — on an A0 the two
that are not fused in simply never come online. So "four cores" is a runtime
fact, not a device-tree one. `./build-tdm8-uac2-pb2.sh probe` asks a live board.

---

## 0. The commands, in order

From the BSP root. Steps 1–3 are only for building a whole card; if the board
already boots, do 4 then `deploy` (bottom of this section).

```sh
# ---- 1. sources (once) ------------------------------------------------------
./fetch.sh                      # clones everything; or, on an already-populated
                                # tree, just the one nothing else needs:
git clone https://openbeagle.org/beagleboard/u-boot.git \
    -b v2025.04-rc4-pocketbeagle2 u-boot-pb

# ---- 2. bootloader: TF-A -> R5 SPL -> OP-TEE -> A53 U-Boot (HS-FS) ----------
./build.sh PB2

# ---- 3. prove the bootloader before it can reach a card ---------------------
./build-tdm8-uac2-pb2.sh bootloader

# ---- 4. kernel + device trees + modules -------------------------------------
#        stages the codec shim AND res/tdm8/patches/*.patch AND both TDM8 trees
#        before it configures and builds - none of those is a separate step
./build-tdm8-uac2-pb2.sh image

# ---- 5. rootfs  (MUST follow 4: post-build.sh bakes in .kstage-tdm8-pb2) -----
make -C ../buildroot O=$PWD/../buildroot-pb2 \
     BR2_EXTERNAL=$PWD/br2-external bb_pocketbeagle2_avb_defconfig
make -C ../buildroot O=$PWD/../buildroot-pb2 BR2_JLEVEL=32

# ---- 6. the microSD image  (needs sudo) -------------------------------------
./build-tdm8-uac2-pb2.sh sdimage

# ---- 7. flash ---------------------------------------------------------------
sudo dd if=res/spare-sd-pb2/pocketbeagle2-tdm8.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Already have a booting board? Skip 1, 2, 3 and 6 — this replaces only the
kernel, the device trees and `extlinux.conf` over ssh, and leaves the bootloader
chain on the card alone:

```sh
./build-tdm8-uac2-pb2.sh image
BOARD=pb2 ./build-tdm8-uac2-pb2.sh deploy
```

Three things about that list that are not obvious, each explained where it
belongs below:

* **5 must follow 4.** `post-build.sh` copies `.kstage-tdm8-pb2/lib/modules`
  into the rootfs as Buildroot assembles it. Run 5 first and you get an image
  with no TDM8 modules and no warning. (§5.5)
* **`O=` in step 5 is not optional.** One Buildroot checkout serves three
  boards; building in `../buildroot` itself overwrites the MYIR `.config`.
  Images then land in `<O>/images/`, *not* `<O>/output/images/`. (§5.5)
* **Use `sdimage`, not `build-spare-sd.sh` directly.** Every default in that
  script points at the MYIR GP chain, and `TISPL`/`UB` share a directory with
  the `*_unsigned` twins you must not flash on HS-FS. (5.6)

`BR2_JLEVEL=32` is only needed on a very-high-core-count host: glibc's `manual/`
build races at `-j128` and dies with an empty `libc.info`. Drop it otherwise.

Verify after 5, before 6:

```sh
tar tzf ../buildroot-pb2/images/rootfs.tar.gz |
  grep -cE 'lib/modules/7\.1\.0-tdm8-pb2/.*(kl-tdm8-dummy|davinci-mcasp|simple-card)|usr/sbin/tdm8-uac2\.sh|etc/tdm8/tdm8\.env|usr/bin/alsaloop'
# expect 6 or more
```

And on the board once it is up, the one command that proves the capture path
end to end (see section 3, "The BCDMA cyclic-RX kernel patch", for why):

```sh
arecord -D hw:0,0 -c8 -f S32_LE -r48000 -d3 /tmp/c.wav
ls -l /tmp/c.wav        # exit 0 and exactly 4608044 bytes
```

---

## 1. This is the easy board

On the SK-AM62B-P1 a full-duplex TDM8 link cannot be made from header pins at
all. A McASP transmit section is always clocked from its **own** `ACLKX`/`AFSX`
pins, and per the AM625 datasheet (SPRSP58C tables 5-35..5-37) no transmit frame
sync ball reaches any SK header — which is why every SK duplex tree
(`k3-am625-sk-tdm8-ospi.dts`, `-split.dts`) has to solder onto the OSPI flash
pads and give the NOR flash up.

PocketBeagle 2 brings a **whole McASP0** out to its pre-soldered P1/P2 headers.
From the PocketBeagle 2 SRM rev A1 §4.2 "Cape Header Connectors", all at mux
**MODE 0**:

| Header pin | Ball | PADCONFIG | `AM62X_IOPAD` | McASP0 function |
|---|---|---|---|---|
| **P1.02** | E18 | 104 / `0x000F41A0` | `0x01a0` | `MCASP0_AXR0` |
| **P1.04** | D20 | 106 / `0x000F41A8` | `0x01a8` | `MCASP0_AFSX` |
| **P1.06** | E19 | 107 / `0x000F41AC` | `0x01ac` | `MCASP0_AFSR` |
| **P1.08** | A20 | 108 / `0x000F41B0` | `0x01b0` | `MCASP0_ACLKR` |
| **P1.10** | B19 | 101 / `0x000F4194` | `0x0194` | `MCASP0_AXR3` |
| **P1.12** | A19 | 102 / `0x000F4198` | `0x0198` | `MCASP0_AXR2` |
| **P2.01** | B20 | 105 / `0x000F41A4` | `0x01a4` | `MCASP0_ACLKX` |
| **P2.03** | B18 | 103 / `0x000F419C` | `0x019c` | `MCASP0_AXR1` |

Transmit clock, transmit frame sync, receive clock, receive frame sync and four
data lanes. So the whole 8×8 duplex link is **four jumper wires onto pre-soldered
headers** — no soldering, no FET switch, no sacrificed peripheral, and no pad
that the stock PocketBeagle 2 device tree already owns.

### Wiring — the default tree (`k3-am62-pocketbeagle2-tdm8.dtb`)

| FPGA | Signal | Header | Ball | Pad | Mux | SoC dir |
|---|---|---|---|---|---|---|
| BCLK 12.288 MHz | `MCASP0_ACLKX` | **P2.01** | B20 | `0x1a4` | 0 | in |
| FSYNC 48 kHz | `MCASP0_AFSX` | **P1.04** | D20 | `0x1a8` | 0 | in |
| DIN — 8 slots out | `MCASP0_AXR0` | **P1.02** | E18 | `0x1a0` | 0 | **out** |
| DOUT — 8 slots in | `MCASP0_AXR1` | **P2.03** | B18 | `0x19c` | 0 | in |
| GND | — | P1.15, P1.16, P1.22, P2.15, P2.21 | — | — | — | — |

Receive is left in the transmit clock domain (no `ti,async-mode`), so both
directions share the one BCLK/FSYNC pair. That is what makes it four wires
instead of six.

#### Pin-to-pin: PocketBeagle 2 to AX7101 J11 (default tree, full duplex)

The FPGA TDM wires come out on the AX7101's 40-pin "FPGA 40 PIN External IO"
header **J11** (`sw/litex/platforms/alinx_ax7101.py`, the `tdm` resource). Four
signals plus ground, capture and render:

| Function | AX7101 J11 pin (ball) | Direction | PocketBeagle 2 pin | PB2 signal |
|---|---|---|---|---|
| BCLK 12.288 MHz | **6** (B20) | FPGA -> SoC | **P2.01** | `MCASP0_ACLKX` |
| FSYNC 48 kHz | **8** (F19) | FPGA -> SoC | **P1.04** | `MCASP0_AFSX` |
| Data out, render (8 slots) | **5** (A20) | FPGA -> SoC | **P2.03** | `MCASP0_AXR1` (SoC RX) |
| Data in, capture (8 slots) | **7** (F20) | SoC -> FPGA | **P1.02** | `MCASP0_AXR0` (SoC TX) |
| GND | **1**, **37**/**38** | shared | **P1.15/16/22**, **P2.15/21** | GND |

Rules, same as the other boards: no power rail between the boards (AX7101 J11.2
is +5 V, J11.39/40 are +3.3 V; leave PB2 P1.14/P2.23 3V3 unconnected unless the
FPGA wants a reference); both ends 3.3 V, no level shifter; a 22-33 ohm series
resistor in the BCLK wire (J11.6 -> P2.01); ribbon <= 15 cm with a ground beside
each signal. This is the whole link: four signals on pre-soldered headers, no
solder points, no sacrificed peripheral. The FPGA's `mclk` (J11.3) is unused.

3V3 is on P1.14 and P2.23 if the FPGA side wants a reference; **all expansion
signals are 3.3 V** and the SRM's warning applies — do not drive any I/O pin
while the board is unpowered.

### Wiring — the async tree (`k3-am62-pocketbeagle2-tdm8-async.dtb`)

For an FPGA that drives two independent clock pairs. Same three transmit wires,
plus:

| FPGA | Signal | Header | Ball | Pad | Mux | SoC dir |
|---|---|---|---|---|---|---|
| BCLK (RX domain) | `MCASP0_ACLKR` | **P1.08** | A20 | `0x1b0` | 0 | in |
| FSYNC (RX domain) | `MCASP0_AFSR` | **P1.06** | E19 | `0x1ac` | 0 | in |

and `MCASP0_AXR1` on P2.03 moves into that receive domain.

**Scope P1.08 before you connect anything to it.** P1.06/P1.08 are
`MAIN_UART1` RXD/TXD. `k3-am62-pocketbeagle2.dts` declares `&main_uart1` with
`status = "reserved"` *and* `bootph-pre-ram`: "reserved" keeps Linux from
probing it, so Linux never muxes those pads — but `bootph-pre-ram` means U-Boot
does, and on AM62x `MAIN_UART1` is the port TI's device-manager firmware
conventionally uses for trace output. The async tree disables `&main_uart1` so
nothing in Linux re-muxes them behind your back, and that is all a device tree
can do. If something is driving P1.08 it is `UART1_TXD`, an output, and your
FPGA would be driving into it. P1.06 is `UART1_RXD`, an input, so it is harmless
either way.

Use the four-wire tree unless you actually need split clock domains.

### Two SoC balls per header pin — and why the device tree parks four pads

PocketBeagle 2 keeps the classic PocketBeagle 36-pin headers by tying **two SoC
balls to one header pin** over much of P1/P2 (SRM §4.2: "the 2nd BALL row is the
pin number on the processor for a second processor pin connected to the same pin
on the expansion header"). Every pin this link uses is such a pin:

| Header pin | Our ball | Partner ball | Partner's MODE 0 | Partner pad |
|---|---|---|---|---|
| P1.02 | E18 `MCASP0_AXR0` | AA19 | `RGMII2_TX_CTL` | `0x164` |
| P1.04 | D20 `MCASP0_AFSX` | Y18 | `RGMII2_TD0` | `0x16c` |
| P1.06 | E19 `MCASP0_AFSR` | AD18 | `RGMII1_TD3` | `0x140` |
| P1.08 | A20 `MCASP0_ACLKR` | *(none — single-ball pin)* | — | — |
| P2.01 | B20 `MCASP0_ACLKX` | AD24 | `MDIO0_MDC` | `0x160` |
| P2.03 | B18 `MCASP0_AXR1` | AB22 | `MDIO0_MDIO` | `0x15c` |

The partners are all CPSW/MDIO functions, and PocketBeagle 2 has no Ethernet
connector, so nothing in the stock tree drives them. But "nothing in the stock
tree" is not the same as "guaranteed high-Z", and the FPGA is driving those
partner balls whether we like it or not. So both TDM8 trees add a second pinctrl
group that parks each partner at **mux 7 (GPIO), `PIN_INPUT`**: a GPIO nobody
claims stays an input, which is what keeps the pad off the net, and leaving the
receiver on means you can still read the line back with `gpioget` while
debugging. Without it, a pad left at its reset mux could drive against the FPGA.

### Conflicts with the stock device tree: none

`k3-am62-pocketbeagle2.dts` muxes `0x194`/`0x198`/`0x1ac`/`0x1b0` for
`main_uart1`, which is exactly why the default tree deliberately picks the
*other* four McASP0 pads — `0x19c`, `0x1a0`, `0x1a4`, `0x1a8` appear in no
pinctrl group of the stock tree. `main_uart1`, the four user LEDs (GPIO0_3..6 on
the OSPI0_D0..D3 pads), `main_uart6` (the console), `main_i2c0`/`main_i2c2`, the
microSD and both USB ports are untouched. The default TDM8 tree boots and
consoles exactly like the stock one.

### USB side

`k3-am62-pocketbeagle2.dts` already sets `dr_mode = "peripheral"` on `&usb0`
(the Type-C socket is wired as USB 2.0), so — unlike the SK and MYIR trees —
neither TDM8 tree has to override it. `&usb1` stays a host and keeps P1.03
(`USB1_DRVVBUS`), P1.05, P1.07, P1.09 and P1.11.

One consequence worth planning for: **PocketBeagle 2 has no Ethernet at all.**
(The Kebag-Logic Ethernet Cap adds one, on the same header pins this link
uses, so a board runs one or the other: see `README.pb2-ethcap.md`.)
The ECM leg of the composite gadget on `usb0` is the only network path to the
board, which is why `TDM8_ECM=yes` is not optional here the way it is on the
other two boards. Turn it off and the 3-pin JST-SH serial console is your only
way in.

---

## 2. Clock and framing contract

Identical to the MYIR and SK links — see `README.tdm8-uac2.md` §2. In short: the
FPGA owns the oscillator and is bit-clock and frame master, BCLK = 48 000 × 8 ×
32 = 12.288 MHz, FSYNC = 48 kHz, `dsp_a`, 8 × 32-bit slots with 24 valid bits
left-justified (`S32_LE`). `TDM8_RATE` only tells software what to expect;
nothing on the AM62x side can force a rate onto a link it does not clock.

The default tree runs **synchronous** (no `ti,async-mode`). The async tree exists
only for an FPGA whose two directions are in different clock domains.

---

## 3. What this adds to the BSP

| File | Role |
|---|---|
| `res/tdm8-pb2/k3-am62-pocketbeagle2-tdm8.dts` | McASP0 8×8 duplex, 4 wires on P1/P2. `#include`s mainline `k3-am62-pocketbeagle2.dts`. |
| `res/tdm8-pb2/k3-am62-pocketbeagle2-tdm8-async.dts` | Same, with a separate receive clock pair (6 wires, `&main_uart1` off). |
| `res/tdm8-pb2/apply-tdm8-pb2-dts.sh` | Idempotently copies both into `linux/arch/arm64/boot/dts/ti/` and registers them in that `Makefile`. |
| `res/kl-pb2-am62.config` | Kernel fragment: the PocketBeagle 2 delta on top of `arm64 defconfig` + `res/kl-tdm8-uac2.config`. Pins `CONFIG_LOCALVERSION="-tdm8-pb2"`. |
| `build-tdm8-uac2-pb2.sh` | `fetch → shim → dts → config → dtb → build → {deploy \| stage}`, plus `probe`. |
| `br2-external/board/bb-pocketbeagle2/` | `post-build.sh` (default `KL_MODDIR=.kstage-tdm8-pb2/lib/modules`) plus this board's `etc/tdm8/tdm8.env`, `etc/network/interfaces`, `etc/init.d/S05growrootfs` (first boot grows `/` to the whole card) and `root/.ssh/authorized_keys`. |
| `br2-external/configs/bb_pocketbeagle2_avb_defconfig` | Buildroot config. Mirrors the SK one, plus `iperf3` for the Ethernet Cap, with a 1 G rootfs for a 512 MB board. |
| `res/uboot/check-bootloader.sh` | Verifies a built K3 chain is the variant you meant and that the real TI firmware is inside it. Board-independent: `SOC`/`VARIANT`/`FW` cover the SK and MYIR too. |
| `res/uboot/fix-pb2-uart6-bootph.sh` | Gives the A53 SPL a working console: adds the `bootph-all` that the upstream board DT puts on `&main_uart6` but not on its pinmux group. |
| `res/uboot/fix-pb2-kernel-comp.sh` | Adds `kernel_comp_addr_r` / `kernel_comp_size` to the board env so `booti` can unpack a gzipped `Image`. |
| `res/uboot/pb2-console-on-p1.sh` | `on`/`off`. Moves the A53 U-Boot console to `main_uart0` (P1.30/P1.32) so the whole boot log lands on one wire. |

Shared with the other two boards: `res/tdm8/kl-tdm8-dummy.c`,
`res/tdm8/apply-tdm8-kernel.sh`, `res/tdm8/patches/`,
`res/kl-tdm8-uac2.config`, `br2-external/board/common/rootfs-overlay/`. The
patches are SoC-level, so all three AM62x boards get them; see below.

### The BCDMA cyclic-RX kernel patch

`res/tdm8/patches/0001-dmaengine-ti-k3-udma-count-bcdma-cyclic-rx-static-tr-z-in-bursts.patch` is a
fix in `drivers/dma/ti/k3-udma.c`, and without it **capture does not
work on any of these boards**.

**Symptom.** An 8-channel 48 kHz capture dies after exactly one period,
whatever the period size, with `read error: Input/output error`. The RX
channel takes one TR event and one ring completion and then goes quiet while
the McASP RX FIFO keeps filling.

**Root cause.** On a BCDMA `DEV_TO_MEM` cyclic channel the upstream driver programs the PDMA
static TR Z (`BSTCNT`) in elements, but the PDMA counts it in bursts of `elcnt`
elements. The two agree only while `elcnt` is 1. These boards set
`rx-num-evt = <32>`, so McASP asks for 32-word bursts, the PDMA closes a packet
only every 32 periods while every period's last TR carries EOP, and the cyclic
TR descriptor retires after the first period. The patch computes Z per period
in bursts, as the packet-mode branch already does, and keeps EOP on every RX
period: that is what lets a BCDMA RX channel finish a teardown.

The first version of this fix (kernel build `#5`, 2026-09-17) took EOP off RX
instead. Capture ran, but every capture stop then timed out in
`udma_synchronize()` (`chan<N> teardown timeout!`) and fell back to the hard
channel reset. The alsaloop bridge restarts capture on every xrun, and on the
PocketBeagle 2 that left CPU 0 in an interrupt livelock (RCU stall) seconds
after the bridge started. This version (build `#6`, 2026-09-28) ran the bridge
for 30 minutes with one teardown completion per capture stop and no stall.

**It is not a manual step.** `res/tdm8/apply-tdm8-kernel.sh` stages every
`res/tdm8/patches/*.patch` in lexical order alongside the codec shim, and every
phase that builds a kernel runs it first, on all three boards. It is
idempotent: a patch already in the tree is recognised and skipped, and one that
no longer applies stops the build by name rather than half-patching `linux/`.
There is nothing to apply by hand and nothing to commit into the `linux`
submodule.

**Verify on the board.** This is the test, and the byte count is the whole
answer:

```sh
arecord -D hw:0,0 -c8 -f S32_LE -r48000 -d3 /tmp/c.wav
ls -l /tmp/c.wav
# 4608044 bytes = 44 byte WAV header + 3 s x 48000 frames x 8 ch x 4 bytes
```

`arecord` must exit 0. A kernel built before this patch fails the same command
after one period with `read error: Input/output error` and leaves a short file.
If you want to watch the mechanism rather than the result, diff
`grep dma-controller /proc/interrupts` across the run: the RX line advances by
about 140 (3 s x 48000 / 1024) with the patch and by exactly 1 without it.

**No stop-time residue.** With this version a capture stop completes its
teardown. A `chan<N> teardown timeout!` at stop, or an RCU stall on CPU 0 soon
after the bridge starts, means the kernel still carries the build `#5`
workaround: rebuild from the current `res/tdm8/patches/` and redeploy.

**`res/spare-sd-pb2/boot/` carries the fix.** The tracked
`Image-7.1.0-tdm8-pb2.gz` there was restaged on 2026-10-07 from a tree with this
patch applied (`sha256 2f8029da5e4a4a56955fd9b12118331a73512dd6196eef7f6d5bbee38f847e33`),
together with the Ethernet Cap device tree (`README.pb2-ethcap.md`). Cards built
from earlier checkouts of that directory carried the pre-patch build #4. Any
rebuild re-stages it in one go:

```sh
./build-tdm8-uac2-pb2.sh image
```

Then `sdimage` as usual.

### Ordinary in-tree device trees

Like the SK trees and unlike the MYIR one, these need no injector: mainline
carries `k3-am62-pocketbeagle2.dts` (present in the `v7.1` tree this BSP pins —
`apply-tdm8-pb2-dts.sh` refuses to run if it is missing), so the TDM8 trees
`#include` it and `dtc` builds them normally. Verify the delta at any time:

```sh
./build-tdm8-uac2-pb2.sh dtb
diff <(dtc -q -I dtb -O dts res/tdm8-pb2/k3-am62-pocketbeagle2.dtb) \
     <(dtc -q -I dtb -O dts res/tdm8-pb2/k3-am62-pocketbeagle2-tdm8.dtb)
```

That delta is: the model string, `&mcasp0` enabled with `#sound-dai-cells`,
`op-mode`, `tdm-slots = 8`, `serial-dir = <1 2 0 0 …>`, `tx/rx-num-evt`, the two
new pinctrl groups, and the two new root nodes (`tdm8-codec`, `sound-tdm8`).
Nothing else.

`#sound-dai-cells` is worth one note: `k3-am62-main.dtsi` does not give the
McASPs that property — on the SK it arrives via `k3-am62x-sk-common.dtsi`.
PocketBeagle 2 has no audio node at all, so the TDM8 trees set it themselves or
`simple-audio-card` cannot reference `&mcasp0`.

### One kernel tree, three boards

`build-tdm8-uac2.sh` (MYIR), `build-tdm8-uac2-sk.sh` (SK) and
`build-tdm8-uac2-pb2.sh` (this one) share `./linux` and each rewrites
`linux/.config` in its `config` step. Run one board's `image` or `all` through to
the end before starting another's. The three kernels do *not* collide once built
— `-tdm8` vs `-tdm8-sk` vs `-tdm8-pb2` keeps `/lib/modules` and the staged
`Image-*.gz` apart, and the staging dirs (`.kstage-tdm8`, `.kstage-tdm8-sk`,
`.kstage-tdm8-pb2`) are separate too.

---

## 4. Build and deploy to a live board

```sh
# one shot: clone v7.1, add the shim + device trees, config, build, deploy
BOARD=pb2 ./build-tdm8-uac2-pb2.sh all

# or step by step
./build-tdm8-uac2-pb2.sh fetch    # clone linux v7.1 (shared with the other boards)
./build-tdm8-uac2-pb2.sh shim     # kl-tdm8-dummy.c -> sound/soc/codecs, and
                                  # res/tdm8/patches/*.patch -> the kernel tree
./build-tdm8-uac2-pb2.sh dts      # both trees -> arch/arm64/boot/dts/ti + Makefile
./build-tdm8-uac2-pb2.sh config   # board .config (or arm64 defconfig) + both fragments
./build-tdm8-uac2-pb2.sh dtb      # both TDM8 trees + the stock tree -> res/tdm8-pb2/
./build-tdm8-uac2-pb2.sh build    # shim again, then Image.gz + modules -> .kstage-tdm8-pb2/
./build-tdm8-uac2-pb2.sh deploy   # scp + rewrite extlinux.conf on the card
./build-tdm8-uac2-pb2.sh probe    # what is this board? rev A1/AM6254 or A0/AM6232
```

`BOARD` is an ssh alias for a live board — over `usb0`, since there is no
Ethernet. `config` pulls the board's own `/proc/config.gz` when it can reach it
and falls back to plain `arm64 defconfig`, which already carries the whole K3
base.

`deploy` finds the FAT boot partition either way: BeagleBoard's own images keep
it mounted at `/boot/firmware`, a Buildroot card from `build-spare-sd.sh` does
not mount it at all (it is partition 1 of whatever disk carries `/`). It keeps
one pristine `extlinux.conf.orig` and **inherits that file's `append` line**, so
a card whose rootfs is not where this script would guess keeps booting. The
fallback cmdline, used only when there is nothing to inherit, is derived from
the stock device tree:

```
console=ttyS2,115200n8 earlycon=ns16550a,mmio32,0x02860000 root=/dev/mmcblk1p2 ro rootfstype=ext4 rootwait net.ifnames=0
```

* `ttyS2` — `k3-am62-pocketbeagle2.dts` has `stdout-path = &main_uart6` and
  `aliases { serial2 = &main_uart6; }`, and `k3-am62-main.dtsi` puts `main_uart6`
  at `0x02860000`. That is the 3-pin JST-SH debug port (Raspberry Pi Debug Probe
  compatible), 115200 8N1.
* `mmcblk1` — and the "1" is the part worth spelling out, because `sdhci1` is
  the *only* MMC host this board enables and it is still not `mmcblk0`. The
  board's `aliases` node carries `mmc1 = &sdhci1`, and `mmc_alloc_host()` takes
  `host->index` straight from `of_alias_get_id(np, "mmc")`, so the index is
  pinned to 1 regardless of probe order. U-Boot's own PocketBeagle 2
  environment agrees — `board/beagle/pocketbeagle2/pocketbeagle2.env` sets
  `mmcdev=1` and `bootpart=1:2` — and BeagleBoard's stock images use
  `mmcblk1` too.

Four labels are written, default `tdm8`:

| Label | Tree |
|---|---|
| `tdm8` | `k3-am62-pocketbeagle2-tdm8.dtb` — 8×8, four wires |
| `tdm8-async` | `k3-am62-pocketbeagle2-tdm8-async.dtb` — 8×8, separate RX clocks |
| `notdm8` | stock `k3-am62-pocketbeagle2.dtb` — same kernel, TDM8 off |
| `ethcap` | `k3-am62-pocketbeagle2-ethcap.dtb` — Ethernet Cap on RGMII2, TDM8 off (`README.pb2-ethcap.md`) |

`notdm8` is the point of this arrangement: it isolates a TDM8 device-tree
problem from a kernel problem without reflashing anything. Set
`PB2_DEFAULT_LABEL=tdm8-async` (the old name `TDM8_DEFAULT_LABEL` still works)
to change what boots when nobody touches the console.

---

## 5. Build it into the image

### Bootloaders — PocketBeagle 2 is **HS-FS**

`README.silcons` is the full story; this is the part that matters here.

PocketBeagle 2's bootloader does **not** come from `u-boot-official/`. `fetch.sh`
clones BeagleBoard's fork — `https://openbeagle.org/beagleboard/u-boot.git`,
branch `v2025.04-rc4-pocketbeagle2` — into `u-boot-pb/`, and `build.sh PB2`
builds `am6232_pocketbeagle2_{r5,a53}_defconfig` into `u-boot-pb/out_bp2/`.

That fork's `arch/arm/dts/k3-am6232-pocketbeagle2-binman.dtsi` declares exactly
two R5 images and **no GP image at all**:

| binman output | Device type | |
|---|---|---|
| `tiboot3-am62x-hs-evm.bin` | HS-SE | needs your own fused key — not this |
| `tiboot3-am62x-hs-fs-evm.bin` | **HS-FS** | `symlink = "tiboot3.bin"`, i.e. the default |

So the chain to flash is:

| On the card | From |
|---|---|
| `tiboot3.bin` | `u-boot-pb/out_bp2/r5/tiboot3-am62x-hs-fs-evm.bin` |
| `tispl.bin` | `u-boot-pb/out_bp2/a53/tispl.bin` — **signed**, not `tispl.bin_unsigned` |
| `u-boot.img` | `u-boot-pb/out_bp2/a53/u-boot.img` — **signed**, not `u-boot.img_unsigned` |

**HS-FS is secure boot with no encryption and no keys of yours**, which is what
makes it the right default. U-Boot's `doc/board/ti/k3.rst` defines it as the
state of a K3 device *before* it has been eFused with customer security keys:
authentication runs exactly as on HS-SE, but with no customer hash fused it
passes for a certificate signed with any key. In practice that means:

* **Signed, never encrypted.** The only encrypted artifact in the chain is TI's
  own `ti-fs-firmware-am62x-hs-fs-enc.bin`, which TI ships that way and the ROM
  decrypts with TI's key. SPL, ATF, OP-TEE, DM and U-Boot go in as plain signed
  images.
* **No key of yours anywhere.** binman signs with U-Boot's in-tree demo key,
  `arch/arm/mach-k3/keys/custMpk.pem`, which `k3-binman.dtsi` copies into the
  build directory for it. The certificate comes out reading `CN=TI Support`.
* **No `TI_SECURE_DEV_PKG`.** That is the legacy signing path; nothing here uses
  it, and nothing needs it.
* **Nothing gets fused.** Going to HS-SE would mean burning your own MPK hash,
  which is one-way and would immediately invalidate the demo key above.

### The console, and why it is on the header

Stock PocketBeagle 2 splits its boot log across two UARTs, which makes bring-up
much harder than it needs to be:

| Stage | UART | Where | tty |
|---|---|---|---|
| R5 SPL → TF-A → OP-TEE | `main_uart0` | **P1.30** TXD / **P1.32** RXD | `ttyS3` |
| A53 SPL → U-Boot → Linux | `main_uart6` | JST-SH 3-pin | `ttyS2` |

The handover is the `INFO: Entry point address = 0x80080000` line. Watch only
the header and the boot appears to die there; watch only the JST-SH and you see
nothing until late — and, before `fix-pb2-uart6-bootph.sh`, nothing at all.

This BSP puts **everything on `main_uart0`**, the header pins, at 115200:

| | Set by |
|---|---|
| R5 SPL, TF-A, OP-TEE | already there — `k3-am6232-r5-pocketbeagle2.dts` |
| A53 SPL, U-Boot | `res/uboot/pb2-console-on-p1.sh` |
| kernel + earlycon | `DEFAULT_APPEND` in `build-tdm8-uac2-pb2.sh` |
| login prompt | `BR2_TARGET_GENERIC_GETTY_PORT="ttyS3"` + `..._BAUDRATE_115200` |

```
console=ttyS3,115200n8 earlycon=ns16550a,mmio32,0x02800000,115200n8 no-console-suspend
```

**One** `console=`, on purpose. Listing `ttyS2` as well makes the kernel log to
both ports — which sounds harmless, but `/dev/console` then fans out to two
devices and the preferred console that owns its *input* side depends on
registration order. One console, one owner. The getty is on `ttyS3` for the same
reason rather than on `console`.

Every rate is explicit — nothing inherits a divisor from whatever the previous
stage left in the register. `no-console-suspend` keeps the console alive across
a suspend; the port is not runtime-idled either way, because `8250_omap` sets an
autosuspend delay of `-1` for a node with no serdev children, deliberately, "to
prevent an unsafe default policy with lossy characters on wake-up".

Wire it up: **P1.30** → your adapter's RX, **P1.32** → its TX, ground on P1.15,
P1.16 or P1.22, 3.3 V only. To go back to the JST-SH, swap the two `console=`
terms, set `earlycon` to `0x02860000`, and run `pb2-console-on-p1.sh off`.

None of these pins clash with the TDM8 link on P2.01 / P1.04 / P1.02 / P2.03, so
console and FPGA cable can stay connected together.

### Verify the chain before it reaches a card

Two failure modes here are both silent, and the second is the easy one to hit.

U-Boot runs binman with `--allow-missing --fake-ext-blobs` (see `cmd_binman` in
its `Makefile`), so a build with no `BINMAN_INDIRS` **succeeds** — it writes
zero-byte stand-ins for the TI firmware into `<out>/binman-fake/` and packages
those. The result is the right size and has a real certificate but contains no
TIFS, and the ROM rejects it before the UART says a word. The TIFS *stub* inside
`tispl.bin` is worse still: binman marks it `optional;`, so it disappears
without even a warning.

`res/uboot/check-bootloader.sh` checks the artifacts rather than the build log,
by searching the images for the actual firmware bytes:

```sh
./build-tdm8-uac2-pb2.sh bootloader      # = check-bootloader.sh on u-boot-pb/out_bp2, VARIANT=hs-fs
```

```
PASS  no binman stand-ins left in either output dir
PASS  tiboot3-am62x-hs-fs-evm.bin present (281518 bytes)
PASS  tiboot3.bin -> tiboot3-am62x-hs-fs-evm.bin
PASS  ti-fs-firmware-am62x-hs-fs-enc.bin embedded at offset 145612
PASS  ROM certificate parses
        signed by: C=US, ST=TX, ..., CN=TI Support, emailAddress=support@ti.com
PASS  tispl.bin present and larger than tispl.bin_unsigned (signed)
PASS  u-boot.img present and larger than u-boot.img_unsigned (signed)
PASS  TIFS stub embedded in tispl.bin at offset 955217
```

Non-zero exit means do not flash. It ends by printing the exact
`build-spare-sd.sh` variables for the chain it just verified.

Then confirm the silicon agrees, on the board and not on the host — the U-Boot
banner prints `SoC:   AM62X SR1.0 HS-FS`, and if it will not get that far,
`res/parse_uart_boot_socid.py` decodes `DeviceType` straight out of the ROM's
UART-boot SoC ID dump.

> **Known rough edge in `build.sh`.** The `PB2` case sets `UB_R5_PATH`/
> `UB_A53_PATH` under `$UBOOT_DIR_PB`, but the build steps below the `case` still
> `cd $UBOOT_DIR` (`u-boot-official/`). Since `am6232_pocketbeagle2_*_defconfig`
> does not exist in `u-boot-official`, the build fails loudly rather than
> producing a wrong binary — but it does need the `cd` to follow the board
> before `./build.sh PB2` works. Left alone here on purpose: `build.sh` has
> uncommitted changes in this tree.

If you would rather not touch the bootloader at all, don't: `deploy` replaces
only the kernel, the device trees and `extlinux.conf`, so the chain that came on
the card keeps running.

### The full compile order

The numbered procedure, from a fresh clone to a card that captures. The order is
not arbitrary: **step 5 must come after step 4**, because `post-build.sh` bakes
`.kstage-tdm8-pb2/lib/modules` into the rootfs as it builds it. Everything else
is a dependency of the step below it. Each step says what it needs, what it
produces and how you know it worked. Steps 1, 2, 3, 6 and the `dd` in 7 are only
for building a whole card; to put a new kernel on a board that already boots, do
0, 4 and `deploy` (after step 7).

#### 0. Host prerequisites

`README.prereq.md` has the full list. On Arch:

```sh
sudo pacman -S --needed aarch64-linux-gnu-gcc arm-none-linux-gnueabihf-gcc \
    dtc bc flex bison swig openssl python mtools dosfstools e2fsprogs util-linux
```

Two `arm` toolchains are in play: `aarch64-linux-gnu-` for the A53 side and
`arm-none-linux-gnueabihf-` for the R5 wakeup-domain SPL. `build.sh` hardcodes
both prefixes.

*Needs:* nothing but the host. *Produces:* nothing.
*Success:*

```sh
which aarch64-linux-gnu-gcc arm-none-linux-gnueabihf-gcc dtc bc python3 \
      mformat mkfs.ext4
grep -qw loop /proc/devices && echo "step 6 will work"
```

#### 1. Sources

`./fetch.sh` clones the lot. If the tree is already populated, the one it is
usually missing is BeagleBoard's U-Boot fork — no other board needs it:

```sh
git clone https://openbeagle.org/beagleboard/u-boot.git \
    -b v2025.04-rc4-pocketbeagle2 u-boot-pb
```

**Why not `u-boot-official`?** Because mainline has no PocketBeagle 2 board
support. Checked at `v2026.07`, `v2026.10-rc4` and `master`: no
`*_pocketbeagle2_*_defconfig`, no `k3-am62*-pocketbeagle2-binman.dtsi`, no
`TARGET_..._POCKETBEAGLE2`. Randolph Sapp's `k3-am62-pocketbeagle2: add board
support` series was at **PATCHv5 in August 2026** and still in review. When it
lands, this board collapses into `u-boot-official` like the SK and MYIR — note
upstream names it `am62_pocketbeagle2_*_defconfig`, not the fork's
`am6232_pocketbeagle2_*`, so `build.sh`'s `PB2_*_CONFIG` values change with it.

That pin is also **old**: `v2025.04-rc4`. BeagleBoard have since moved to a
second repo, `github.com/beagleboard/u-boot-pocketbeagle2`, whose newest tag is
`v2026.01-am62-pocketbeagle2-11.02.18` — and their own Debian images ship from
that line. Bumping to it would drop the binman patch below, since upstream fixed
that in v2025.10. Not done here: the pinned tag is what this BSP has been built
and documented against, and moving it is a change to make deliberately.

*Needs:* network access and `git`. *Produces:* `ti-linux-firmware/`,
`optee_os/`, `u-boot-pb/`, `trusted-firmware-a/` and, next to the BSP,
`../buildroot`. `linux/` is not cloned here: step 4's `fetch` phase does it, on
its own, into `linux/`.
*Success:* those directories exist and are non-empty.

#### 2. Bootloader — TFA, R5 SPL, OP-TEE, A53 U-Boot

```sh
./build.sh PB2
```

In order: TF-A `bl31.bin` → R5 U-Boot (`tiboot3-am62x-hs-fs-evm.bin`) → OP-TEE
`tee-raw.bin` → A53 U-Boot (`tispl.bin`, `u-boot.img`), all into
`u-boot-pb/out_bp2/{r5,a53}/`. `BINMAN_INDIRS` points at `ti-linux-firmware/` in
both U-Boot invocations — that is what keeps the fake-blob trap above from
firing.

Before building, `build.sh` patches the U-Boot tree the board selected. All of
these are idempotent and all no-op on a tree that does not need them.

Host-compatibility, run for every board:

| | For |
|---|---|
| `res/uboot/fix-pylibfdt-swig.sh` | SWIG ≥ 4.3, which dropped the Python 2 macros U-Boot's vendored pylibfdt still uses |
| `res/uboot/fix-binman-pkg-resources.sh` | setuptools ≥ 81, which removed `pkg_resources`; binman ≤ v2025.07 imports it. See `README.prereq.md` §3.2 |

PocketBeagle 2 only, run when the argument is `PB2`:

| | For |
|---|---|
| `res/uboot/fix-pb2-uart6-bootph.sh` | The A53 SPL's console **pinmux group** has no `bootph-*`, only the `&main_uart6` node does, so `fdtgrep` drops it from the SPL device tree and the SPL prints into a UART whose pads were never muxed |
| `res/uboot/fix-pb2-kernel-comp.sh` | `booti` refuses a gzipped `Image` without `kernel_comp_addr_r` / `kernel_comp_size`, which the board env does not set |
| `res/uboot/pb2-console-on-p1.sh` | Puts the A53 stages on `main_uart0` so the whole boot log is on one wire. `on` by default; `PB2_CONSOLE_ON_P1=off ./build.sh PB2` keeps the stock split |

`build.sh` ends by checking that `tiboot3*.bin`, `tispl.bin` and `u-boot.img`
actually exist, and exits non-zero if not — binman failures do not abort the
make, so without that check a broken build reports success.

*Needs:* both toolchains from step 0, `u-boot-pb/`, `ti-linux-firmware/`,
`optee_os/` and `trusted-firmware-a/` from step 1.
*Produces:* `u-boot-pb/out_bp2/r5/tiboot3-am62x-hs-fs-evm.bin`,
`u-boot-pb/out_bp2/a53/tispl.bin`, `u-boot-pb/out_bp2/a53/u-boot.img`.
*Success:* exit 0 and those three files present.

#### 3. Prove the bootloader before it reaches a card

```sh
./build-tdm8-uac2-pb2.sh bootloader
```

Non-zero exit means do not flash. See the two sections above for what it checks
and why the build itself cannot be trusted to have failed.

*Needs:* step 2. *Produces:* nothing, it only reads.
*Success:* exit 0, and the variant it names is `hs-fs`.

#### 4. Kernel, device trees, modules

```sh
./build-tdm8-uac2-pb2.sh image
```

`fetch → shim → dts → config → dtb → build → stage`, ending with `Image.gz`, the
three DTBs and an `extlinux.conf` staged in `res/spare-sd-pb2/boot/`, and modules
in `.kstage-tdm8-pb2/`. Pass `JOBS=` to override `nproc`.

**This is where every out-of-tree kernel change is staged, and there is no
other step that does it.** The `shim` phase runs
`res/tdm8/apply-tdm8-kernel.sh`, which copies `res/tdm8/kl-tdm8-dummy.c` into
`linux/sound/soc/codecs/` with its `Kconfig`/`Makefile` hooks *and* applies
every `res/tdm8/patches/*.patch` in lexical order, the BCDMA cyclic-RX fix
among them (section 3). The `dts` phase does the same for the two PocketBeagle 2
device trees. All of it is idempotent, so a re-clone of `linux/` or a kernel
bump costs nothing, and a patch that no longer applies stops the build by name
instead of half-patching the tree. Nothing here is a manual step and nothing is
committed into the `linux` submodule.

Every phase of this script that compiles a kernel runs `shim` first, `build`
included, so `./build-tdm8-uac2-pb2.sh build` on its own cannot produce an
unpatched `Image.gz` either.

All three TDM8 boards share `./linux` and each rewrites `linux/.config` in its
`config` phase, so **this step discards the SK's or MYIR's kernel config**. Run
one board through to the end before starting another.

*Needs:* the aarch64 toolchain, `dtc`, `bc`, `python3`; a network for the first
`fetch`. A live board on `BOARD=` is optional: `config` pulls its
`/proc/config.gz` when it can and falls back to `arm64 defconfig`.
*Produces:* `linux/arch/arm64/boot/Image.gz`, `res/tdm8-pb2/*.dtb`,
`res/spare-sd-pb2/boot/{Image-7.1.0-tdm8-pb2.gz,ti/*.dtb,extlinux/extlinux.conf}`,
`.kstage-tdm8-pb2/lib/modules/7.1.0-tdm8-pb2/`.
*Success:* exit 0; the run prints `patch: applied ...` or
`patch: ... already applied` for each patch, then `staged into
res/spare-sd-pb2/boot:`. The `build` phase also `ls -l`s the codec module, so a
missing `snd-soc-kl-tdm8-dummy.ko` fails the step rather than passing quietly.

#### 5. Rootfs

One Buildroot checkout, three boards: `.config` and `output/` are per build
directory, so building in `../buildroot` itself overwrites the MYIR ones. Give
PocketBeagle 2 its own output directory, the way the SK gets `../buildroot-sk`:

```sh
make -C ../buildroot O=$PWD/../buildroot-pb2 \
     BR2_EXTERNAL=$PWD/br2-external bb_pocketbeagle2_avb_defconfig
make -C ../buildroot O=$PWD/../buildroot-pb2
```

Images land in `../buildroot-pb2/images/` — note that is `<O>/images/`, not
`<O>/output/images/`, which is what step 6's `ROOTTAR` has to point at. Re-run
the defconfig line even on an existing tree. Then check the overlay and the
modules actually made it in:

```sh
tar tzf ../buildroot-pb2/images/rootfs.tar.gz |
  grep -cE 'lib/modules/7\.1\.0-tdm8-pb2/.*(kl-tdm8-dummy|davinci-mcasp|simple-card)|usr/sbin/tdm8-uac2\.sh|etc/tdm8/tdm8\.env|usr/bin/alsaloop'
# expect 6 or more
```

*Needs:* step 4 finished, because `post-build.sh` copies
`.kstage-tdm8-pb2/lib/modules` in as Buildroot assembles the image.
*Produces:* `../buildroot-pb2/images/rootfs.tar.gz`.
*Success:* the `grep -c` above prints 6 or more.

#### 6. The microSD image

```sh
./build-tdm8-uac2-pb2.sh sdimage [out.img]
```

Needs `sudo` for `losetup`/`mount`/`mkfs`. Default output is
`res/spare-sd-pb2/pocketbeagle2-tdm8.img`; `ROOTTAR=` overrides the rootfs.

It wraps `build-spare-sd.sh`, whose every default points at the **MYIR GP**
chain, and checks each input exists before starting — naming what is missing
rather than reporting a default path you never asked for. Equivalent by hand:

```sh
BOOTSRC=res/spare-sd-pb2/boot \
UBOUT=u-boot-pb/out_bp2 \
R5=u-boot-pb/out_bp2/r5/tiboot3-am62x-hs-fs-evm.bin \
TISPL=u-boot-pb/out_bp2/a53/tispl.bin \
UB=u-boot-pb/out_bp2/a53/u-boot.img \
KIMG_NAME=Image-7.1.0-tdm8-pb2.gz \
ROOTTAR=../buildroot-pb2/images/rootfs.tar.gz \
  ./build-spare-sd.sh res/spare-sd-pb2/pocketbeagle2-tdm8.img
```

Don't. Seven assignments across seven continuation lines is one dropped line
away from a card built out of another board's bootloader, and `TISPL`/`UB` sit
in the same directory as the `*_unsigned` twins that must not go on HS-FS. If
one assignment goes missing the failure names a path you never typed, e.g.
`missing input: .../res/spare-sd/boot/Image-....gz` when only `BOOTSRC` is
lost — that `res/spare-sd/` is the MYIR default reasserting itself.

*Needs:* steps 2, 4 and 5, and `sudo`, `mtools`, `dosfstools`, `e2fsprogs`,
`util-linux` with a loadable `loop` module (`README.prereq.md` section 5).
*Produces:* `res/spare-sd-pb2/pocketbeagle2-tdm8.img`, a sparse 1.6 G MBR image.
*Success:* exit 0; the script prints the apparent and on-disk sizes, and lists
what it put in each partition. Anything missing is named before it starts.

#### 7. Flash, boot and prove the capture path

```sh
sudo dd if=res/spare-sd-pb2/pocketbeagle2-tdm8.img of=/dev/sdX bs=4M \
        conv=fsync status=progress
sync
```

`/dev/sdX` is the **card**, not a partition; find it with `lsblk`. Copying the
image around first needs `cp --sparse=always` or `rsync -S`, or the holes turn
into 1.4 GB of real zeros.

Then boot the card, pick the `tdm8` label (it is the default), and on the
board:

```sh
uname -r                                   # 7.1.0-tdm8-pb2
cat /proc/asound/cards                     # 0 TDM8, 1 UAC2Gadget

arecord -D hw:0,0 -c8 -f S32_LE -r48000 -d3 /tmp/c.wav
ls -l /tmp/c.wav
```

*Needs:* a card reader, or an already-booting board plus `deploy` (below).
*Success:* `arecord` exits 0 and `/tmp/c.wav` is exactly **4608044** bytes. A
short file, or `read error: Input/output error` after one period, means the
kernel on the card predates the BCDMA cyclic-RX patch (section 3) - that card was
built from a `res/spare-sd-pb2/boot/` that step 4 never refreshed.

#### Skipping the bootloader entirely

Steps 1, 2, 3, 6 and the `dd` in 7 are only needed if you are building a whole
card. To put a new kernel on a board that already boots, do steps 0 and 4, then:

```sh
BOARD=pb2 ./build-tdm8-uac2-pb2.sh deploy
```

which replaces `Image`, the DTBs and `extlinux.conf` over ssh and leaves the
bootloader chain that is already on the card alone.

The card comes out MBR p1 = FAT32 (`tiboot3.bin`, `tispl.bin`, `u-boot.img`,
`Image-*.gz`, `ti/*.dtb`, `extlinux/`) and p2 = ext4 rootfs. The A/B card
(`build-spare-sd-ab.sh`) and the RAUC bundle are **not** ported to this board;
`res/ab/boot.cmd`, the MYIR `fw_env.config` offsets and `etc/rauc/system.conf`
are all MYIR-specific.

**First boot fills the card.** The image is 1.6 G whatever the card size.
`/etc/init.d/S05growrootfs` moves the end of p2 to the end of the card
(`sfdisk -N 2`, keeping its start), tells the running kernel (`partx -u`) and
grows the mounted ext4 online (`resize2fs`), all on the first boot and with no
reboot. Expect a few extra seconds on that boot, and
`growrootfs: done: <size> on /, <free> free` on the console. It finds the root partition
from `/proc/self/mountinfo` and only grows the last partition on the card. It
writes `/etc/growrootfs.done` once every step has succeeded, so an interrupted
first boot finishes on the next one. To grow again after copying the card to a
bigger one:

```sh
rm /etc/growrootfs.done && /etc/init.d/S05growrootfs start
```

### Verify, then flash

The image is **sparse**: `truncate -s` creates the full extent but only written
parts get blocks, so `du -h` reports ~190M for a 1.6G image. `ls -lh` and
`du -h --apparent-size` give the real size, which is what `dd` writes and what
the card has to hold. `build-spare-sd.sh` prints both. Copy it with
`cp --sparse=always` or `rsync -S` or the holes become 1.4 GB of real zeros.

```sh
IMG=res/spare-sd-pb2/pocketbeagle2-tdm8.img
mdir -i $IMG@@1048576 ::
mtype -i $IMG@@1048576 ::/extlinux/extlinux.conf | head -5     # default tdm8

dtc -q -I dtb -O dts res/tdm8-pb2/k3-am62-pocketbeagle2-tdm8.dtb |
  sed -n '/audio-controller@2b00000 {/,/^\t\t};/p'
# -> status = "okay"; tdm-slots = <0x08>; serial-dir = <0x01 0x02 0x00 0x00 ...>

strings linux/arch/arm64/boot/Image | grep -m1 'Linux version'   # 7.1.0-tdm8-pb2
ls .kstage-tdm8-pb2/lib/modules/                                 # 7.1.0-tdm8-pb2

sudo dd if=$IMG of=/dev/sdX bs=4M conv=fsync status=progress
```

PocketBeagle 2 has no boot-mode switches (SRM §3.2.1): it boots from the microSD
by default and, with no card present, falls back to **USB DFU**. Holding the
**USER** button at power-up still tries the microSD first but falls back to
**UART** instead — which is the recovery path if you ever write a card the ROM
will not read. Console is the 3-pin JST-SH port, `ttyS2`, 115200 8N1.

---

## 6. Bring-up

```sh
./build-tdm8-uac2-pb2.sh probe             # 4 cores online => rev A1 / AM6254
```

On the board:

```sh
uname -r                                   # 7.1.0-tdm8-pb2
cat /proc/device-tree/model                # ... + Kebag-Logic TDM8 8x8 (McASP0 on P1/P2)

# start the FPGA clocking BCLK/FSYNC FIRST - McASP is the slave and will not
# advance a single frame without them
/etc/init.d/S99usb_gadgets start
/usr/sbin/tdm8-uac2.sh status
```

What "good" looks like:

```sh
cat /proc/asound/cards
#  0 [TDM8       ]: simple-card - TDM8
#  1 [UAC2Gadget ]: UAC2_Gadget - UAC2_Gadget

aplay   -D hw:TDM8,0 --dump-hw-params /dev/zero     2>&1 | grep -E 'CHANNELS|RATE|FORMAT'
arecord -D hw:TDM8,0 --dump-hw-params -d1 /dev/null 2>&1 | grep -E 'CHANNELS|RATE|FORMAT'
dmesg | grep -iE 'mcasp|tdm8|simple-card|uac2'
```

Both `aplay` and `arecord` must work here — that is the whole point of this
board. If one of them fails you are on the wrong tree or the wrong wire.

Proving the wire, and the host side, are exactly as in `README.tdm8-uac2.md` §6:

```sh
arecord -D hw:TDM8,0 -c 8 -f S32_LE -r 48000 -d 2 /tmp/tdm8.wav
lsusb -v -d 1d6b:0104 | grep -E 'bNrChannels|bSubframeSize|tSamFreq|bmAttributes'
```

A capture that ends after one period with `read error: Input/output error` is
not a wiring fault: that is the BCDMA cyclic-RX bug, and the kernel is missing
`res/tdm8/patches/0001-dmaengine-ti-k3-udma-count-bcdma-cyclic-rx-static-tr-z-in-bursts.patch` (section 3).
The 3 s form there, which must return 4608044 bytes, settles it either way.

A one-slot rotation in the captured channels means `dsp_a` ↔ `dsp_b`; an all-zero
capture with healthy framing means FSYNC polarity
(`simple-audio-card,frame-inversion`).

---

## 7. Tunables

`/etc/tdm8/tdm8.env` is the MYIR table (`README.tdm8-uac2.md` §8); PocketBeagle 2
ships `TDM8_ENABLE=yes` and differs in four entries:

| Variable | PB2 default | Notes |
|---|---|---|
| `TDM8_ENABLE` | `yes` | `S99usb_gadgets` builds the TDM8 gadget + bridge at boot |
| `TDM8_DIRECTION` | `duplex` | both trees are full duplex; there is no capture-only fallback on this board |
| `TDM8_ECM` | `yes` | **not optional here** — no Ethernet, so `usb0` is the only network path |
| `TDM8_USB0_IP` | `192.168.7.12/24` | `.10` is MYIR, `.11` is the SK, so all three can share one host |
| `TDM8_ECM_DEV_ADDR` | `6a:65:62:6f:6f:20` | ditto, distinct from the other two gadgets |

---

## 8. Troubleshooting

Everything in `README.tdm8-uac2.md` §9 applies. Board-specific additions:

| Symptom | Cause |
|---|---|
| `arecord` dies after exactly one period with `read error: Input/output error` | the kernel predates `res/tdm8/patches/0001-dmaengine-ti-k3-udma-count-bcdma-cyclic-rx-static-tr-z-in-bursts.patch` (section 3). Re-run `./build-tdm8-uac2-pb2.sh image` and redeploy; a card built from a pre-2026-10-07 `res/spare-sd-pb2/boot/` carries the old build #4 |
| `chan1 teardown timeout!` + `unhandled rx event. rxstat: 0x00000104` at every capture stop, or an RCU stall on CPU 0 seconds after the bridge starts | the kernel carries the build `#5` workaround (EOP taken off RX). Rebuild from the current `res/tdm8/patches/` and redeploy (section 3) |
| No `TDM8` card at all | the `notdm8` label was selected — check `cat /proc/device-tree/model` and the booted `fdt` |
| `TDM8` card exists, every open blocks | FPGA not driving BCLK/FSYNC. McASP is the slave; it will not advance a frame on its own |
| Capture all zeros, playback silent, framing healthy | FSYNC polarity, or `dsp_a` vs `dsp_b` — see §6 |
| Playback works, capture is zeros | `MCASP0_AXR1` is on **P2.03**, not P2.01 — the two are adjacent on the same header row |
| Random bit errors on BCLK | two SoC balls sit on each of P1.02/P1.04/P2.01/P2.03. Keep the stubs short and put the series resistor at the FPGA end |
| One direction dies after minutes | `TDM8_SYNC=none` drifts to an xrun; use `samplerate` |
| async tree: BCLK looks like UART framing | firmware is driving P1.08 (`UART1_TXD`). Scope it; disabling `&main_uart1` only stops Linux. Use the four-wire tree |
| No `UAC2Gadget` card | `cat /sys/kernel/config/usb_gadget/g/UDC` should read `31000000.usb`. If `/sys/class/udc` is empty the Type-C port is not in device role |
| Board unreachable over ssh after a deploy | `usb0` is the only link. `TDM8_ECM=no`, a UDC that did not bind, or a host that renumbered the ECM interface — fall back to the JST-SH console |
| Board does not reach its rootfs | `MMC_SDHCI_AM654`, `REGULATOR_GPIO`, `MFD_TPS65219` and `GPIO_DAVINCI` must be `=y`; `config` checks all four |
| Silent right after `Entry point address = 0x80080000` | the console moved to the other UART. Either watch the JST-SH, or build with `pb2-console-on-p1.sh on` (the default) and watch P1.30 |
| `kernel_comp_addr_r or kernel_comp_size is not provided!`, every label `err=-14` | `fix-pb2-kernel-comp.sh` was not applied — `booti` cannot unpack a gzipped `Image` without them |
| `missing input: .../res/spare-sd/boot/Image-....gz` | `res/spare-sd/` is the **MYIR** default: a variable did not reach `build-spare-sd.sh`. Use `./build-tdm8-uac2-pb2.sh sdimage` instead of calling it by hand |
| Card is completely dead — no UART output at all | wrong or empty `tiboot3.bin`. The ROM rejects it before the console exists. Run `./build-tdm8-uac2-pb2.sh bootloader` |
| U-Boot SPL starts, then nothing | signed/unsigned mismatch further up: `tispl.bin_unsigned` or `u-boot.img_unsigned` on HS-FS silicon |
| Banner says `GP` or `HS-SE`, not `HS-FS` | not a stock PocketBeagle 2 rev A1. Set `UB_VARIANT` to match and re-check; on HS-SE the in-tree demo key will not authenticate |
| `root=/dev/mmcblk1p2` does not exist | the card's own `extlinux.conf` said something else and there was no `.orig` to inherit from. Fix the `append` line on the console |
| Only two cores in `/proc/cpuinfo` | rev A0 / AM6232 board. Everything here still works; the device tree describes four cores on both revisions |
