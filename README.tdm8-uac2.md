# TDM8 (8 in / 8 out) → USB Audio Class 2.0 gadget on the MYIR AM6254

Stream an 8-channel-in / 8-channel-out TDM8 link into a **USB Audio Class 2.0
device** that the AM62x presents on its USB-C OTG port. The TDM8 peer is the
Kebag-Logic AVB **FPGA**; it has no control bus and owns the audio oscillator.

```
  FPGA  ──8 slots──▶ MCASP1_AXR2 ──▶ hw:TDM8 capture  ──alsaloop──▶ gadget playback ──USB IN ──▶ host
  FPGA  ◀─8 slots─── MCASP1_AXR0 ◀── hw:TDM8 playback ◀─alsaloop─── gadget capture  ◀─USB OUT─── host
        ◀─BCLK 12.288 MHz ── FSYNC 48 kHz ──  (FPGA is bit-clock and frame master)
```

Read `README.board-facts.md` first for the board, and `README.install-kernel.md`
for the kernel build/deploy conventions this reuses.

---

## 1. Which I/O to use — **McASP1 on the J11 expansion header**

J11 is the 2×15 "8 bit GPMC" header on the MYD-YM62X baseboard (Foxconn
`13201215CNG4M80T01`, 2.54 mm pitch — mates with any 30-way IDC socket). Every
GPMC0 *control* pin on it doubles as a **MCASP1** signal at **pad mux mode 2**,
which is exactly the four-wire TDM8 link plus power and ground on one connector.

### J11 as wired on the baseboard

```
                J11 — 2x15, 2.54 mm, Foxconn 13201215CNG4M80T01
         (numbering per the schematic symbol: odd left, even right;
          confirm which end is pin 1 on the board silkscreen before plugging in)

   TDM8 use              net                pin       pin  net
   --------------------  -----------------  ---     ---   --------------------
   3V3 rail              VDD_3V3              1 [o|o]  2   VDD_5V
 * GND                   GND                  3 [o|o]  4   GND
   spare: AXR3           GPMC0_CLK            5 [o|o]  6   GPMC0_AD0/BOOTMODE00
                         GPMC0_CSn0           7 [o|o]  8   GPMC0_AD1/BOOTMODE01
 * AXR0   SoC  -> FPGA   GPMC0_WEn            9 [o|o] 10   GPMC0_AD2/BOOTMODE02
   spare: AXR1           GPMC0_OEn_REn       11 [o|o] 12   GPMC0_AD3/BOOTMODE03
 * GND                   GND                 13 [o|o] 14   GND
 * AFSX   FPGA -> SoC    GPMC0_WAIT0         15 [o|o] 16   GPMC0_AD4/BOOTMODE04
 * AXR2   FPGA -> SoC    GPMC0_ADVn_ALE      17 [o|o] 18   GPMC0_AD5/BOOTMODE05
                         GPMC0_DIR           19 [o|o] 20   GPMC0_AD6/BOOTMODE06
 * ACLKX  FPGA -> SoC    GPMC0_BE0n_CLE      21 [o|o] 22   GPMC0_AD7/BOOTMODE07
 * GND                   GND                 23 [o|o] 24   GND
                         NC                  25 [o|o] 26   NC
                         NC                  27 [o|o] 28   NC
                         NC                  29 [o|o] 30   NC

   *  = the six pins to wire.
   Pins 6,8,10,12,16,18,20,22 are BOOTMODE00..07 straps — leave them unconnected.
```

### The cable

Six conductors. Nothing else on J11 needs to be touched.

| Wire | J11 pin | SoC signal | FPGA side | Direction |
|------|---------|------------|-----------|-----------|
| 1 | **21** | `MCASP1_ACLKX` | bit clock out, 12.288 MHz | FPGA → SoC |
| 2 | **15** | `MCASP1_AFSX` | frame sync out, 48 kHz | FPGA → SoC |
| 3 | **17** | `MCASP1_AXR2` | serial data out, 8 slots | FPGA → SoC |
| 4 | **9**  | `MCASP1_AXR0` | serial data in, 8 slots | SoC → FPGA |
| 5 | **13** | `GND` | ground | — |
| 6 | **23** | `GND` | ground | — |

