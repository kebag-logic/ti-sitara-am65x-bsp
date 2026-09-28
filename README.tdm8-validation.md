# TDM8 link bring-up and validation log (AX7101 J11 <-> AM62x McASP1)

Bench validation of the TDM8 link between the ALINX AX7101 (FPGA, TDM master)
and the MYiR AM62x McASP1. Wiring recipe: see the future-board sections of the
milan-fpga issues and `README.tdm8-uac2.md`. This file records what was proven
on 2026-09-16 and what remains, and is the reproducible procedure.

## Roles and pin directions (confirmed against the device tree)

McASP1 (`audio-controller@2b10000`), 8 slots, 32-bit, `dsp_a`, codec = master:
the FPGA drives ACLKX (bit clock) and AFSX (frame sync); the SoC follows.

| J11 | AM62x pad | signal | direction | pinmux |
|-----|-----------|--------|-----------|--------|
| 21  | 0x0090 GPMC0_BE0n_CLE | MCASP1_ACLKX | FPGA -> SoC (BCLK 12.288 MHz) | in, mux2 |
| 15  | 0x0098 GPMC0_WAIT0     | MCASP1_AFSX  | FPGA -> SoC (FSYNC 48 kHz)     | in, mux2 |
| 9   | 0x008c GPMC0_WEn       | MCASP1_AXR0  | SoC -> FPGA (DIN, 8 slots out) | out, mux2 |
| 17  | 0x0084 GPMC0_ADVn_ALE  | MCASP1_AXR2  | FPGA -> SoC (DOUT, 8 slots in) | in, mux2 |

`serial-dir = <1 0 2 0>` (AXR0 TX, AXR2 RX). This matches the FPGA RTL:
`KL_tdm_render_master` (dsp_a, fsync in bit period 0, data from bit period 1,
256-bclk frame) and `KL_tdm_capture_master` (the timing owner driving BCLK/FSYNC).

## FPGA side (the DUT must run the render image)

The render lane ships in milan-fpga at dev >= 0a3a78a9 (VERSION 0x0002_0058).
An older image (0x0002_0057 or earlier) has no TDM render master on J11.

Build (3-seed sweep, best WNS) and flash from a clean milan-fpga checkout:

```sh
cd sw/litex
TAG=tdm8 ./build.sh ax7101 --sweep            # pick best WNS of eppo/asl/eto
# prove the currently installed bitstream, then flash bitstream + AEM (verified):
INSTALLED_BUILD=<current-build> SERIAL=<ax-ftdi-serial> AX_FTDI=<ax-ftdi-serial> \
  ./build.sh flash ax7101:<best-build>
# power-cycle the board (NEVER a JTAG warm reload); confirm on the console:
#   milan_status -> VERSION=00020058 AEM=loaded
```

Note: a clean milan-fpga checkout used as the flash host has no `boards.local.sh`,
so pass `SERIAL=`/`AX_FTDI=` explicitly; `CABLE` defaults to `ft232`.

## Board side (MYiR)

Serial console: /dev/ttyACM0 on the dev box, 115200 8N1, login root.
Kernel `7.1.0-tdm8`. Cards: 0 AM62xSKEVM, 1 TDM8 (McASP1), 2 UAC2Gadget.
`/etc/tdm8/tdm8.env` has TDM8_ENABLE=yes; the composite gadget (uac2.0 + ecm.usb0)
binds to UDC 31000000.usb.

## Validation results 2026-09-16

PROVEN:
- FPGA render image live: console VERSION=00020058, AEM=loaded.
- BCLK and FSYNC reach the board: the McASP synchronizes to the FPGA. Board->FPGA
  playback (`aplay -D hw:TDM8 -c 8 -f S32_LE -r 48000`) runs with no error, which
  requires the received ACLKX/AFSX. So J11-21 (BCLK) and J11-15 (FSYNC) are wired.
- Board TDM8 config is correct and matches this BSP (8 slots, dsp_a, codec master,
  AXR0 TX / AXR2 RX, pinmux above).

NOT YET VALIDATED (data path FPGA -> board):
- FPGA -> board capture (`arecord -D hw:TDM8 ...`) returns exactly one 2048-frame
  buffer of ZEROS, then `read error: Input/output error`, every time.
  RESOLVED 2026-09-17: the one-buffer-then-EIO half is a k3-udma BCDMA cyclic-RX
  bug, now fixed by a tracked patch the build scripts stage; see the 2026-09-17
  update at the end of this file. The ZEROS half is a separate question and is
  still item 2 below.
