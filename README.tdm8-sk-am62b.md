# TDM8 → USB Audio Class 2.0 gadget on the TI SK-AM62B-P1

The same function as `README.tdm8-uac2.md` does for the MYIR AM6254: stream the
Kebag-Logic AVB **FPGA**'s TDM8 link into a **USB Audio Class 2.0 device** that
the AM62x presents on its USB-C port, so a host sees one multichannel UAC2
interface and the FPGA's render/capture lanes can be validated against it.

```
  FPGA  ──8 slots──▶ McASP AXR rx ──▶ hw:TDM8 capture  ──alsaloop──▶ gadget playback ──USB IN ──▶ host
  FPGA  ◀─8 slots─── McASP AXR tx ◀── hw:TDM8 playback ◀─alsaloop─── gadget capture  ◀─USB OUT─── host
        ◀─BCLK 12.288 MHz ── FSYNC 48 kHz ──  (FPGA is bit-clock and frame master)
```

Everything above the device tree — the codec shim, the kernel fragment, the
gadget builder, the `alsaloop` bridge, `/etc/tdm8/tdm8.env` — is shared with the
MYIR board. Read `README.tdm8-uac2.md` first: this file only documents what is
different, and §2, §7 and §10 there apply verbatim.

---

## 1. The connector problem on this board, and the two answers

On the MYIR MYD-YM62X, McASP1's four TDM wires come out on the J11 GPMC header
and nothing else wants them. The SK-AM62B-P1 is not so kind.

### McASP1 — the right McASP, the wrong connector

TI's own `main_mcasp1_pins_default` in `k3-am62x-sk-common.dtsi` muxes **exactly
the four pads the MYIR TDM8 link uses, in the same directions**:

| Signal | Pad | Mux | Ball | SoC dir |
|---|---|---|---|---|
| `MCASP1_ACLKX` — BCLK | `0x0090` | 2 | M24 | **in** |
| `MCASP1_AFSX` — FSYNC | `0x0098` | 2 | U23 | **in** |
| `MCASP1_AXR0` — SoC→FPGA | `0x008c` | 2 | L25 | **out** |
| `MCASP1_AXR2` — FPGA→SoC | `0x0084` | 2 | L23 | **in** |

so the device-tree delta is a near-copy of the MYIR one. But on the SK those
four nets are the **on-board audio bus**. They run through a FET switch and a
buffer to the TLV320AIC3106 codec (3.5 mm TRRS jack), the SII9022 HDMI bridge's
I²S input, and the M.2 E-key connector's BT PCM pins. Mainline names the three
control lines on the TCA6424 expander (`exp1`, `0x22` on `main_i2c1`):

```
line 18  MCASP1_FET_EN
line 19  MCASP1_BUF_BT_EN
line 20  MCASP1_FET_SEL
```

Of the four, only **`MCASP1_AXR2` reaches J3 (pin 15)** and `MCASP1_AXR1` reaches
J3-31. `ACLKX`, `AFSX` and `AXR0` do not come out on any header, so the FPGA
cable has to meet this bus at the **M.2 E-key connector** (BT PCM pins, behind
`MCASP1_BUF_BT_EN`) or at the buffer itself.

### McASP0 — the right connector, one pin short

The 40-pin user expansion header **J3** (RPi-compatible; `SPRUJ40` Table 2-25)
carries most of McASP0:

| J3 pin | Net name | Ball | Pad | McASP0 function (mux 0) |
|---|---|---|---|---|
| 11 | `EXP_SPI2_CS1` | B20 | `0x01a4` | `MCASP0_ACLKX` |
| 12 | `EXP_SPI2_CS0` | E19 | `0x01ac` | `MCASP0_AFSR` |
| 33 | `EXP_EHRPWM1_B` | E18 | `0x01a0` | `MCASP0_AXR0` |
| 35 | — | A19 | `0x0198` | `MCASP0_AXR2` |
| 36 | `EXP_EHRPWM1_A` | B18 | `0x019c` | `MCASP0_AXR1` |
| 38 | — | B19 | `0x0194` | `MCASP0_AXR3` |
| 39 | — | A20 | `0x01b0` | `MCASP0_ACLKR` |

