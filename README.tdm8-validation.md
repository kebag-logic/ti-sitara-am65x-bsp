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
3. Characterize the RX `Input/output error`: /dev/mem is locked (CONFIG_STRICT_DEVMEM),
   so the McASP RXSTAT is not readable from userspace. A scope on AXR2 (J11-17) and
   AFSX confirms whether AXR2 carries valid framed data. TX works on the same
   clocks, so the error is specific to the AXR2 receive path.

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
against TX during a sustained capture. The fix (an AFIFO/`num-evt`, period, or DMA
property) then lands in this BSP's McASP node.