J11-1 (+3V3) and J11-2 (+5V) are baseboard rails; only draw from them if the
FPGA carrier's budget is known — they are not fused on the MYD side.

### Full pad mapping

| Signal (SoC side)        | J11 | SoM pin | AM62 ball | Pad offset | Mux | Pad name         | SoC dir |
|--------------------------|-----|---------|-----------|------------|-----|------------------|---------|
| `MCASP1_ACLKX` — BCLK    | 21  | L34     | M24       | `0x0090`   | 2   | `GPMC0_BE0n_CLE` | **in**  |
| `MCASP1_AFSX`  — FSYNC   | 15  | L35     | U23       | `0x0098`   | 2   | `GPMC0_WAIT0`    | **in**  |
| `MCASP1_AXR0`  — SoC→FPGA| 9   | L42     | L25       | `0x008c`   | 2   | `GPMC0_WEn`      | **out** |
| `MCASP1_AXR2`  — FPGA→SoC| 17  | L48     | L23       | `0x0084`   | 2   | `GPMC0_ADVn_ALE` | **in**  |
| `GND`                    | 3, 4, 13, 14, 23, 24 | — | — | — | — | — | — |
| `+3V3` rail              | 1   | —       | —         | —          | —   | —                | out     |
| `+5V` rail               | 2   | —       | —         | —          | —   | —                | out     |

Two more MCASP1 serializers are on the same header if you ever want a second
TDM pair (16×16 over two lanes):

| Spare        | J11 | SoM pin | Ball | Pad offset | Mux | Pad name        |
|--------------|-----|---------|------|------------|-----|-----------------|
| `MCASP1_AXR1`| 11  | L40     | L24  | `0x0088`   | 2   | `GPMC0_OEn_REn` |
| `MCASP1_AXR3`| 5   | 120     | P25  | `0x007c`   | 2   | `GPMC0_CLK`     |

Sources: `docs/HardwareFiles/MYC-YM62X-PinList-V1.0.xlsx` (SoM pin ↔ ball ↔ mux),
`docs/HardwareFiles/SCH&PCB/MYB-Y62X-V10.pdf` sheet 12 (J11 net list), and the
AM62x pad-offset comments in mainline `k3-am625-beagleplay.dts`. The ball names
agree across all three.

### Electrical notes — read before wiring

* **3V3 I/O.** Every GPMC0 pad on J11 is a 3.3 V bank. Level-match the FPGA side;
  do not drive 1V8-only pins at 3V3 or vice-versa.
* **`JP2` must be OPEN.** `SoC_GPMC0_WAIT0` (our FSYNC, J11-15) is also wired
  through `R571` 22 Ω and jumper **JP2** to `LVDS0_TP_PEN` on the LVDS connector.
  The schematic note reads *"JP2 & JP3 Connect: LVDS / JP2 & JP3 Disconnect:
  GPMC"*. Leave JP2 disconnected, or FSYNC carries a stub to the LVDS header.
  (`JP3`/`GPMC0_DIR` is the same story but we do not use that pin.)
* **None of the four TDM8 pins is a boot strap.** `GPMC0_CLK`, `WEn`, `OEn_REn`,
  `WAIT0`, `ADVn_ALE` and `BE0n_CLE` have an empty Bootstrap column in the MYIR
  pin list. By contrast **`GPMC0_AD0..AD7` on J11 (pins 6,8,10,12,16,18,20,22) are
  `BOOTMODE00..07`** — never let the FPGA drive those while the board is in reset.
* **Signal integrity.** BCLK is 12.288 MHz on a pin header. Keep the cable short
  (≤ 15 cm ribbon), put a 22–33 Ω series resistor at each driver, and use the
  ground pins at J11-13/14 next to the signals rather than only the end pins.

### Why not McASP0 or McASP2

