# AECP on the board: status

Status: milan-ctrld serves milan-fpga's AECP from a host AEM store generated
from the board's own end-station config, and milan-mediad keeps the Milan
counters of all three streams. On a veth pair, the x86 build passes the
host enumeration suite and a controller built on la_avdecc 4.3.1.1 (the
library Milan controllers use) finds the entity Milan 1.2 compatible, with no
compatibility event, warning or error. That is controller compatibility, not
Milan conformance. **No Milan conformance is claimed for the board**: its media
plane does not clock from the CRF input, and the gaps below remain.

The pin names the published head of milan-fpga PR #986 (milan-fpga#983, the
descriptor-only host profile this config needs). `tools/fetch-milan-fpga.sh`
verifies its archive hash; no local checkout is required. The pin must move to
the upstream merge commit after #986 lands.

## The entity

`config/endstation_pb2_bridge.yaml` is milan-fpga's host profile
(`board.target: host`, `board.gptp_owner: host-phc`). It states the board's
inventory once, and `tools/gen-config.sh` generates everything else from it
through milan-fpga's generators. `make check` refuses a stale file.

| Descriptor | What it is |
| --- | --- |
| STREAM_INPUT 0 | AAF, 48 kHz, 8 channels, 32-bit (the Base family completed, Milan v1.2 6.4) |
| STREAM_INPUT 1 | CRF, `0x041060010000BB80` (Milan v1.2 7.2.2, 7.3.2) |
| STREAM_OUTPUT 0 | AAF, the same format |
| CLOCK_SOURCE 0, 1 | INTERNAL, and the CRF input's, located on STREAM_INPUT 1 |
| AVB_INTERFACE 0 | flexptpd's dataset at its pin: priorities 248/248, clockClass 248, accuracy 0xFE, variance 0xFFFF, log intervals -3/0/0, port 1 |

ADP advertises 1 source and 2 sinks. There is no second AAF stream and no CRF
output (Milan v1.2 7.2.3 asks one only of a listener with two AAF inputs). The
entity_model_id is pinned to `0x001BC50AC1000100` (IEEE 1722.1-2021 7.2.1:
controllers cache the model by it). The image is 32 descriptors, 5784 bytes.

The image bakes the config's MAC into three mac-derived fields (ENTITY
entity_id, AVB_INTERFACE mac_address and clock_identity). milan-ctrld verifies
the generated image against its generated CRC, checks that those fields are one
mac-derived identity and that `entity.conf` and the compiled shape describe the
same model, then writes eth0's identity into a copy and loads that copy against
its own CRC. ADP and AECP therefore name the same entity (IEEE 1722.1-2021
Table 7-2).

## How it runs