- DOUT is silent even when noise is fed into DIN: with no AAF stream bound and no
  active fabric loopback, the FPGA renders nothing, so AXR2 carries no signal to
  distinguish "wire good, nothing to send" from "AXR2 not landing".
- gPTP is not synced on the DUT (console SYNC=0, ASCAPABLE=0), because the gPTP
  TX-timestamp fix (milan-fpga #360) is not merged; without a media-clock lock the
  render/loopback junction does not move PCM.

## Remaining actions

1. Rebuild + reflash the board image from THIS BSP. The running rootfs predates
   `BR2_PACKAGE_ALSA_UTILS_ALSALOOP` (already in `myir_am62x_avb_defconfig`), so
   the production bridge (`tdm8-uac2.sh`, which uses `alsaloop`) cannot start on it.
   Until then the two directions can only be hand-driven with `arecord`/`aplay`.
2. Drive a KNOWN non-zero, framed signal on FPGA DOUT so AXR2 can be proven:
   bind an AAF stream (needs #360 merged + a controller + talker) or enable a
   fabric loopback / test pattern. Then the first captured buffer shows it.
3. ~~Characterize the RX `Input/output error`~~ DONE 2026-09-17: it was
   `drivers/dma/ti/k3-udma.c`, not the McASP and not the AXR2 wire. See the
   2026-09-17 update at the end of this file. What remains of this item is only
   to re-run the 3 s `arecord` on the MYiR with a kernel built from this BSP,
   which is a build-and-boot, not an investigation.

## Notes

- Key-based SSH over the ECM leg (192.168.7.10) needs the overlay
  `root/.ssh/authorized_keys` populated; it currently ships the REPLACE placeholder.
- The ECM/UAC2 composite re-enumerates on the host when `tdm8-uac2.sh` rebinds the
  gadget; the host cdc_ether interface changes name and needs its address re-added.

## Update 2026-09-16 (devmem register-level diagnosis)

Using `devmem` (the board McASP is at 0x2b10000; register offsets from the mainline
`davinci-mcasp.h`), read WHILE a capture/playback loop keeps the McASP powered:

- RX and TX are configured IDENTICALLY and correctly: RXFMT==TXFMT (0x000180F0),
  RXFMCTL==TXFMCTL (0x400 => FSRMOD=8 slots, FSRPOL=0, 1-bclk frame sync),
  ACLKRCTL==ACLKXCTL (0x00180080 => external clock, synchronous RX), RXTDM==TXTDM
  (0xFF => 8 slots), serializer 2 = RX mode, PFUNC=0 (all McASP function).
  `rx-num-evt` = `tx-num-evt` = 0x20. So the receive path is NOT misconfigured.
- The `Input/output error` is ON-CHIP, not the wire: with the McASP's internal
  DIGITAL loopback enabled (LBCTL.LBEN=1 at 0x2b1004c, which the driver never
  touches), capture returned NON-ZERO data (proving the RX serializer + DMA can
  move data) but STILL errored after exactly one 2048-frame buffer. So the error
  reproduces with no FPGA and no external signal: it is a board-side McASP capture
  fault (RX stops after the first period/buffer), independent of the TDM8 wire.
- Without loopback, capture is zeros: consistent with the FPGA rendering silence
  (no stream bound, gPTP unsynced), so the zeros do not by themselves prove an AXR2
  wiring fault. The on-chip one-buffer-then-error is the primary board issue to fix.

### CAUTION: devmem on this SoC can panic the board

A `devmem` access to a McASP register while the peripheral is runtime-PM suspended
(no PCM open, clock/power gated) raises an Asynchronous SError and PANICS the kernel
(hang, no auto-reboot, needs a power cycle). Only devmem McASP registers while an
`arecord`/`aplay` is actively holding the device powered; never between streams.

### Next for the on-chip capture error

Reproduce standalone (internal loopback, no FPGA), then bisect the RX period/DMA
path: try `plughw` vs `hw`, explicit `--period-size`/`--buffer-size` that are
multiples of `rx-num-evt`x`channels`, and compare the RX AFIFO status (RFIFOSTS)
against TX during a sustained capture.

**Answered on 2026-09-17, see the update below.** It was not an AFIFO,
`num-evt` or period property and it does not live in the McASP node at all: it
was the k3-udma driver. The line above is kept as the record of what was tried.

## Update 2026-09-17 (RESOLVED: it was k3-udma, not the McASP)

The one-buffer-then-`Input/output error` above was fixed on 2026-09-17 by a
three-hunk change in `drivers/dma/ti/k3-udma.c` (kernel build `#5`). That
version was replaced on 2026-09-28 by
`res/tdm8/patches/0001-dmaengine-ti-k3-udma-count-bcdma-cyclic-rx-static-tr-z-in-bursts.patch`;
see the 2026-09-28 update below. It is a **tracked patch that
`res/tdm8/apply-tdm8-kernel.sh` stages automatically**, so every kernel built
from this BSP carries it; there is nothing to apply by hand, and nothing was
committed into the `linux` submodule.

Root cause: on a BCDMA `DEV_TO_MEM` **cyclic** channel the driver closed a
packet on every period, in two places - `CPPI5_TR_CSF_EOP` on the last TR of
each period in `udma_prep_dma_cyclic_tr()`, and a static burst count of one
period's worth of elements in `udma_configure_statictr()`. That packet boundary
retires the cyclic TR descriptor the hardware would otherwise reload forever,
and nothing re-arms it: the descriptor is pushed to the ring once, and the only
re-arm path is host-descriptor code a TR mode channel must not take. So the
channel takes exactly one TR event and one ring completion and then stops -
precisely the signature recorded above, including the fact that it reproduced
with the McASP's internal digital loopback and no FPGA at all, because the
fault was never on the wire.

Proven on the **PocketBeagle 2** (AM6254, McASP0, kernel `7.1.0-tdm8-pb2` build
`#5`):

```sh
arecord -D hw:0,0 -c8 -f S32_LE -r48000 \
        --period-size=1024 --buffer-size=16384 -d3 /tmp/c.wav
```

exits 0 and writes **4608044** bytes (44 byte WAV header + 3 s x 48000 frames x
8 ch x 4 bytes), and the RX line in `grep dma-controller /proc/interrupts`
advances by about 140 across the run instead of by exactly 1.

The MYiR result above has **not** been re-measured since; what is established
is that the defect is in the shared AM62x BCDMA path rather than in either
board's McASP node, and that the MYiR symptom (one buffer, then EIO, reproduced
under internal loopback) is the same signature. Re-running the arecord above on
the MYiR with a kernel built from this BSP is the outstanding confirmation.

### Update 2026-09-28: the stop-time residue was the defect

The residue described next belonged to the build `#5` version, which took EOP
off RX. With the alsaloop bridge restarting capture on every xrun, each stop's
teardown timeout and hard channel reset eventually left CPU 0 in an interrupt
livelock (RCU stall) on the PocketBeagle 2. The replacement patch keeps EOP on
RX and counts the PDMA static TR Z in bursts (`rx-num-evt = <32>`), so the
teardown completes: build `#6` ran the bridge for 30 minutes with no stall and
no teardown timeout. The text below is kept as the build `#5` record.

### Known residue of the fix (build `#5`, superseded)

With the patch applied, **stopping** a capture still logs one pair of lines
every time:

```
ti-udma 485c0100.dma-controller: chan1 teardown timeout!
davinci-mcasp 2b00000.audio-controller: unhandled rx event. rxstat: 0x00000104
```

At stream stop only, never while a stream runs, and the next capture still
opens and runs to full length, so capture is unaffected. What the driver source
supports: `udma_synchronize()` waits one second for a teardown completion
message, which `udma_ring_irq_handler()` signals only when a TDCM descriptor is
popped off the completion ring, and on `DMA_DEV_TO_MEM` it is the peer PDMA
that `udma_stop()` asks for it - so the warning means none arrived in time, and
the same branch then forces `udma_reset_chan()`, which is why the channel comes
back. The McASP line is the RX interrupt handler warning about a status it does
not handle: it handles only `ROVRN` (RXSTAT bit 0), which is clear in `0x104`.
What the source does not say is what produces the teardown completion for a
cyclic RX channel at all. Whether the timeout predates the fix is unknown,
because before it a capture never ran long enough to be stopped normally. The
residue is unexplained and the patch is a bench fix, not finished upstream
work.