**`MCASP0_AFSX` (pad `0x01a8`, ball D20) is not on J3.** That single omission is
what splits this board in two, because of how the McASP is wired internally:

* the **transmit** section is clocked by `ACLKX` + `AFSX` — always, in both
  synchronous and `ti,async-mode`;
* the **receive** section uses `ACLKX`/`AFSX` when `ASYNC = 0`, or `ACLKR`/`AFSR`
  when `ASYNC = 1`.

There is no register path that feeds the transmit section from `ACLKR`/`AFSR`
(see `davinci_mcasp_hw_params()` → `mcasp_common_hw_param()` in
`sound/soc/ti/davinci-mcasp.c`). So on J3 the FPGA can drive a receive frame and
nothing else: **capture only**, whichever side is master.

### So: two device trees, pick one at the boot menu

| | `k3-am625-sk-tdm8.dtb` | `k3-am625-sk-tdm8-j3.dtb` |
|---|---|---|
| McASP | **McASP1** | **McASP0** |
| Directions | 8 in **and** 8 out | 8 in only |
| `TDM8_DIRECTION` | `duplex` | `capture` |
| UAC2 interface | 8-in / 8-out | 8-in / 0-out |
| Where the cable goes | M.2 E-key BT PCM pins / the audio buffer | J3, a plain ribbon |
| On-board `AM62x-SKEVM` card | disabled (McASP1 is ours) | still there |
| Gotcha | the TCA6424 FET lines have to isolate the codec | J3-39/J3-12 are the TIFS trace UART |

`build-tdm8-uac2-sk.sh stage` puts both on the card plus `notdm8` (the same
kernel on the stock SK device tree), with `tdm8` as the default and
`prompt 1 / timeout 30`, so the serial console always has a way out.

### Wiring — McASP1 variant (8×8)

Four wires plus ground, same contract as the MYIR link:

| Wire | SoC signal | FPGA side | Direction |
|------|------------|-----------|-----------|
| 1 | `MCASP1_ACLKX` | bit clock out, 12.288 MHz | FPGA → SoC |
| 2 | `MCASP1_AFSX` | frame sync out, 48 kHz | FPGA → SoC |
| 3 | `MCASP1_AXR2` | serial data out, 8 slots | FPGA → SoC |
| 4 | `MCASP1_AXR0` | serial data in, 8 slots | SoC → FPGA |
| 5,6 | `GND` | ground | — |

Before connecting, take the on-board loads off the bus, or the codec and the
FPGA will drive into each other. The expander lines are named, so find them
rather than counting:

```sh
gpiofind MCASP1_FET_EN MCASP1_BUF_BT_EN MCASP1_FET_SEL
# e.g. gpiochip2 18 / gpiochip2 19 / gpiochip2 20

# read the as-booted state first, then try the other one and re-check with a scope
gpioget $(gpiofind MCASP1_FET_EN)
gpioset $(gpiofind MCASP1_FET_EN)=0
```

**The polarity of these three is not in the device tree and this BSP does not
guess it.** Once you have proved a working combination on the bench, make it
survive a reboot by uncommenting the `gpio-hog` block at the bottom of
`res/tdm8-sk/k3-am625-sk-tdm8.dts` with the levels you found.

### Wiring — McASP0 / J3 variant (8 in)

| Wire | J3 pin | SoC signal | FPGA side |
|------|--------|------------|-----------|
| 1 | **39** | `MCASP0_ACLKR` | bit clock out, 12.288 MHz |
| 2 | **12** | `MCASP0_AFSR` | frame sync out, 48 kHz |
| 3 | **33** | `MCASP0_AXR0` | serial data out, 8 slots |
| 4 | 6, 9, 14, 20, 25, 30, 34 | `GND` | ground |