* **McASP0** is consumed on the baseboard. `SoC_MCASP0_ACLKX/AFSX/AXR0/AXR1`
  (SoM L13–L16) land on a **74CBTLV3257PW** 4-bit 2:1 bus mux driven by
  `AUDIO_SEL`/`JP1`, which routes them either to the SII9022 HDMI I2S or to the
  SGTL5000 codec. Its other pins (`MCASP0_AFSR/ACLKR/AXR2/AXR3`, SoM L7/L8/L11/L12)
  are wired as UART1. Nothing reaches a header.
* **McASP2** has eight data lanes on J11 (`GPMC0_AD0..AD7`, mux 3) but **no clock
  pair there**: `MCASP2_ACLKX/AFSX` exist only on `GPMC0_AD12/AD13` (not on J11),
  on the RGMII2 pins (that is the AVB Ethernet port) and on `UART0_RTSn/CTSn`
  (used on the baseboard for `LVDS1_I2C_SDA/SCL`). And those eight lanes are the
  BOOTMODE straps.

### USB side

The gadget binds the **USB-C OTG port (J15/J10)**, UDC `31000000.usb`. This board
has **no USB3 PHY** — the gadget runs USB 2.0 high-speed, which is plenty:

```
8 ch × 4 B × 48 000 Hz = 1.536 MB/s per direction = 192 B per 125 µs microframe
```

against 1024 B/microframe for a single high-speed isochronous endpoint (~19 %).
Even 8 ch × 32 bit × 96 kHz fits.

---

## 2. The clock and framing contract with the FPGA

The FPGA is **bit-clock and frame master**. That is deliberate: no
`AUDIO_EXT_REFCLK` / MCLK pad reaches J11, so the AM62x cannot hand the FPGA a
master clock, and a local oscillator on the FPGA gives the better jitter anyway.

| | |
|---|---|
| MCLK        | FPGA-local (typically 24.576 MHz). **Not wired to the SoC.** |
| BCLK (ACLKX)| `rate × slots × slot_width` = 48 000 × 8 × 32 = **12.288 MHz**, FPGA → SoC |
| FSYNC (AFSX)| **48 kHz**, FPGA → SoC |
| Framing     | `dsp_a`: FSYNC is a **1-BCLK-wide pulse**; slot 0 starts **one BCLK after** the FSYNC edge; MSB first |
| Slots       | 8 × 32 bit; 24 valid bits left-justified in each slot (`S32_LE`) |

If the FPGA emits slot 0 *on* the FSYNC edge instead, switch the device tree to
`simple-audio-card,format = "dsp_b"`. If FSYNC is active low, add
`simple-audio-card,frame-inversion`. Both live in
`res/tdm8/k3-am625x-myd-6254-tdm8.dtsi`.

**The rate is whatever the FPGA clocks.** McASP is a slave here; nothing in ALSA
can force 48 kHz onto the wire. `TDM8_RATE` in `/etc/tdm8/tdm8.env` only tells
the software what to expect — it must match the FPGA build.

McASP runs in **synchronous** mode (no `ti,async-mode`), because `ACLKR`/`AFSR`
have no pad on J11: receive is clocked from the transmit clock domain. A
side-effect is that both directions must use the same rate, channel count and
sample width — which is what the bridge wants anyway.

---

## 3. What this adds to the BSP

