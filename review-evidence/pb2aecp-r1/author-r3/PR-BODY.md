The board advertised AEM_SUPPORTED, but milan-ctrld served no AECP, so a controller could not read the Configuration descriptor IEEE 1722.1-2021 7.2.1 requires. This branch makes milan-ctrld serve milan-fpga's AECP, unchanged, from a host AEM store generated from the board's own end-station config. milan-mediad keeps the Milan counters of all three streams, the CRF input's included.

Refs #29. **This PR does not close #29**: two of its acceptance criteria are open (the pin on dev-soft-core with the full firmware gate, and a controller enumerating the board), and a third is only partly met (the install). The table below gives each one's state. **No Milan conformance is claimed for the board**: the media plane does not clock from the CRF input. `milan-linux/AECP-STATUS.md` lists what is met and what is not.

## Acceptance (#29)

| Criterion | State |
| --- | --- |
| Pin: milan-fpga moved to dev-soft-core, with `make check` and the firmware gate | **open.** The pin is milan-fpga PR #986's published round-2 head (`66b4def3`), not a dev-soft-core merge, which has not happened. `make check` passes. milan-fpga's firmware gate at the pin was run in part: selected arms passed; gate 1b, the gate shards and the mutation campaigns were not completed (milan-fpga#983's receipts) |
| Host test: enumeration against the daemon on a veth pair | met: `tests/run-netns-aecp.sh` (107 checks), and a controller on la_avdecc 4.3.1.1 (`make check-compat`) that finds the entity Milan 1.2 compatible with no compatibility event, warning or error |
| Board: a controller enumerates the board without errors | **open.** The owner's controller enumerated the board running the f405a57 build and reported `[Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor`. This head fixes that on the veth pair, where the same la_avdecc program reproduces the error at f405a57. It has not been installed on the board |
| Install: under the RAUC flow, or a documented manual install | **partly met.** The Buildroot packages are in the defconfig, but the image build has not been run with the published archive. A manual install is documented in the receipts: which files, and that milan-ctrld, milan-mediad and milan-dp go in together |

## What changed