Two on-board owners are released by `k3-am625-sk-tdm8-j3.dts`:

* **`&epwm1` → disabled.** `EHRPWM1_B` is the same pad as `MCASP0_AXR0`.
* **`&main_uart1` → disabled.** J3-39 and J3-12 are `UART1_TXD`/`UART1_RXD`, and
  mainline marks that node `status = "reserved"` with the comment *"Main UART1
  is used by TIFS firmware"*. Disabling the node stops **Linux** claiming the
  pads, but it cannot stop the **TIFS firmware** — if the TIFS build on your
  board emits trace there it keeps driving J3-39 as an output, against the
  FPGA's BCLK. Scope J3-39 for UART traffic before you plug the cable in, and
  put a 22–33 Ω series resistor in the BCLK wire either way.

Other J3 electrical notes: the header is a **3.3 V** bank, `VCC3V3_EXP`
(J3-1/17) and `VCC5V0_EXP` (J3-2/4) are switched by the expander's
`EXP_PS_3V3_En` / `EXP_PS_5V0_En`, and J3-27/28 are `I2C0` with the HAT ID
EEPROM on them — leave those alone. Keep the ribbon ≤ 15 cm and use the ground
pins next to the signals, not only the ends.

### USB side

The gadget binds **usb0** (`31000000.usb`), the DRD behind the **USB-C port**,
which on this board is also the power inlet. Both TDM8 device trees set
`dr_mode = "peripheral"` so the gadget binds deterministically; PD is negotiated
by the TPS6598x independently, so the board still powers up normally. Drop that
override to go back to mainline's role switching. There is no USB3 PHY: the
gadget runs USB 2.0 high-speed, and 8 ch × 4 B × 48 kHz = 192 B per 125 µs
microframe against 1024 B available.

Sources for the pin tables: `SPRUJ40` *EVM User's Guide: SK-AM62, SK-AM62B,
SK-AM62B-P1* Table 2-25, cross-checked against the `P11/P26/P33/P36 of J3`
comments and `main_mcasp1_pins_default` in mainline `k3-am62x-sk-common.dtsi`,
and against `doc/board/ti/am62x_sk.rst` in U-Boot.

---

## 2. Clock and framing contract

Identical to the MYIR link — see `README.tdm8-uac2.md` §2. In short: the FPGA
owns the oscillator and is bit-clock and frame master, BCLK = 48 000 × 8 × 32 =
12.288 MHz, FSYNC = 48 kHz, `dsp_a`, 8 × 32-bit slots with 24 valid bits
left-justified (`S32_LE`). `TDM8_RATE` only tells software what to expect;
nothing on the AM62x side can force a rate onto a link it does not clock.

The McASP1 tree runs **synchronous** (no `ti,async-mode`) because `MCASP1_ACLKR`
/`AFSR` are not wired. The J3 tree runs **asynchronous**, which is the whole
reason it can receive at all.

---

## 3. What this adds to the BSP

