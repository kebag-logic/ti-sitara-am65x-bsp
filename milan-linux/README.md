# milan-linux: the PocketBeagle 2 USB-to-Milan bridge daemons

Path A of the PB2 TSN work (issue #1): Milan on the PocketBeagle 2 Ethernet Cap
without PipeWire, running **the same control-plane implementation as the RISC-V
end station**.

| Program | What it is | Ticket |
|---|---|---|
| `milan-ctrld` | the Milan control plane: milan-fpga's Mark II control-plane firmware ([milan-fpga#665](https://github.com/kebag-logic/milan-fpga/issues/665)), unchanged, on a soft fabric: ADP, ACMP, MAAP and AECP with its saved state | #8, #13, #29 |
| `milan-mediad` | the media plane: the AAF talker fed by the UAC2 gadget, on the media clock; the AAF listener that feeds the gadget back; and the servos that steer the USB host | #10, #11, #12 |
| `milan-dp` | prints the datapath block `milan-ctrld` publishes and the media block `milan-mediad` publishes | #8 |
| `milan-bridge.sh` | starts and stops them; `S95avb` runs it for `AVB_STACK=native` | #8 |
| `milan-gptp-watch` | judges gPTP from the wire, whichever daemon runs it: the PHC's offset from the grandmaster, from the board's hardware receive time of each Sync and the time its Follow_Up carries | #4 |

## How the RISC-V firmware runs on Linux

On the RISC-V end station, the control-plane firmware reaches the FPGA fabric
only through `sw/firmware/ctrl/mbx/mbx_hal.h`: `read32`, `write32` and `wait` on
the packet mailbox. The fabric supplies the RX rings and the ingress filter, the
TX merge, the timer bank, the link level and the gPTP plane.

On the PB2, those three calls land on milan-fpga's own C model of the fabric
side of the mailbox (`sw/firmware/ctrl/host/mbx_model.c`). The mailbox suite
grades that model against the RTL. `src/softfab.c` then feeds the model from
Linux:

| Fabric duty | On Linux |
|---|---|
| ingress filter, RX rings | `AF_PACKET` on `eth0`. A BPF prefilter passes AVTP control subtypes, MSRP and MVRP; the model's own full-tuple filter does the rest |
| TX merge | each frame the model sends goes out on the socket inside the write that committed it |
| timer bank, TICK | the model's millisecond clock, advanced to `CLOCK_MONOTONIC` on every wake |
| link level | rtnetlink (`IFF_UP` and `IFF_RUNNING`) |
| gPTP plane | `flexptpd`'s status block `/dev/shm/flexptpd.<interface>` (`-g`), read every 100 ms under its seqlock (`src/gptp_shm.c`): the grandmaster and asCapable |
| interrupt and sleep | `epoll`, with a timeout at the model's next deadline |
| datapath CSR window | `/dev/shm/milan-datapath` (`src/datapath.h`): the MAAP range, each STREAM_OUTPUT's stream, each STREAM_INPUT's settled stream |
| the face block (contract 2.3): AECP's observations and settings | the model's far side (`src/aecp_far.c`): GET_STREAM_INFO, GET_AVB_INFO, GET_AS_PATH and GET_COUNTERS from the datapath block, flexptpd's block and milan-mediad's media block; SET_STREAM_FORMAT's verdict; map edits refused; every commit acknowledged ([AECP-STATUS.md](AECP-STATUS.md)) |

The firmware sources are compiled from the milan-fpga tree at
`milan-fpga.pin`, with the flags of milan-fpga's own gate (C11, `-Wall -Wextra
-Werror -pedantic`), and with the board's shape header (`config/srp_entity_gen.h`) and
`CTRL_HOST_PLATFORM` in every unit, as an image of the split platform is built.
The boot is the split platform's (`sw/firmware/ctrl/plat/ctrl_plat.c`): the
generated AEM image verified against its CRC, the application (ADP, ACMP, MAAP)
and AECP with its face composed, the saved state applied through AECP's owner,
the mailbox opened with ADP held, AECP released, then ADP (Milan v1.2 5.6.1).
SRP is not attached yet (#14).

The board's end-station config is `config/endstation_pb2_bridge.yaml`,
milan-fpga's descriptor-only host profile (`board.target: host`,
[milan-fpga#983](https://github.com/kebag-logic/milan-fpga/issues/983)): one
8-channel 32-bit AAF input, the CRF input an AAF talker requires (Milan v1.2
7.2.2) and one AAF output, so ADP advertises 1 source and 2 sinks, under the
pinned entity_model_id `0x001BC50AC1000100`. `tools/gen-config.sh` generates
every file milan-ctrld reads about the entity from it, through milan-fpga's own
generators: `entity.conf` (the ADP fields, `adp_entity.py`), `aecp_entity_gen.h`
(the AEM image, `aecp_entity.py`), `srp_entity_gen.h` (the shape header,
`srp_entity.py`) and `nvm_shape_gen.h` (the store's shape). `make check`
regenerates them and refuses a stale one. The entity ID is the EUI-64 of
`eth0`'s MAC, the builder's `mac-derived` rule: milan-ctrld puts it, and the
MAC, in the image's three mac-derived fields after it verifies the generated
image.

The integrator's ports (`src/milan_ctrld.c`):

| Port | Today |
|---|---|
| `acmp_env.locked` | AECP's lock (the bridge, `ctrl_app_aecp.c`) |
| `acmp_env.source` | stream ID = the MAC, then the source index; destination = MAAP base + index; VLAN `AVB_VLAN_ID` |
| `acmp_env.srp` | the settled stream, published in the datapath block (SRP itself is #14) |
| `acmp_env.persist`, `.changed` | logged; the bridge persists the binding and notifies AECP's controllers |
| `maap_allocation` | published in the datapath block |

## The talker: USB to Milan (#10, #11)

`milan-mediad` sends the AAF stream on the **media clock**, not the USB host's.
PDU *k* leaves at gPTP time *t_k* = *T0* + *k* × 125 us and carries 6 frames
taken from the gadget's capture buffer. It is stamped *t_k* + PTO, so the
timestamps step by exactly 125 000 ns. The host keeps that buffer at a steady
level because of the **servo** (`src/servo.c`). A PI controller on the buffer
level writes the gadget's asynchronous feedback, `Capture Pitch 1000000`, and the
host's rate follows it, whatever the offset between the host's USB clock and
gPTP. No frame is resampled, dropped or repeated: the stream is bit-exact.

| Setting | Default | What it is |
|---|---|---|
| `-L` (`AVB_TALKER_LEVEL`) | 48 frames | the level the servo holds, and the bridge's own latency: about 1 ms from a USB packet to its PDU. 24 (500 us) ran dry behind the bench host's USB delivery gaps |
| `-M` | 192 frames | the level past which frames are dropped (an overrun); `milan-bridge.sh` passes 4 x `-L` |
| `-o` (`AVB_PTO_NS`) | 2 000 000 | presentation time offset |
| `-P` (`AVB_MEDIAD_PRIO`), `-a` (`AVB_RT_CPU`) | 70, CPU 3 | the talker thread: SCHED_FIFO, on the core the `ethcap` label isolates |

* **gPTP time** is the PHC, which flexptpd steers onto the grandmaster.
  Nothing steers the system clocks (there is no `phc2sys`), so
  `src/gptp_time.c` maps `CLOCK_MONOTONIC_RAW` (a vDSO read) onto the PHC
  with a model:
  - it is measured once a second with `PTP_SYS_OFFSET_EXTENDED` on
    `CLOCK_MONOTONIC_RAW`;
  - it is a least-squares line over the last 16 measurements, anchored at the
    newest;
  - a measurement that misses it by more than 20 us is a clock step, and the
    window starts over.

  `tests/test_gptp.c` holds it within 250 ns of a PHC running 36.9 ppm fast
  under +/-200 ns of read noise. The miss of each measurement is reported as
  `gptp_residual_ns`.
* **The talker's states.** PRIMING sends silence until the buffer holds its
  level. RUNNING sends 6 frames per PDU, or silence on an underrun, and drops
  frames past `-M` (an overrun). The level is judged as the slot saw it: frames
  that arrived because a wake was late are not an excess. IDLE begins when no
  new frame has arrived for 10 ms, not when the buffer is empty, since a host
  that stops can leave a few frames. It drops them, sends silence, and puts the
  pitch back at nominal.
* **Sleeping, and gPTP jumps.** Each slot's wait is measured in gPTP time and
  slept on `CLOCK_MONOTONIC`, which no clock step moves. A slot more than 50 ms
  ahead or behind means the time base jumped (flexptpd's first lock, a
  grandmaster change), and the stream starts over on the new time base.
* **Frames go out tagged** by the talker itself (VID from the datapath block,
  PCP = `SO_PRIORITY` = 3), on `eth0`, so `mqprio` puts them in class A
  (`avb-shaper.sh`).
* **The stream runs while MAAP holds a destination.** Gating it on an SRP
  Listener Ready waits for SRP (#14).
* **Reports** go to `/dev/shm/milan-media`, printed by `milan-dp`:
  `talker_frames_tx`, `_underruns`, `_overruns`, `_late`, `_level_min/max/avg`,
  `_pitch`, `_locked`, and `gptp_corr_ns`/`gptp_residual_ns`; and
  `milan_stream_output0`, STREAM_OUTPUT 0's Milan counters (Milan v1.2 Table
  5.4), which GET_COUNTERS serves ([AECP-STATUS.md](AECP-STATUS.md)).

**Latency, with the PTO excluded.** From a frame's USB packet to its PDU's slot
is the buffer level: `talker_level_avg` / 48 000, about 500 us, plus up to one
125 us slot. From slot to wire is `hw_rx_ts - (avtp_timestamp - PTO)` on a peer
that stamps every frame (`validation/pb2-tsn/latency-peer.py`). The bridge
latency is the sum of the two.

## The listener: Milan to USB (#12)

The listener plays the stream `milan-ctrld` settled on (ACMP, BIND_RX) into the
gadget's playback PCM, so the host records it with `arecord`. A frame with
presentation time *T* must reach the host's USB IN packet at *T*. When the
listener queues a PDU at gPTP time *t*, its first frame leaves at about
*t* + (frames queued ahead) / 48 000 + `-R` (the IN requests already filled:
`TDM8_REQ_NUMBER` × 125 us, 500 us). The difference from *T* is the
presentation error:

* The first PDU of a stream is placed: silence is queued until the error is
  nought.
* After that, the servo holds it at nought through `Playback Pitch 1000000`. A
  stream running late makes the gadget send faster.
* An error past 1 ms is jumped instead (`listener_jumps`): silence inserted,
  or frames dropped, which interrupts the playback (STREAM_INTERRUPTED, once
  for a run of jumps).
* A toggle of the PDUs' mr bit (a media clock restart, IEEE 1722-2016
  4.4.4.3) places the stream again. While tu is set (4.4.4.7), or tv clear,
  PDUs are played as they come, with no jump and no servo step. A PDU stamped
  more than 4 x PTO ahead is too far in the future to process, and is dropped.

The media block holds each event's raw count: `listener_frames_rx` (PDUs),
`_seq_mismatch`, `_late_timestamp`, `_early_timestamp`, `_unsupported_format`,
`_media_locked`, `_media_unlocked`, `_jumps`, `_stream_interrupted`,
`_timestamp_uncertain` and `_media_restarts`. `milan_stream_input0` holds the
Milan counters GET_COUNTERS serves (v1.2 5.3.8.10, Table 5.6): most count
500 ms observation intervals in which the event happened, and all are zeroed
when the input goes from not bound to bound. It also reports
`_align_min/max/avg_ns`, the presentation error, and `_margin_min_ns`, how
early the PDUs arrive.

## The CRF input (STREAM_INPUT 1)

milan-mediad's CRF receive path (`src/crf_rx.c`) receives the CRF stream
`milan-ctrld` settled STREAM_INPUT 1 on, and counts it as the listener counts
its own: sequence, mr, tu, the Milan CRF format (Milan v1.2 Table 7.1), its
timestamps late or early. It tracks the media clock the stream carries: locked
(MEDIA_LOCKED) once 25 timestamps in a row step on the format's 2 ms grid,
unlocked when one leaves it, when mr toggles, or when the stream stops
(STREAM_INTERRUPTED). Nothing clocks from it: the talker and the listener keep
their own gPTP-derived clock. `milan-dp` prints `crf_*` and
`milan_stream_input1`. `-r` names the roles (`talker,listener,crf`, all three
by default); without `crf`, STREAM_INPUT 1 has no counter valid.

**No SRP domain (`-n`, `AVB_SRP_DOMAIN=none`).** A Milan listener that has
settled waits TMR_NO_TK (10 s) for its talker's MSRP registration, and probes
again without one. Until the bridge has SRP (#14), `milan-ctrld -n` declares
the link direct, with no MSRP bridge, and reports a settled sink's talker as
registered (`acmp_tk_registered()`, from the main loop: no port calls back
into a core, milan-fpga #678). Through an AVB switch, SRP is needed anyway: a
switch forwards a stream only for a registered talker and listener.

## Saved state (#13)

`milan-ctrld -N /data/milan/journal.bin` (`AVB_JOURNAL`) runs milan-fpga's
Mark II saved-state store (`sw/firmware/ctrl_nvm`), unchanged, with AECP's
owner (`aecp_nvm`) as its state port and the bindings' owner (`acmp_nvm`) on
it, in the split platform's boot order: compose, then the store's boot, its tick
and its step, and then open. So a binding, a name, a format or a map is
restored before any channel opens, and the listener fast-connects at boot
without a controller. `-N none` keeps the journal in memory for the life of the
process.

* **The media** (`src/nvm_file.c`) is a file of the KLJ3 journal's size,
  640 KiB: two slots of five 64 KiB erase blocks (`nvm_shape.h`
  `NVM_JOURNAL_SIZE`), with flash semantics kept. An erase sets a block to
  0xFF, and a program only clears bits. A worker thread writes every operation
  to the file (`pwrite`, `fdatasync`) in issue order, and `busy()` stays 1 until
  it is on the card. The store's write order is the flash's, and the control
  loop never waits on the card.
* **The earlier journal.** A file of the KLJ2 journal's size (128 KiB, the
  image before this pin) found at the path is moved to `<path>.klj2` and read
  back, never written, as the legacy range: the store records its header
  (`legacy 0x1` in the log) and applies nothing of it, so the bindings start
  blank once.
* **The shape** (`config/nvm_shape_gen.h`) comes from milan-fpga's own
  derivation (`tools/gen-nvm-shape.py`, through
  `sw/firmware/ctrl_nvm/test/nvm_bench.py`), for the board's config. The
  container's identity words are run-time variables: the entity ID comes from
  `eth0`'s MAC. milan-ctrld refuses a shape derived for another model ID.
* **The model** is proven as on the RISC-V end station: by the generated AEM
  image's CRC at boot, before the board's identity goes into its copy.
* At boot, and on SIGUSR1, `milan-ctrld` logs the store's verdict: the
  authoritative slot and sequence, `bindings restored`/`blank`, the rest's
  terminal, the phase, the commits, any unread slot or read fault, and the
  legacy header.

## AECP (#29)

milan-ctrld serves the firmware's AECP from a host AEM store: the generated
image (`config/aecp_entity_gen.h`) with the board's identity, in the static
storage the split platform sizes from it. What a controller reads that the
fabric answers on the RISC-V end station, the face's windows, is answered from
the system (`src/aecp_far.h`). [AECP-STATUS.md](AECP-STATUS.md) lists what is
served, what is refused, what the media plane does not follow, and why no
Milan conformance is claimed for the board.

## Build and test on the host

```sh
tools/fetch-milan-fpga.sh --with-submodules   # the pinned tree into .milan-fpga (sha256-checked)
make                                          # build/milan-ctrld, build/milan-dp
make check                                    # config/ current, unit tests, network-namespace tests
```

The pin names the published head of milan-fpga PR #986.
`tools/fetch-milan-fpga.sh` verifies its archive hash; no local checkout is
required. The pin must move to the upstream merge commit after #986 lands.

`make check` runs the unit tests (`test_gptp`, and `test_media`: the AAF
PDU and the CRF PDU byte for byte, the Milan observation-interval counter, and
the servo in closed loop at 0, +-100 and +300 ppm), then the network-namespace
tests across a veth pair inside `unshare -rn`, so they need neither root nor a
real interface.

`tests/run-netns-talker.sh` streams a simulated USB host running 80 ppm fast
and records the far end. The validation kit then grades the recording:
`aaf-analyze.py` (no sequence gap, the 125 000 ns step, the AAF fields, VLAN 2
PCP 3) and `ramp-check.py` (the counting ramp bit for bit). The test also
checks that the pitch cancels the host's offset and that the level holds. The
servo's lock time and the slot-to-wire figures are board checks: without
SCHED_FIFO the host stalls for milliseconds.

`tests/run-netns-listener.sh` runs two complete bridges, A on `veth0` and B on
`veth1`, in no-SRP mode:

1. A streams a simulated USB host that runs 80 ppm fast.
2. A controller binds B's STREAM_INPUT 0 to A's STREAM_OUTPUT 0 with a Milan
   BIND_RX (`tests/bind.py`).
3. B probes A and settles, then plays the stream to its own simulated host,
   50 ppm slow, which records it.

Over the measured window, B's counters must show 8000 PDUs/s and no sequence
error, late PDU, interruption, reset or underrun, and the presentation error
must stay within 125 us. The playback pitch must cancel the host's offset. The
recording, inside that window, must be the ramp bit for bit. Without SCHED_FIFO,
on a shared host, either bridge can be held off its CPU for tens of
milliseconds. So the test runs a 50 ms PTO and drop level, and a 100 ms
interruption threshold (`-T`). The board runs 2 ms, 2 ms and 10 ms, on its own
isolated core.

`tests/run-netns-nvm.sh` runs the saved state against real power cuts (SIGKILL,
nothing flushed):

* A6.1: a bind is written, B is killed and restarted, and B settles on the
  talker again with no controller.
* A6.2: an unbind survives a power cut.
* A6.3: 20 cuts at random points of bind/unbind; every boot must judge an
  accepted slot and restore the bindings, with no read fault, and the journal
  must still take a bind afterwards.

`tests/peer.py` is the control-plane test. The peer plays a fake flexptpd
(its status block), the controller, a listener, a talker and a MAAP peer,
and grades what `milan-ctrld` sends:

- ADP: the identity, `entity.conf`'s fields, flexptpd's grandmaster, the reply to
  DISCOVER, a grandmaster change, ENTITY_DEPARTING on SIGTERM;
- MAAP: the PROBEs, their spacing and the ANNOUNCE, a range in the pool sized
  to the talker sources, and the DEFEND against a conflicting PROBE;
- ACMP talker: PROBE_TX_RESPONSE with the stream, the MAAP address and VLAN 2,
  inside 200 ms, and TALKER_UNKNOWN_ID for a source that does not exist;
- ACMP listener: BIND_RX, its PROBE_TX_COMMAND, settlement in the datapath
  block, and UNBIND_RX.

`tests/run-netns-aecp.sh` is a controller on the other end of the pair. It
enumerates the entity (READ_DESCRIPTOR of ENTITY, CONFIGURATION 0 and every
descriptor its counts list, field by field where the board's inventory is
about), runs GET_MILAN_INFO, ACQUIRE and LOCK, the Milan v1.2 5.4 command set as
the firmware implements it, an unsolicited notification and GET_COUNTERS over a
media block with known counters, then binds STREAM_INPUT 0 and STREAM_INPUT 1
with BIND_RX. It also starts milan-ctrld over a KLJ2 journal file and checks the
move to KLJ3. The DUT's MAC is not the generated image's placeholder, so the
board's identity in ENTITY, AVB_INTERFACE and ADP is checked as milan-ctrld
writes it at run time. Last, CLOCK_DOMAIN's LOCKED and UNLOCKED are checked
against a flexptpd that stops updating its block, leaves it mid-write, or is
replaced by a new block.

`tests/run-netns-counters.sh` runs milan-ctrld and milan-mediad (talker, AAF
listener and CRF input, on a simulated USB host) against a talker on the other
end that binds both inputs and sends AAF and CRF streams. Into those it puts a
lost PDU, tu, an mr toggle, another format, a late backlog, early stamps and
an interruption, one at a time. It reads each counter's change back over
GET_COUNTERS, and checks the zeroing at a new binding and the talker's
STREAM_OUTPUT counters.

`make check-compat` enumerates the entity with a controller on la_avdecc 4.3.x
(`tests/milan-compat/`, which says how to build the library). It passes only
for a Milan-compatible entity with no compatibility event, warning or error.

milan-fpga's own gate (`python3 sw/firmware/ctrl/test/test_ctrl_firmware.py`)
must pass at the pinned revision too. It needs a git checkout with the
`protocol-processor` and `gptp-processor` submodules.

## Build for the board

The Buildroot packages `milan-fpga-src` (the pinned archive, sha256 in its
`.hash`) and `milan-bridge` (this directory) are in
`bb_pocketbeagle2_avb_defconfig`. On the board:

```sh
/etc/init.d/S95avb status          # ... --- stack: native --- then milan-ctrld and milan-dp
/usr/sbin/milan-bridge.sh status   # logs the state to syslog, prints the datapath block
logread | grep milan-ctrld          # or /var/log/messages
```

## Moving the pin

1. Change `MILAN_FPGA_REV` and `MILAN_FPGA_SHA256` in `milan-fpga.pin`, the
   gitlinks' revisions and sha256 beside them, and the hash lines in
   `br2-external/package/milan-fpga-src/milan-fpga-src.hash`.
2. `tools/fetch-milan-fpga.sh --with-submodules`, then regenerate `config/`
   with `tools/gen-config.sh`.
3. `make check`, and milan-fpga's gate at the new revision.