- **The pin** is milan-fpga `66b4def3`, the round-2 head of `pf-linuxprof-1` (milan-fpga#983, the descriptor-only host profile), published as milan-fpga PR #986. `MILAN_FPGA_SHA256` is its archive's sha256 (`c5b3b01c…`), and `tools/fetch-milan-fpga.sh` fetches and verifies it without a local checkout. The pin must move to the upstream merge commit after #986 lands. It also names tsn-c-stack (ADP, ACMP, MAAP, the wire layer) and lwSRP (the headers AECP's bridge includes), each with its archive's sha256.
- **The board's config**, `config/endstation_pb2_bridge.yaml`, is milan-fpga's host profile (`board.target: host`, `gptp_owner: host-phc`). It has one AVB_INTERFACE, STREAM_INPUT 0 AAF 48 kHz 8 ch 32-bit, STREAM_INPUT 1 CRF, and STREAM_OUTPUT 0 AAF in the same format. There is no second AAF and no CRF output, and ADP advertises 1 source and 2 sinks. It carries the specified entity name and the entity_model_id pinned to `0x001BC50AC1000100` (Milan v1.2 7.2.2, 7.2.3; IEEE 1722.1-2021 7.2.1). `tools/gen-config.sh` generates `entity.conf`, the AEM image, the shape header and the store's shape from it through milan-fpga's generators, and `make check` refuses a stale one.
- **milan-ctrld** boots as the split platform does (`sw/firmware/ctrl/plat/ctrl_plat.c`):
  - the generated image is verified against its CRC;
  - the application, AECP and contract 2.3's face are composed;
  - the saved state is applied through AECP's owner;
  - the mailbox opens with ADP held, AECP is released, and ADP starts last.

  The board's MAC goes into the image's three mac-derived fields only after the image is verified, so ADP and AECP name one entity. Every unit is compiled with the shape header and `CTRL_HOST_PLATFORM`, milan-fpga#983's host-only row of THE SHAPES. SRP is still not attached (#14).
- **The face's windows** are answered by `src/aecp_far.c` from what the board has:
  - the datapath block answers the outputs' stream info;
  - the gPTP daemon's status block answers GET_AVB_INFO and GET_AS_PATH;
  - the stream counters come from what milan-mediad counts;
  - SET_STREAM_FORMAT is supported for the format the media plane runs;
  - map edits are refused.
- **The stream counters** follow Milan v1.2 Table 5.6 (STREAM_INPUT, 5.3.8.10) and Table 5.4 (STREAM_OUTPUT, 5.3.7.7), in the quadlets of Tables 5.16 and 5.17. Most count 500 ms observation intervals in which their event happened. A STREAM_INPUT's are zeroed when it goes from not bound to bound. milan-ctrld counts those changes per sink in the datapath block, and GET_COUNTERS reads zero until milan-mediad has zeroed for the new binding.
  - STREAM_INPUT 0 (the AAF listener) counts TIMESTAMP_UNCERTAIN from the tu bit and MEDIA_RESET from mr toggles (IEEE 1722-2016 4.4.4.7, 4.4.4.3). It places its stream again on an mr toggle, plays PDUs with tu set without steering, and drops PDUs too far in the future to process.
  - STREAM_INPUT 1 has a CRF receive path (`src/crf_rx.c`), which clocks nothing. It counts the same header events. UNSUPPORTED_FORMAT is a PDU outside the Milan CRF format (Milan v1.2 Table 7.1). MEDIA_LOCKED is 25 stamps in a row on the format's 2 ms grid.
  - STREAM_OUTPUT 0 (the talker) counts all five Table 5.4 counters from the PDUs it sends. Its MEDIA_RESET and TIMESTAMP_UNCERTAIN stay 0, since it never toggles mr nor sets tu.
  - A stream no program counts has none valid.
- **gPTP freshness:** the gPTP daemon's status is live only while its block's update count is seen to move. A first read, a remapped block or a replacement block is a baseline, not evidence. 2 s without a move, whether the block is unchanged, unreadable or stuck mid-write, expires it: one CLOCK_DOMAIN UNLOCKED, asCapable cleared, and no LOCKED until updates resume. Before, every 2 s remap of a stopped publisher's block counted a new LOCKED/UNLOCKED pair.
- **The journal** moves to the KLJ3 layout this pin's store writes, a 640 KiB file. A KLJ2 file from the earlier image is moved aside and read only for its header. `-N none` keeps the journal in memory.
- **Tests**, in `make check` unless noted:
  - `tests/aecp.py` enumerates with a DUT MAC that is not the image's placeholder, so a daemon serving the image's identity unchanged fails it. It also checks ADP's entity_id, each stream's counters as the media block marks them, the zeroing at bind, and CLOCK_DOMAIN against a gPTP publisher that stops, sticks mid-write, or is replaced live or dead.
  - `tests/counters.py` runs the real daemons. A talker on the other end binds both inputs and puts each fault into its AAF and CRF streams, and every counter's change is read back over AECP.
  - `tests/milan-compat/` (`make check-compat`, against a la_avdecc build) is a controller on la_avdecc 4.3.x that prints every compatibility event and warning.

## Evidence

- `make check` passes at the head (2 min 46 s):
  - `config/` current; `test_media` (AAF and CRF PDUs byte by byte, the interval counter, the servo); `test_gptp`;
  - `peer.py`, 26 checks; `aecp.py`, 107; `counters.py`, 63;
  - talker, listener, and the saved-state power-cut suite.
- `make check-compat` with la_avdecc v4.3.1.1 (`6d61a92e`), built twice: once with the library's defaults (its five leniencies on), and once with all five off. Each build ran unbound, and with both inputs bound to running AAF and CRF streams. All four runs: `IEEE17221|Milan`, Milan 1.2, zero compatibility events, warnings or errors. The same program fails the f405a57 build with the owner's error, and fails this head when milan-mediad runs without its CRF receive path.
- The reviewer's probes, run unchanged: the identity mutant (all three substitutions removed) fails the suite's own wrapper on exactly the three identity checks; the pristine daemon passes; the gPTP-stale probe passes (LOCKED unchanged over 6.5 s of a stopped publisher, one UNLOCKED).
- aarch64 cross build of all four programs, milan-mediad against the target's alsa-lib, reproduced by two clean builds:
  - `milan-ctrld` `9fe4012fb698f97295fd14cb4e44e238be2e4427720aaf962dcdf4433cf708b2`
  - `milan-mediad` `162c8223b2ffc52484381e061e691e1467815294ba502a2637d4d27ac59a8b77`
  - `milan-dp` `0c61cd3e0d4a3f2a6b72187ad31d22c5be4b8419e6a6710e514e6a58584a86f8`
  - `milan-gptp-watch` `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` (unchanged)
  - `entity.conf` `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c` (unchanged)

  The datapath and media blocks changed layout, so milan-ctrld, milan-mediad and milan-dp are installed together.

## Not met, not claimed

- The CRF input is received and counted, but nothing clocks from it. AECP accepts selecting it as clock source, and the media plane does not follow.
- The talker never sets tu: on a gPTP step it starts a new stream instead.
- ADD/REMOVE_AUDIO_MAPPINGS answer NOT_SUPPORTED, because the media plane has no channel router.
- SET_STREAM_FORMAT takes only the running format.
- GET_AS_PATH is answered only where the gPTP daemon determines the path.
- No SRP; REBOOT is NOT_IMPLEMENTED.
- The host shape row is a stated scheduling profile, not a measurement on the board.
- Milan v1.3 is not among the specifications used: the counters follow v1.2. AECP-STATUS.md states what the controller's v1.3 citation implies.
- Nothing from this head was installed on a board, and the Buildroot image build has not been run with the published archive.

Target: `pb2-aecp-1 -> pb2-tsn`.