| File | Role |
|---|---|
| `res/tdm8-sk/k3-am625-sk-tdm8.dts` | McASP1 8×8 device tree. `#include`s mainline `k3-am625-sk.dts`. |
| `res/tdm8-sk/k3-am625-sk-tdm8-j3.dts` | McASP0-on-J3 capture-only device tree. |
| `res/tdm8-sk/apply-tdm8-sk-dts.sh` | Idempotently copies both into `linux/arch/arm64/boot/dts/ti/` and registers them in that `Makefile`. |
| `res/kl-sk-am62b.config` | Kernel fragment: the SK delta on top of `arm64 defconfig` + `res/kl-tdm8-uac2.config`. Pins `CONFIG_LOCALVERSION="-tdm8-sk"`. |
| `build-tdm8-uac2-sk.sh` | `fetch → shim → dts → config → dtb → build → {deploy \| stage}`. |
| `br2-external/board/common/rootfs-overlay/` | The board-independent half of the rootfs overlay, **moved out of `board/myir-am62x/`**: `usr/sbin/tdm8-uac2.sh`, `etc/init.d/S99usb_gadgets`, `root/setup_gadgets.sh`, `root/remove_usb.sh`. Both boards' defconfigs now list two overlay dirs. |
| `br2-external/board/ti-sk-am62b/` | SK `post-build.sh` (default `KL_MODDIR=.kstage-tdm8-sk/lib/modules`) plus this board's `etc/tdm8/tdm8.env`, `etc/network/interfaces` and `root/.ssh/authorized_keys`. |
| `br2-external/configs/ti_sk_am62b_avb_defconfig` | Buildroot config. Mirrors the MYIR one minus RAUC/`libubootenv` (no A/B layout on this board yet) plus `libgpiod` for the `MCASP1_FET_*` lines. |
| `build.sh` | New `SK` target: `am62x_evm_{r5,a53}_defconfig` → `u-boot-official/out_sk/`. |
| `build-spare-sd.sh` | Gained `BOOTSRC` / `UBOUT` / `R5_NAME` / `R5` / `TISPL` / `UB` overrides. MYIR defaults unchanged. |

Shared with the MYIR board, unchanged: `res/tdm8/kl-tdm8-dummy.c`,
`res/tdm8/apply-tdm8-kernel.sh`, `res/kl-tdm8-uac2.config`.

### No device-tree injector here

The MYIR board boots a vendor blob with no source and no `__symbols__`, which is
why `res/tdm8/mk-tdm8-dtb.py` exists. Mainline carries `k3-am625-sk.dts`, so the
SK trees are ordinary sources that `#include` it and are built by `dtc` in the
normal way. Verify the delta at any time:

```sh
./build-tdm8-uac2-sk.sh dtb
diff <(dtc -q -I dtb -O dts res/tdm8-sk/k3-am625-sk.dtb) \
     <(dtc -q -I dtb -O dts res/tdm8-sk/k3-am625-sk-tdm8.dtb)
```

For the McASP1 tree that is: the model string, `dr_mode`, `tdm-slots 2 → 8`,
`tx/rx-num-evt`, `status = "disabled"` on `codec_audio`, and the two new root
nodes. `serial-dir` and the pinmux need no change at all — TI already has
`1 0 2 0` and the four pads.

### One kernel tree, two boards

`build-tdm8-uac2.sh` (MYIR) and `build-tdm8-uac2-sk.sh` (SK) share `./linux` and
each rewrites `linux/.config` in its `config` step. Run one board's `image` or
`all` through to the end before starting the other's; switching boards means a
full reconfigure and rebuild either way. The two kernels do *not* collide once
built — `-tdm8` vs `-tdm8-sk` keeps `/lib/modules` and the staged `Image-*.gz`
apart.

---

## 4. Build and deploy to a live board

```sh
# one shot: clone v7.1, add the shim + device trees, config, build, deploy
BOARD=sk ./build-tdm8-uac2-sk.sh all

# or step by step
./build-tdm8-uac2-sk.sh shim     # kl-tdm8-dummy.c into linux/ + Kconfig/Makefile
./build-tdm8-uac2-sk.sh dts      # both SK .dts into linux/ + ti/Makefile
./build-tdm8-uac2-sk.sh config   # arm64 defconfig + the two fragments
./build-tdm8-uac2-sk.sh dtb      # tdm8, tdm8-j3 and stock SK dtbs -> res/tdm8-sk/
./build-tdm8-uac2-sk.sh build    # Image.gz + modules -> .kstage-tdm8-sk/
./build-tdm8-uac2-sk.sh deploy   # scp + a new extlinux menu, originals kept
```

Env: `KVER` (default `v7.1`), `BOARD` (ssh alias, default `sk`), `JOBS`,
`CROSS_COMPILE` (default `aarch64-linux-gnu-`).