| File | Role |
|---|---|
| `res/tdm8/kl-tdm8-dummy.c` | ASoC codec-side DAI shim for a control-less 8-slot TDM peer. Mainline has no generic multichannel equivalent of `linux,spdif-dit`, and ASoC will not build a DAI link without a codec. Compatible `kebag-logic,tdm8-dummy`. |
| `res/tdm8/apply-tdm8-kernel.sh` | Idempotently copies the shim into `linux/sound/soc/codecs/` and registers it in `Kconfig` + `Makefile`. Survives a re-clone of `linux/`. |
| `res/tdm8/k3-am625x-myd-6254-tdm8.dtsi` | The device-tree delta: McASP1 pinmux, McASP1 node, the codec node and the `simple-audio-card`. |
| `res/tdm8/mk-tdm8-dtb.py` | Injects that delta into the **vendor blob** and emits `k3-am625x-myd-6254-tdm8.dtb`. The vendor blob is never modified. |
| `res/kl-tdm8-uac2.config` | Kernel config fragment. |
| `build-tdm8-uac2.sh` | The recipe: `fetch → shim → config → dtb → build → {deploy \| stage}`. |
| `br2-external/.../usr/sbin/tdm8-uac2.sh` | Builds the 8×8 UAC2 (+ECM) gadget and runs the alsaloop bridge. |
| `br2-external/.../etc/tdm8/tdm8.env` | Runtime tunables, including the `TDM8_ENABLE` master switch. |
| `br2-external/.../etc/init.d/S99usb_gadgets` | Now dispatches on `TDM8_ENABLE`; `no` keeps the exact gadget the safe-update/RAUC flow has always built. |
| `br2-external/.../post-build.sh` | Also chmods `tdm8-uac2.sh` and bakes `$KL_MODDIR` (default `.kstage-tdm8/lib/modules`) into the rootfs image. |
| `br2-external/configs/myir_am62x_avb_defconfig` | Adds `alsaloop`, `amixer`, `libsamplerate`. |
| `build-spare-sd.sh`, `build-spare-sd-ab.sh`, `build-rauc-bundle.sh` | Gained `KIMG_NAME` / `ROOTTAR` / `KIMG` / `DTB` / `DTB_NAME` overrides so the TDM8 kernel and device tree can go into an image. Defaults are unchanged. |

### Why a device-tree *injector* and not a `.dtbo` overlay

Mainline has no MYIR DTS; the board boots a vendor binary, and that binary
carries **no `__symbols__`**. A `.dtbo` therefore cannot reference the McASP node
by label, and the raw-phandle workaround breaks because `fdtoverlay` renumbers
overlay-local phandles. So `mk-tdm8-dtb.py` decompiles the vendor blob, injects
the sections at their absolute node paths with phandles allocated above the
blob's highest (`0x58` → `0x59..0x5c`), and recompiles. The result is a **new**
file; the original `k3-am625x-myd-6254.dtb` stays on the boot partition as the
fallback entry's `fdt`.

Verify the delta at any time:

```sh
./build-tdm8-uac2.sh dtb
diff <(dtc -q -I dtb -O dts res/spare-sd/boot/ti/k3-am625x-myd-6254.dtb) \
     <(dtc -q -I dtb -O dts res/tdm8/k3-am625x-myd-6254-tdm8.dtb)
```

It should be exactly 40 lines: the pinmux phandle, the McASP1 properties, and the
two new root nodes.

### The kernel config delta is tiny

The board's running config already has `SND_SOC_DAVINCI_MCASP`, `SND_SIMPLE_CARD`,
`USB_DWC3_AM62`, `USB_F_UAC2`, `USB_U_AUDIO`, `USB_CONFIGFS_F_UAC2` and
`USB_CONFIGFS_ECM` as modules. The only genuinely new symbol is
`CONFIG_SND_SOC_KL_TDM8_DUMMY`. The fragment also pins
`CONFIG_LOCALVERSION="-tdm8"` and turns `LOCALVERSION_AUTO` off so `uname -r`
reads `7.1.0-tdm8` and this kernel's `/lib/modules` never collides with the
`7.1.0-dirty` tree already on the board.

---

## 4. Build and deploy to a live board

```sh
# one shot: clone v7.1, add the shim, config, build the DTB, build, deploy
./build-tdm8-uac2.sh all

# or step by step
./build-tdm8-uac2.sh shim      # copy kl-tdm8-dummy.c into linux/ + Kconfig/Makefile
./build-tdm8-uac2.sh config    # board's /proc/config.gz + res/kl-tdm8-uac2.config
./build-tdm8-uac2.sh dtb       # vendor blob -> k3-am625x-myd-6254-tdm8.dtb
./build-tdm8-uac2.sh build     # Image.gz + modules -> .kstage-tdm8/
./build-tdm8-uac2.sh deploy    # scp + a new extlinux label, originals kept
```