The boot is the split platform's (`sw/firmware/ctrl/plat/ctrl_plat.c`), with
the firmware unchanged. The image is verified. The application, AECP and
contract 2.3's face are composed. The saved state is applied through AECP's
owner. The mailbox opens with ADP held, AECP is released at the store's
accepted terminal, and ADP starts last (Milan v1.2 5.6.1). Two platform parts
have no counterpart here. SRP is not attached (#14). The receive monitor's
per-input format contexts (#946) are fabric registers for AAF inputs above 0,
and this entity has none. Every unit is compiled with the board's shape header
and `CTRL_HOST_PLATFORM`, which selects THE SHAPES' host row for (1, 1, 2)
(milan-fpga#983). That row's figures are a stated host scheduling profile, not
a measurement on the board.

The face's windows are answered by `src/aecp_far.c` from what the board has:

| AECP | Answered from |
| --- | --- |
| GET_STREAM_INFO, STREAM_OUTPUT | the datapath block: stream ID, MAAP address, VLAN |
| GET_STREAM_INFO, STREAM_INPUT | ACMP's view, through the firmware's bridge |
| SET_STREAM_FORMAT | supported only for each stream's current format, the one the media plane runs |
| GET_AVB_INFO | flexptpd: grandmaster, mean link delay, asCapable, gPTP enabled; SR class A at PCP 3, VLAN 2 |
| GET_AS_PATH | flexptpd, where it determines the path: the board as grandmaster, or the grandmaster one step away; otherwise not answered |
| GET_COUNTERS | AVB_INTERFACE LINK_UP, LINK_DOWN, GPTP_GM_CHANGED; CLOCK_DOMAIN LOCKED, UNLOCKED (flexptpd's lock, while its status is live); STREAM_INPUT 0, STREAM_INPUT 1 and STREAM_OUTPUT 0 from the counters milan-mediad keeps for each (below) |
| ADD/REMOVE_AUDIO_MAPPINGS | refused at the edit's begin: NOT_SUPPORTED |
| F_COMMIT (settings) | acknowledged at once |

## The stream counters

GET_COUNTERS serves, for each stream, the counters a media program counts, and
marks valid only those (`src/media.h`, `src/aecp_far.c`). The definitions are
Milan v1.2's: Table 5.6 (5.3.8.10) for a STREAM_INPUT, Table 5.4 (5.3.7.7) for
a STREAM_OUTPUT, in the quadlets of Tables 5.16 and 5.17 (5.4.2.25). Milan
counts most of them per observation interval: one more at the end of each
interval in which the event happened. The interval here is 500 ms, closed
within 50 ms of its end. IEEE 1722.1-2021's per-event definitions
(7.4.42.2.4 and 7.4.42.2.5, Tables 7-157 and 7-159) are what Milan narrows.

| Stream | Who counts it | What it counts |
| --- | --- | --- |
| STREAM_INPUT 0, AAF | milan-mediad's listener | FRAMES_RX: intervals with a PDU of the bound stream. SEQ_NUM_MISMATCH: with a non-sequential sequence_num. MEDIA_RESET: with the mr bit toggled (IEEE 1722-2016 4.4.4.3). TIMESTAMP_UNCERTAIN: with tu set (4.4.4.7). UNSUPPORTED_FORMAT: with a PDU not 8 channels, 32-bit, 48 kHz. LATE_TIMESTAMP and EARLY_TIMESTAMP: with a valid (tv) stamp in the past, or more than 4 x PTO ahead. MEDIA_LOCKED, MEDIA_UNLOCKED: the presentation servo's lock. STREAM_INTERRUPTED: no PDU played for 10 ms (none arriving, or only ones too far ahead or of another format), or a run of jumps to put the stream back on time |
| STREAM_INPUT 1, CRF | milan-mediad's CRF receive path (`src/crf_rx.c`) | the same header counters for CRF PDUs (IEEE 1722-2016 10.4); UNSUPPORTED_FORMAT for a PDU not in the Milan CRF format (Milan v1.2 Table 7.1); LATE and EARLY on its timestamp; MEDIA_LOCKED once 25 stamps in a row step on the 2 ms grid of the format, MEDIA_UNLOCKED when a step leaves it, mr toggles, or the stream stops (STREAM_INTERRUPTED: no Milan PDU processed for 10 ms, none arriving or only ones too far ahead) |
| STREAM_OUTPUT 0, AAF | milan-mediad's talker | STREAM_START, STREAM_STOP; FRAMES_TX: intervals in which it sent; MEDIA_RESET and TIMESTAMP_UNCERTAIN: intervals in which what it sent had mr toggled or tu set. Its PDUs never toggle mr (its media clock is gPTP's slot grid, and a gPTP step starts a new stream) and never set tu, so both stay 0. The last three are zeroed at each start (Table 5.4) |

A STREAM_INPUT's counters are zeroed each time it goes from not bound to bound
(Milan v1.2 5.3.8.10), however fast the bindings follow one another.
milan-ctrld counts those changes per sink in the datapath block where the
binding is owned: the publication block's BINDING.BOUND, which ACMP writes
before it answers BIND_RX or UNBIND_RX (the soft fabric sees each edge in the
write that makes it). So a burst that unbinds and binds again inside one loop
pass counts each binding, and the count is in the block before the response
leaves. A binding the saved state restores at boot counts too, on from the
count the previous milan-ctrld left in the block. GET_COUNTERS compares the
media block's count with the datapath block's as it answers, so it reads zero
for a binding milan-mediad has not yet zeroed its counters for, from the
moment the binding is made. milan-mediad zeroes an input when it sees the count
move, and drops what its socket still holds from before. MEDIA_LOCKED is
always MEDIA_UNLOCKED or one more.

Nothing is reported valid that nothing counts. A media program that is not
running (milan-mediad absent, or run without its `crf` role) leaves its
stream's counters not valid, and so does an input whose receive socket cannot
be set up (no such interface, or a socket, filter, bind or receive timeout that
fails): it marks its counters valid only once the socket is set up. A failed
setup closes what it opened and is logged once with its step; milan-mediad runs
on with its other streams, and the input stays not received until milan-mediad
is restarted. `tests/run-netns-counters.sh` puts each fault into real AAF and
CRF streams and reads the counters back over AECP, with a burst of bindings
queued while both daemons are held, and GET_COUNTERS at once after a BIND_RX
response. `tests/run-netns-rxfail.sh` makes each setup step fail in each input.

CLOCK_DOMAIN LOCKED and UNLOCKED follow flexptpd's lock while its status block
is live: its update count must be seen to move. 2 s without a move (a stopped
flexptpd, a block stuck mid-write, a block nobody updates) expires the status:
one UNLOCKED, asCapable cleared, and no LOCKED until updates are seen again.

**Milan v1.3.** It is not among the specifications here. The controller cites
"Milan 1.3 - 5.3.8.10" for STREAM_INPUT's mandatory counters, the clause number
of v1.2's STREAM_INPUT diagnostic counters. la_avdecc 4.3.1.1, which reports
it, requires the ten counters of v1.2's Table 5.16 at the same bits, so for
STREAM_INPUT no difference can be inferred. For STREAM_OUTPUT, la_avdecc takes
the IEEE 1722.1-2021 layout (Table 7-158) from an entity whose GET_MILAN_INFO
gives a specification version of 1.3 or later. This entity's GET_MILAN_INFO
has no such field (protocol_version 1), so it is judged by v1.2's Table 5.17,
which is the layout served.

## As a la_avdecc controller sees it

`tests/milan-compat/` is a controller on la_avdecc 4.3.1.1 (tag v4.3.1.1,
6d61a92e). It discovers the entity over pcap, reads every descriptor, and
listens for 15 s while notifications and counters arrive, printing every
compatibility event and warning the library records. It ran against the x86
build on a veth pair, with the library's default options (five leniencies on)
and with all five off, both with the inputs unbound and with both bound to
streams that run (`streams`: a bridge, and a talker sending AAF and CRF). In
all four: `IEEE17221|Milan`, Milan 1.2, zero compatibility events, zero
warnings or errors. The Milan flag stayed set throughout, so every check the
library applies to a Milan 1.2 entity ran, and none recorded anything. Among
them are the mandatory counters of each descriptor, and their LOCKED/UNLOCKED,
START/STOP and LINK_UP/DOWN pairs, on every GET_COUNTERS answer and
notification. The receipts hold the full output.

The same program run against the build at f405a57 reports
`[Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT
descriptor`, the error the owner's controller gave the board. It also fails
when milan-mediad runs without its CRF receive path. No compatibility finding
was left unfixed, so none is listed as unfixable.

## What is not met, and why no Milan conformance is claimed

1. **The CRF input is received and counted, but nothing clocks from it.**
   STREAM_INPUT 1 binds over ACMP and settles, and milan-mediad's CRF receive
   path counts its PDUs and tracks the media clock they carry. The talker and
   the listener keep their own gPTP-derived clock. AECP accepts
   SET_CLOCK_SOURCE to the CRF source (index 1), as the model offers it, but
   the media plane does not follow it (Milan v1.2 7.2.2 asks the input; what a
   talker does with it is the media plane's).
2. **The talker never signals uncertainty.** Its PDUs never set tu: it starts
   a new stream on a gPTP step instead of streaming through it with tu set
   (IEEE 1722-2016 4.4.4.7 says a talker should set it). Its
   TIMESTAMP_UNCERTAIN is therefore 0, as counted from what it sends.
3. **Dynamic input maps:** ADD and REMOVE_AUDIO_MAPPINGS answer NOT_SUPPORTED
   (Milan v1.2 5.4.2.27, 5.4.2.28). The media plane renders stream channel n to
   USB channel n and has no router. GET_AUDIO_MAP serves that identity map.
4. **Formats:** STREAM_INPUT 0 advertises the Base family (Milan v1.2 6.4),
   but SET_STREAM_FORMAT takes only the 8-channel format the media plane runs.
5. **GET_AS_PATH** is not answered beyond one step from the grandmaster,
   because flexptpd publishes no path trace.
6. **No SRP** (#14): GET_AVB_INFO's SRP_ENABLED is clear, and no MSRP field of
   GET_STREAM_INFO is valid.
7. **REBOOT** answers NOT_IMPLEMENTED: this platform has no reset port.
8. **Timing:** the loop's budgets are the host row's stated profile. The
   millisecond bounds of AECP notices and SRP are not measured on the board.

## Acceptance (bsp#29)

| Criterion | State |
| --- | --- |
| Pin: milan-fpga moved to dev-soft-core, with `make check` and the firmware gate | **open.** The pin is PR #986's published round-2 head, not its merge into dev-soft-core, which has not happened: #986 is still in review, with a fix to its `ctrl_loop.h` in progress. `make check` passes at it. milan-fpga's firmware gate at the pin was run in part (milan-fpga#983's receipts list which); gate 1b, the shards and the mutation campaigns were not completed |
| Host test: enumeration against the daemon on a veth pair | met: `tests/run-netns-aecp.sh`, and the la_avdecc run above |
| Board: a controller enumerates the board without errors | **open.** The owner's controller enumerated the f405a57 build on the board and reported the STREAM_INPUT counters error. The 49b2035 build is installed on the board: read from a controller, STREAM_INPUT 0 and 1 report all ten counters valid and STREAM_OUTPUT 0 its five. Its enumeration by the owner's controller is not recorded. This build is not installed on the board |
| Install: under the RAUC flow, or a documented manual install | **partly met.** A manual install is documented in the change's receipts: the cross-built binaries and their list, milan-ctrld, milan-mediad and milan-dp together. The 49b2035 build was installed on the board that way; this build is not. The Buildroot packages are in the defconfig, but the Buildroot image build has not been run with the published archive |

## Evidence

- `make check`: `config/` current; `test_media` (AAF and CRF PDUs byte by
  byte, the observation-interval counter, the servo), `test_gptp`;
  `tests/peer.py` (ADP, MAAP, ACMP); `tests/aecp.py`: enumeration of all 32
  descriptors with a DUT MAC that is not the image's placeholder,
  GET_MILAN_INFO and the system unique ID, ACQUIRE (NOT_SUPPORTED), LOCK, the
  5.4 command set, an unsolicited notification, GET_COUNTERS of every stream as
  the media block marks it, BIND_RX on STREAM_INPUT 0 and 1 and the zeroing at
  bind, the KLJ2-to-KLJ3 journal move, and CLOCK_DOMAIN against a flexptpd that
  stops, sticks or is replaced; `tests/counters.py` (the counters under faults,
  a burst of bindings, GET_COUNTERS at once after BIND_RX); `tests/rxfail.py`
  (each receive setup failure, in each input); the talker, listener and
  saved-state power-cut suites, the last with the bind count a restored
  binding moves.
- `make check-compat`: the la_avdecc run above.
- The aarch64 cross build of all four programs (milan-mediad against the
  target's alsa-lib).

Not done: this build was not installed on a board (the 49b2035 build is, by
the documented manual install), and no enumeration of the board by the owner's
controller is recorded. The Buildroot image build has not been run with the
published archive. milan-fpga's own firmware gate at the pin
was run in part (milan-fpga#983's receipts list which).