`config` pulls `/proc/config.gz` from `$BOARD` if it answers and otherwise starts
from plain `arm64 defconfig`, which already carries the whole K3 base. It then
asserts that `SND_SOC_KL_TDM8_DUMMY`, `SND_SIMPLE_CARD`, `SND_SOC_DAVINCI_MCASP`,
`USB_F_UAC2`, `USB_CONFIGFS` and `USB_DWC3_AM62` survived `olddefconfig`, and
separately that `MMC_SDHCI_AM654`, `GPIO_PCA953X` and `REGULATOR_GPIO` are `=y` —
the microSD's `vmmc` regulator hangs off the TCA6424 expander, so as modules the
board cannot reach its own rootfs.

`deploy` writes a **new** `extlinux.conf` with `tdm8`, `tdm8-j3` and `notdm8` and
keeps the original as `extlinux.conf.orig`.

This path leaves the rootfs alone. For `alsaloop`, `amixer`, `libsamplerate` and
`gpioset`, build the rootfs as in §5.

---

## 5. Build it into the image

Same ordering rules as `README.tdm8-uac2.md` §5 — the numbered steps there apply,
with these substitutions:

| MYIR | SK-AM62B-P1 |
|---|---|
| `./build.sh MYIR` | `./build.sh SK` |
| `./build-tdm8-uac2.sh image` | `./build-tdm8-uac2-sk.sh image` |
| `.kstage-tdm8/lib/modules` | `.kstage-tdm8-sk/lib/modules` |
| `res/spare-sd/boot` | `res/spare-sd-sk/boot` |
| `myir_am62x_avb_defconfig` | `ti_sk_am62b_avb_defconfig` |
| `tiboot3-am62x-gp-myc-am62x.bin` | `tiboot3-am62x-hs-fs-evm.bin` |
| kernel release `7.1.0-tdm8` | `7.1.0-tdm8-sk` |

### Step 1 — bootloaders

```sh
./build.sh SK
```

Produces `u-boot-official/out_sk/r5/tiboot3-am62x-{gp,hs-fs,hs}-evm.bin`,
`out_sk/a53/tispl.bin` and `out_sk/a53/u-boot.img`. **Use the `hs-fs` image** —
SK-AM62B-P1 ships HS-FS silicon, and binman symlinks `tiboot3.bin` to exactly
that one.

### Step 2 — kernel, device trees, modules

```sh
./build-tdm8-uac2-sk.sh image
```

`image` is `fetch → shim → dts → config → dtb → build → stage`; it touches no
board. Outputs: `linux/arch/arm64/boot/Image.gz`, `res/tdm8-sk/*.dtb`,
`.kstage-tdm8-sk/lib/modules/7.1.0-tdm8-sk/`, and a staged
`res/spare-sd-sk/boot/` with all three extlinux labels.

### Step 3 — which variant starts at boot

`br2-external/board/ti-sk-am62b/rootfs-overlay/etc/tdm8/tdm8.env` ships
`TDM8_ENABLE=yes` and `TDM8_DIRECTION=duplex`, matching the default `tdm8`
(McASP1) label. If you are cabling to J3 instead, set `TDM8_DIRECTION=capture`
**before** step 4 so the gadget advertises 8-in / 0-out.

### Step 4 — rootfs

```sh
cd ../buildroot
make BR2_EXTERNAL=$(pwd)/../ti-sitara-am65x-bsp/br2-external ti_sk_am62b_avb_defconfig
make
cd ../ti-sitara-am65x-bsp
```

Re-run the defconfig line even on an existing tree — and note that **both**
boards' defconfigs changed in this commit (they now list two overlay
directories), so an old `.config` will silently miss `tdm8-uac2.sh`.

```sh
tar tzf ../buildroot/output/images/rootfs.tar.gz |
  grep -cE 'lib/modules/7\.1\.0-tdm8-sk/.*(kl-tdm8-dummy|davinci-mcasp|simple-card)|usr/sbin/tdm8-uac2\.sh|etc/tdm8/tdm8\.env|usr/bin/alsaloop'
# expect 6 or more
```

### Step 5 — the microSD image