Env: `KVER` (default `v7.1`), `BOARD` (ssh alias, default `board`), `JOBS`,
`CROSS_COMPILE` (default `aarch64-linux-gnu-`), `BASE_DTB`.

`BASE_DTB` defaults to `res/spare-sd/boot/ti/k3-am625x-myd-6254.dtb`, which is
**gitignored** - on a fresh clone, pull the blob the board actually boots first:

```sh
ssh board 'mount -o ro /dev/mmcblk1p1 /mnt/bootp; \
  cat /mnt/bootp/ti/k3-am625x-myd-6254.dtb; umount /mnt/bootp' > /tmp/base.dtb
BASE_DTB=/tmp/base.dtb ./build-tdm8-uac2.sh dtb
```

`config` pulls `/proc/config.gz` from the board and falls back to
`res/board-running.config` when the board is unreachable. It then asserts that
`SND_SOC_KL_TDM8_DUMMY`, `SND_SIMPLE_CARD`, `SND_SOC_DAVINCI_MCASP`,
`USB_F_UAC2` and `USB_CONFIGFS` survived `olddefconfig`.

`deploy` writes a **new** `extlinux.conf` with three labels and never removes a
working one:

```
default tdm8      -> Image-7.1.0-tdm8.gz + /ti/k3-am625x-myd-6254-tdm8.dtb
label   rebuilt   -> same kernel, original DTB (TDM8 off, everything else identical)
label   linux     -> the original Image.gz + original DTB
```

`prompt 1 / timeout 30` means the serial console can still pick `rebuilt` or
`linux` if the TDM8 tree misbehaves — there is no JTAG on this board.

This path leaves the rootfs alone. To also get `alsaloop`, `amixer` and
`libsamplerate` onto the board, rebuild the rootfs as in §5 and reflash, or
`scp` the binaries across for a quick test.

---

## 5. Build it into the image

`deploy` in §4 is for iterating on a board you can already `ssh` into. To produce
an **image** that comes up with TDM8 already working, build the kernel and DTB
first, then pick the image flavour.

### Step 1 — kernel, DTB and boot artifacts

```sh
./build-tdm8-uac2.sh image     # = fetch + shim + config + dtb + build + stage
```

`image` is `all` with `stage` instead of `deploy`: it touches no board. It leaves

| Artifact | Path |
|---|---|
| kernel (compressed) | `linux/arch/arm64/boot/Image.gz` |
| kernel (raw, for the A/B `booti` path) | `linux/arch/arm64/boot/Image` |
| device tree | `res/tdm8/k3-am625x-myd-6254-tdm8.dtb` |
| modules (`7.1.0-tdm8`) | `.kstage-tdm8/lib/modules/` |
| staged boot partition | `res/spare-sd/boot/` — `Image-7.1.0-tdm8.gz`, `ti/k3-am625x-myd-6254-tdm8.dtb`, `extlinux/extlinux.conf` |

`stage` is **non-destructive**: kernels, DTBs and extlinux labels already staged
in `res/spare-sd/boot/` are kept as fallback entries, and re-running replaces our
own `tdm8`/`notdm8` labels instead of stacking duplicates. The generated
`extlinux.conf` defaults to `tdm8` and adds a `notdm8` label — same kernel,
stock device tree — which separates a device-tree problem from a kernel problem
without reflashing.

### Step 2 — turn TDM8 on by default in the image

The rootfs ships with the feature **off** so an image built from this tree
behaves exactly as before. To have it come up automatically:

```sh
sed -i 's/^TDM8_ENABLE=no/TDM8_ENABLE=yes/' \
  br2-external/board/myir-am62x/rootfs-overlay/etc/tdm8/tdm8.env
```

Leave it `no` to build a dual-purpose image and flip the switch on the board.

### Step 3 — the rootfs image (Buildroot)

```sh
cd ../buildroot
make BR2_EXTERNAL=$(pwd)/../ti-sitara-am65x-bsp/br2-external myir_am62x_avb_defconfig
make
```

The defconfig now pulls in `alsaloop`, `amixer` and `libsamplerate`, and
`post-build.sh` does two things for TDM8:

