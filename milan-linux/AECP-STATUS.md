# AECP on the board: status

Status: milan-ctrld serves milan-fpga's AECP from a host AEM store generated
from the board's own end-station config. Enumeration, the command set the
firmware implements and ACMP binding of both STREAM_INPUTs pass on a veth pair.
**No Milan conformance is claimed for the board**: its media plane does not
clock from the CRF input, and the gaps below remain.

The pin is **provisional**: `milan-fpga.pin` names `677d605f`, the head of
milan-fpga's `pf-linuxprof-1` (milan-fpga#983, the descriptor-only host profile
this config needs), which has no published archive yet. Its sha256 reads
`provisional` until the archive's is known; the host build takes the tree from
a local checkout (`MILAN_FPGA_LOCAL`), and the Buildroot package cannot
download it until then.

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
| GET_COUNTERS | AVB_INTERFACE LINK_UP, LINK_DOWN, GPTP_GM_CHANGED; CLOCK_DOMAIN LOCKED, UNLOCKED (flexptpd's lock); STREAM_INPUT 0 and STREAM_OUTPUT 0 from milan-mediad's media block |
| ADD/REMOVE_AUDIO_MAPPINGS | refused at the edit's begin: NOT_SUPPORTED |
| F_COMMIT (settings) | acknowledged at once |

## What is not met, and why no Milan conformance is claimed

1. **The CRF input binds and counts, but nothing clocks from it.**
   STREAM_INPUT 1 binds over ACMP, settles, appears in the datapath block and in
   GET_STREAM_INFO, and is counted among ADP's sinks. milan-mediad receives
   only STREAM_INPUT 0 and plays it on its own gPTP-derived clock. AECP accepts
   SET_CLOCK_SOURCE to the CRF source (index 1), as the model offers it, but
   the media plane does not follow it.
2. **STREAM_INPUT 1's counters:** answered with none valid, because no media
   program receives the CRF stream (Milan v1.2 5.4.2.25, Table 5.16).
   TIMESTAMP_UNCERTAIN (STREAM_INPUT 0, STREAM_OUTPUT 0) and STREAM_OUTPUT 0's
   MEDIA_RESET are not counted by milan-mediad and are not reported valid.
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

## Evidence

- `make check`: `config/` current; `test_media`, `test_gptp`; `tests/peer.py`
  (ADP, MAAP, ACMP); `tests/aecp.py`, 87 checks: enumeration of all 32
  descriptors, GET_MILAN_INFO and the system unique ID, ACQUIRE
  (NOT_SUPPORTED), LOCK, the 5.4 command set, an unsolicited notification,
  GET_COUNTERS over a media block with known values, BIND_RX on STREAM_INPUT 0
  and 1, and the KLJ2-to-KLJ3 journal move; the talker, listener and
  saved-state power-cut suites.
- The aarch64 cross build: `make CC=aarch64-linux-gnu-gcc`, which produces
  `milan-ctrld`.

Not done: nothing was installed on a board. The Buildroot build waits for the
published archive. milan-fpga's own firmware gate at the pin was run in part
(milan-fpga#983's receipts list which).