```sh
BOOTSRC=res/spare-sd-sk/boot \
UBOUT=u-boot-official/out_sk \
R5_NAME=tiboot3-am62x-hs-fs-evm.bin \
KIMG_NAME=Image-7.1.0-tdm8-sk.gz \
ROOTTAR=../buildroot/output/images/rootfs.tar.gz \
  ./build-spare-sd.sh res/spare-sd-sk/sk-am62b-tdm8.img
```

`ROOTTAR` is not optional — without it the script falls back to the MYIR board
snapshot. `KIMG_NAME` is only the presence check; the whole staged
`res/spare-sd-sk/boot/` tree is copied, so the card boots `tdm8` by default with
`tdm8-j3` and `notdm8` on the serial console.

The card comes out as MBR p1 = FAT32 (`tiboot3.bin`, `tispl.bin`, `u-boot.img`,
`Image-*.gz`, `ti/*.dtb`, `extlinux/`) and p2 = ext4 rootfs. That is what the
SK's U-Boot wants: `boot_targets` starts at `mmc1`, and the extlinux bootmeth
looks for `extlinux/extlinux.conf` under both `/` and `/boot/`.

The A/B card (`build-spare-sd-ab.sh`) and the RAUC bundle are **not** ported to
this board — `res/ab/boot.cmd`, the MYIR `fw_env.config` offsets and
`etc/rauc/system.conf` are all MYIR-specific. The SK gets the single-slot image
only.

### Step 6 — verify, then flash

```sh
IMG=res/spare-sd-sk/sk-am62b-tdm8.img
mdir -i $IMG@@1048576 ::
mdir -i $IMG@@1048576 ::/ti
mtype -i $IMG@@1048576 ::/extlinux/extlinux.conf | head -5     # default tdm8

dtc -q -I dtb -O dts res/tdm8-sk/k3-am625-sk-tdm8.dtb |
  sed -n '/audio-controller@2b10000 {/,/^\t\t};/p'
# -> status = "okay"; tdm-slots = <0x08>; serial-dir = <0x01 0x00 0x02 0x00 ...>

strings linux/arch/arm64/boot/Image | grep -m1 'Linux version'   # 7.1.0-tdm8-sk
ls .kstage-tdm8-sk/lib/modules/                                  # 7.1.0-tdm8-sk

sudo dd if=$IMG of=/dev/sdX bs=4M conv=fsync status=progress
```

Set the boot-mode switches to **SD** before powering up — `SW2: 01000000`,
`SW1: 11000010`, ON = 1 (U-Boot `doc/board/ti/am62x_sk.rst`). Console is the
XDS110 USB debug port, `ttyS2`, 115200 8N1.

---

## 6. Bring-up

```sh
uname -r                                   # 7.1.0-tdm8-sk
cat /proc/device-tree/model                # ... + Kebag-Logic TDM8 ...

# start the FPGA clocking BCLK/FSYNC FIRST - McASP is the slave and will not
# advance a single frame without them
/etc/init.d/S99usb_gadgets start
/usr/sbin/tdm8-uac2.sh status
```

What "good" looks like on the **McASP1** tree:

```sh
cat /proc/asound/cards
#  0 [TDM8       ]: simple-card - TDM8
#  1 [UAC2Gadget ]: UAC2_Gadget - UAC2_Gadget

aplay   -D hw:TDM8,0 --dump-hw-params /dev/zero  2>&1 | grep -E 'CHANNELS|RATE|FORMAT'
arecord -D hw:TDM8,0 --dump-hw-params -d1 /dev/null 2>&1 | grep -E 'CHANNELS|RATE|FORMAT'
dmesg | grep -iE 'mcasp|tdm8|simple-card|uac2'
```

On the **J3** tree the stock card is still registered, so expect
`0 [AM62x-SKEVM]`, `1 [TDM8]`, `2 [UAC2Gadget]` — which is why
`/etc/tdm8/tdm8.env` looks cards up by **id**, not index. `aplay -D hw:TDM8,0`
is expected to fail there: that device tree has no TX serializer.