* `chmod 0755 usr/sbin/tdm8-uac2.sh` (and 0644 on `etc/tdm8/tdm8.env`);
* copies **`$KL_MODDIR`** — default `<bsp>/.kstage-tdm8/lib/modules` — into
  `lib/modules/<release>/`, dropping the dangling `build`/`source` symlinks.

So the produced rootfs already carries the `7.1.0-tdm8` modules; nothing has to
be `scp`'d after flashing. Override with `KL_MODDIR=/path/to/lib/modules` (for
example `.kstage/lib/modules` to bake the *non*-TDM8 kernel's modules instead),
or `KL_MODDIR=none` to skip module installation entirely:

```sh
KL_MODDIR=none make          # rootfs without any kernel modules baked in
```

Outputs: `output/images/rootfs.tar.gz` and `rootfs.ext4`.

### Step 4 — pick an image

**(a) Single-slot spare microSD** — full boot chain + kernel + rootfs, `dd`-able:

```sh
cd ../ti-sitara-am65x-bsp
KIMG_NAME=Image-7.1.0-tdm8.gz \
ROOTTAR=../buildroot/output/images/rootfs.tar.gz \
  ./build-spare-sd.sh res/spare-sd/spare-am62x-tdm8.img
```

`KIMG_NAME` is only the presence check; the whole staged `res/spare-sd/boot/`
tree is copied, so the card boots `tdm8` by default with `notdm8`, `rebuilt` and
`linux` selectable on the serial console.

**(b) A/B microSD** (safe-update P2a — GPT-less MBR, two rootfs slots, U-Boot
bootchooser):

```sh
DTB=res/tdm8/k3-am625x-myd-6254-tdm8.dtb \
MODDIR=.kstage-tdm8/lib/modules \
ROOTTAR=../buildroot/output/images/rootfs.tar.gz \
  ./build-spare-sd-ab.sh res/spare-sd/spare-am62x-ab-tdm8.img
```

`KIMG` defaults to `linux/arch/arm64/boot/Image`, which after step 1 *is* the
TDM8 kernel. The DTB lands in each slot as **`/boot/k3-am625x-myd-6254-71.dtb`**
— that filename is hard-coded in `res/ab/boot.cmd`, so only the *content*
changes; override `DTB_NAME` only if you edit `boot.cmd` to match.

**(c) RAUC bundle** — OTA onto a board already running the A/B layout:

```sh
DTB=res/tdm8/k3-am625x-myd-6254-tdm8.dtb \
MODDIR=.kstage-tdm8/lib/modules \
VERSION=tdm8-1 \
  ./build-rauc-bundle.sh res/rauc/myir-am62x-tdm8.raucb

scp res/rauc/myir-am62x-tdm8.raucb board:/tmp/
ssh board 'rauc install /tmp/myir-am62x-tdm8.raucb && reboot'
```

The bundle carries a whole slot (rootfs + `/boot/Image` + DTB + modules), so the
inactive slot gets the TDM8 kernel and the bootchooser rolls back on its own if
it fails to boot. See `README.safe-update.md`.

Both (b) and (c) derive the module directory name from the Image itself
(`strings … 'Linux version'`). Pinning `CONFIG_LOCALVERSION="-tdm8"` is what makes
that reliable — with the board config's default `LOCALVERSION_AUTO=y` the release
picks up a git hash and `-dirty`, and `modprobe` then fails on a `uname` mismatch.

### Verify before you flash

```sh
# the TDM8 pieces are in the rootfs
tar tzf ../buildroot/output/images/rootfs.tar.gz |
  grep -E 'snd-soc-kl-tdm8-dummy|snd-soc-davinci-mcasp|snd-soc-simple-card|usb_f_uac2|tdm8-uac2.sh|tdm8.env|bin/alsaloop'

# the staged device tree really is the TDM8 one
dtc -q -I dtb -O dts res/spare-sd/boot/ti/k3-am625x-myd-6254-tdm8.dtb |
  sed -n '/audio-controller@2b10000 {/,/^\t\t};/p'
# -> status = "okay"; tdm-slots = <0x08>; serial-dir = <0x01 0x00 0x02 0x00 ...>

# and the kernel is the pinned release the module dir is named after
strings linux/arch/arm64/boot/Image | grep -m1 'Linux version'
# -> Linux version 7.1.0-tdm8 ...
ls .kstage-tdm8/lib/modules/
# -> 7.1.0-tdm8
```

