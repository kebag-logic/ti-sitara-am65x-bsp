The board advertised AEM_SUPPORTED, but milan-ctrld served no AECP, so a controller could not read the Configuration descriptor IEEE 1722.1-2021 7.2.1 requires. This branch makes milan-ctrld serve milan-fpga's AECP, unchanged, from a host AEM store generated from the board's own end-station config.

Closes #29 for the control plane. **No Milan conformance is claimed for the board**: the media plane does not clock from the CRF input. `milan-linux/AECP-STATUS.md` lists what is met and what is not.

## What changed

- **The pin** moves to milan-fpga `677d605f`, the head of `pf-linuxprof-1` (milan-fpga#983, the descriptor-only host profile). It was published as milan-fpga PR #986, and `MILAN_FPGA_SHA256` is its GitHub archive's sha256 (`e9a782af…`); `tools/fetch-milan-fpga.sh` fetches and verifies it without a local checkout. Once #986 merges, the pin moves to its merge commit. The pin also names tsn-c-stack, where ADP, ACMP, MAAP and the wire layer now live, and lwSRP, whose headers AECP's bridge includes, each with its published archive's sha256. The Buildroot package unpacks both under `third_party/`. Its hash file carries the archive's line.
- **The board's config**, `config/endstation_pb2_bridge.yaml`, is milan-fpga's host profile (`board.target: host`, `gptp_owner: host-phc`). It has one AVB_INTERFACE; STREAM_INPUT 0 AAF 48 kHz 8 ch 32-bit; STREAM_INPUT 1 CRF; STREAM_OUTPUT 0 AAF in the same format; no second AAF and no CRF output; ADP 1 source and 2 sinks. It carries the specified entity name and the entity_model_id pinned to `0x001BC50AC1000100` (Milan v1.2 7.2.2, 7.2.3; IEEE 1722.1-2021 7.2.1). The AVB_INTERFACE dataset is the board's gPTP daemon's at its pin. `tools/gen-config.sh` generates `entity.conf`, the AEM image header, the shape header and the store's shape from it through milan-fpga's generators. `make check` refuses a stale one.
- **milan-ctrld** boots as the split platform does (`sw/firmware/ctrl/plat/ctrl_plat.c`):
  - the generated image is verified against its CRC;
  - the application, AECP and contract 2.3's face are composed;
  - the saved state is applied through AECP's owner;
  - the mailbox opens with ADP held, AECP is released, and ADP starts last.

  The board's MAC goes into the image's three mac-derived fields only after the generated image is verified, so ADP and AECP name one entity. Every unit is compiled with the shape header and `CTRL_HOST_PLATFORM`, milan-fpga#983's host-only row of THE SHAPES. SRP is still not attached (#14).
- **The face's windows** are answered by `src/aecp_far.c` from what the board has. The datapath block answers the outputs' stream info. The gPTP daemon's status block answers GET_AVB_INFO and GET_AS_PATH. The media block answers the stream counters. SET_STREAM_FORMAT is supported for the format the media plane runs. Map edits are refused.
- **The journal** moves to the KLJ3 layout this pin's store writes, a 640 KiB file. A KLJ2 file from the earlier image is moved aside and read only for its header. `-N none` keeps the journal in memory.
- **Tests:** `tests/aecp.py` with `tests/run-netns-aecp.sh`, part of `make check`. Round 1's prerequisite probe is removed; its finding is resolved.

## Evidence

- `make check` passes at the head:
  - `config/` current; `test_media`; `test_gptp`;
  - `peer.py`, 27/27;
  - `aecp.py`, 87/87;
  - talker, listener, and the saved-state power-cut suite, with bindings and the rest restored through AECP's owner.
- `aecp.py` covers, on a veth pair:
  - READ_DESCRIPTOR of all 32 descriptors CONFIGURATION 0 lists, field by field where the inventory matters;
  - GET_MILAN_INFO, ACQUIRE (NOT_SUPPORTED) and LOCK;
  - the Milan v1.2 5.4 command set the firmware implements, and an unsolicited notification;
  - GET_COUNTERS over known values;
  - BIND_RX on STREAM_INPUT 0 and STREAM_INPUT 1, each settling;
  - the journal move.
- aarch64 cross build (`make CC=aarch64-linux-gnu-gcc`): `milan-ctrld` sha256 `04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb`, reproduced by two clean builds. `entity.conf` sha256 `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c`.

## Not met, not claimed

- The CRF input binds and is counted among the sinks, but nothing clocks from it. AECP accepts selecting it as clock source, and the media plane does not follow. Its GET_COUNTERS reports no counter valid.
- ADD/REMOVE_AUDIO_MAPPINGS answer NOT_SUPPORTED, because the media plane has no channel router.
- SET_STREAM_FORMAT takes only the running format.
- GET_AS_PATH is answered only where the gPTP daemon determines the path.
- TIMESTAMP_UNCERTAIN is not counted.
- No SRP; REBOOT is NOT_IMPLEMENTED.
- The host shape row is a stated scheduling profile, not a measurement on the board.
- Nothing was installed on a board, and the Buildroot image build has not been run with the published archive yet.

Target: `pb2-aecp-1 -> pb2-tsn`.