Proving the wire, and the host side, are exactly as in `README.tdm8-uac2.md` §6:

```sh
arecord -D hw:TDM8,0 -c 8 -f S32_LE -r 48000 -d 2 /tmp/tdm8.wav
lsusb -v -d 1d6b:0104 | grep -E 'bNrChannels|bSubframeSize|tSamFreq|bmAttributes'
```

A one-slot rotation in the captured channels means `dsp_a` ↔ `dsp_b`; an all-zero
capture with healthy framing means FSYNC polarity (`simple-audio-card,frame-inversion`).

---

## 7. Tunables

`/etc/tdm8/tdm8.env` is the MYIR table (`README.tdm8-uac2.md` §8) plus one entry,
and the SK ships `TDM8_ENABLE=yes`:

| Variable | SK default | Notes |
|---|---|---|
| `TDM8_ENABLE` | `yes` | `S99usb_gadgets` builds the TDM8 gadget + bridge at boot |
| `TDM8_DIRECTION` | `duplex` | `duplex` \| `capture` \| `playback`. `capture` = the J3 device tree: UAC2 becomes 8-in / 0-out and only the `to-host` alsaloop leg runs |
| `TDM8_USB0_IP` | `192.168.7.11/24` | `.10` is the MYIR board, so the two can share a host |
| `TDM8_ECM_DEV_ADDR` | `6a:65:62:6f:6f:10` | ditto, distinct from the MYIR gadget |

---

## 8. Troubleshooting

Everything in `README.tdm8-uac2.md` §9 applies. Board-specific additions:

| Symptom | Cause |
|---|---|
| No `TDM8` card, `AM62x-SKEVM` present | the `notdm8` label was selected, or you are on the McASP1 tree and `codec_audio` won the race — check `cat /proc/device-tree/model` and the booted `fdt` |
| `TDM8` card exists, every open blocks | FPGA not driving BCLK/FSYNC — or, on the McASP1 tree, the FET switch still has the codec on the bus. `gpioget $(gpiofind MCASP1_FET_EN)` |
| Capture is all zeros on the McASP1 tree | the FET is routing McASP1 to the codec or HDMI bridge instead of your cable |
| J3 tree: BCLK looks like UART framing | the TIFS firmware is driving J3-39 (`UART1_TXD`). Scope it; disabling `&main_uart1` only stops Linux |
| J3 tree: `aplay` fails with `Invalid argument` | expected — no TX serializer. Set `TDM8_DIRECTION=capture` |
| Host sees 8-in / 8-out on the J3 tree | `TDM8_DIRECTION` is still `duplex`; the OUT endpoint has nowhere to go |
| No `UAC2Gadget` card | `cat /sys/kernel/config/usb_gadget/g/UDC` should read `31000000.usb`. If `/sys/class/udc` is empty, the Type-C port is not in device role — check `dr_mode = "peripheral"` made it into the booted tree |
| Board does not reach its rootfs | `GPIO_PCA953X`/`REGULATOR_GPIO`/`MMC_SDHCI_AM654` must be `=y`; the microSD's `vmmc` is gated by the TCA6424 expander |
| Boots the stock TI image instead | boot-mode switches, or U-Boot found `/boot/extlinux/extlinux.conf` on another partition first |

---

## 9. Relationship to the MYIR board

The two are independent targets in one tree: separate bootloader output dirs
(`out_myir` / `out_sk`), separate Buildroot defconfigs, separate kernel releases,
separate staged boot dirs and images. They share the `linux/` submodule, the
codec shim, the TDM8 kernel fragment and the whole userspace bridge, so a fix to
`tdm8-uac2.sh` or `kl-tdm8-dummy.c` lands on both.

Running the same FPGA image against both boards is the point: the MYIR link is
the one `README.tdm8-validation.md` logs, and the SK gives a second, independent
McASP receiver to separate an FPGA problem from a board problem.