Flash: `sudo dd if=<image>.img of=/dev/sdX bs=4M conv=fsync status=progress`.

---

## 6. Bring-up

```sh
ssh board
uname -r                                   # 7.1.0-tdm8
sed -i 's/^TDM8_ENABLE=no/TDM8_ENABLE=yes/' /etc/tdm8/tdm8.env

# start the FPGA clocking BCLK/FSYNC first - McASP is the slave and will not
# advance a single frame without them
/etc/init.d/S99usb_gadgets start
/usr/sbin/tdm8-uac2.sh status
```

What "good" looks like:

```sh
cat /proc/asound/cards
#  0 [TDM8       ]: simple-card - TDM8
#  1 [UAC2Gadget ]: UAC2_Gadget - UAC2_Gadget

# 8 channels, both directions, on the McASP card
aplay  -D hw:TDM8,0 --dump-hw-params /dev/zero 2>&1 | grep -E 'CHANNELS|RATE|FORMAT'
arecord -D hw:TDM8,0 --dump-hw-params -d1 /dev/null 2>&1 | grep -E 'CHANNELS|RATE|FORMAT'

# the DAI link resolved the way the DT asked
dmesg | grep -iE 'mcasp|tdm8|simple-card|uac2'
```

On the **host** the board enumerates as an 8-in/8-out UAC2 interface:

```sh
lsusb -v -d 1d6b:0104 | grep -E 'bNrChannels|bSubframeSize|tSamFreq|bmAttributes'
arecord -D hw:<board>,0 -c 8 -f S32_LE -r 48000 -d 5 /tmp/eight.wav
```

### Proving the wire before trusting the loop

```sh
# board -> FPGA: 8 channels of silence, just to see BCLK/FSYNC being consumed
aplay -D hw:TDM8,0 -c 8 -f S32_LE -r 48000 -d 5 /dev/zero

# FPGA -> board: capture raw and check the slots are not all zero / not rotated
arecord -D hw:TDM8,0 -c 8 -f S32_LE -r 48000 -d 2 /tmp/tdm8.wav
```

A **one-slot rotation** in the captured channels means the `dsp_a` ↔ `dsp_b`
choice is wrong. All-zero capture with a healthy `rx_packets`-equivalent means
FSYNC polarity — add `simple-audio-card,frame-inversion`.

---

## 7. Clock drift: the one thing that will bite

The FPGA's oscillator and the USB host's clock are independent.

* **host → board (USB OUT)** is handled in hardware-ish: the gadget declares
  `c_sync=async`, so f_uac2 exposes an explicit **feedback endpoint** and the
  host rate-adapts. The feedback value is driven by the ALSA control
  `Capture Pitch 1000000` on the gadget card (nominal 1 000 000):

  ```sh
  amixer -c UAC2Gadget controls | grep Pitch
  amixer -c UAC2Gadget cset name='Capture Pitch 1000000' 1000050   # +50 ppm
  ```

* **board → host (USB IN)** has no feedback endpoint — that direction of USB
  isochronous has none by design; the host simply accepts what we send.

So the residual drift lands on `alsaloop`. `TDM8_SYNC` picks how it is absorbed:

| `TDM8_SYNC`  | Behaviour |
|--------------|-----------|
| `samplerate` | **default.** Asynchronous sample-rate conversion via libsamplerate. Survives indefinitely, costs CPU. |
| `none`       | Straight copy. Lowest CPU and latency, drifts to an xrun eventually — fine for bring-up and short captures. |
| `captshift` / `playshift` | Rate-shift one side instead of resampling. |

Lower `TDM8_LATENCY_US` (default 8000 µs per direction) once it is stable; watch
for `XRUN` in `/usr/sbin/tdm8-uac2.sh status`.

---

## 8. Tunables

All of `/etc/tdm8/tdm8.env`:

| Variable | Default | Notes |
|---|---|---|
| `TDM8_ENABLE` | `no` | `yes` makes `S99usb_gadgets` build the TDM8 gadget + bridge |
| `TDM8_RATE` | `48000` | must match the FPGA's FSYNC |
| `TDM8_CHANNELS` | `8` | sets the UAC2 channel mask (`0xff`) |
| `TDM8_FORMAT` | `S32_LE` | `S32_LE`/`S24_3LE`/`S16_LE`; drives `p_ssize`/`c_ssize` (4/3/2) |
| `TDM8_CARD_ID` | `TDM8` | from `simple-audio-card,name` |
| `TDM8_GADGET_CARD_ID` | `UAC2Gadget` | f_uac2's card name with the `_` stripped by ALSA |
| `TDM8_SYNC` | `samplerate` | see §7 |
| `TDM8_LATENCY_US` | `8000` | `alsaloop -t` |
| `TDM8_WAIT_CARDS` | `20` | seconds the bridge waits for both cards at boot |
| `TDM8_REQ_NUMBER` | `4` | pre-allocated USB requests; 8 ch needs more than f_uac2's default 2 |
| `TDM8_FB_MAX` | `5` | max extra bandwidth for the async feedback endpoint |
| `TDM8_ECM` | `yes` | keep `usb0` (ssh/rauc) in the same composite gadget |
| `TDM8_USB0_IP` | `192.168.7.10/24` | unchanged from the existing setup |
| `TDM8_UDC` | *(auto)* | first entry in `/sys/class/udc` |

`tdm8-uac2.sh` sub-commands: `up`, `down`, `status`, `gadget-up`, `gadget-down`,
`bridge-up`, `bridge-down`.

---

## 9. Troubleshooting

| Symptom | Cause |
|---|---|
| No `TDM8` card in `/proc/asound/cards` | `snd-soc-kl-tdm8-dummy` or `snd-soc-simple-card` not loaded, or the `tdm8` extlinux label was not selected (check `uname -r` = `7.1.0-tdm8` and that `/ti/k3-am625x-myd-6254-tdm8.dtb` is the booted `fdt`) |
| `TDM8` card exists, every open blocks | FPGA is not driving BCLK/FSYNC. McASP is the slave; it cannot self-start. Also check JP2 is open |
| Channels rotated by one slot | `dsp_a` vs `dsp_b` mismatch with the FPGA framing |
| Capture is all zeros | FSYNC polarity → `simple-audio-card,frame-inversion`; or AXR2 not landing on J11-17 |
| No `UAC2Gadget` card | Gadget not bound. `cat /sys/kernel/config/usb_gadget/g/UDC` should read `31000000.usb` |
| Host sees 2 channels, not 8 | An older gadget is still bound — `tdm8-uac2.sh gadget-down`, then `up`. `/root/setup_gadgets.sh` builds a different UAC2 (96 kHz, 3-byte) |
| Steady XRUN growth | clock drift — set `TDM8_SYNC=samplerate`, raise `TDM8_LATENCY_US` |
| `alsaloop: command not found` | rootfs predates `BR2_PACKAGE_ALSA_UTILS_ALSALOOP=y`; rebuild Buildroot |

`dmesg | grep davinci-mcasp` reports the resolved BCLK/FSYNC and slot geometry;
`/proc/asound/card*/pcm*/sub*/status` (also printed by `tdm8-uac2.sh status`)
gives per-stream hw/appl pointers and XRUN counts.

---

## 10. Relationship to the AVB stack

This is a *separate* path from `README.avb-am62x.md`. There, a UAC2 interface is
plugged into a **Type-A host** port and driven by `snd-usb-audio`; here the board
itself **is** the UAC2 device on the OTG port. They can coexist — nothing here
touches `eth1`, the CBS shaper or gPTP. Once the TDM8 card is up, the same 8
channels can be handed to PipeWire/`module-avb` instead of (or as well as) the
gadget; that is a bridge-layer change only, no kernel or device-tree work.
