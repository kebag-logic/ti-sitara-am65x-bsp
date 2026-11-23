[A830] REVIEW READY

Commit: `cce8c9a4143548c80800b2def821431ff45e3698`, `pb2-aecp-1` -> `pb2-tsn`. Local, not pushed.
milan-fpga: `677d605f37c3a80654df1dd2cde053b57242845b`, `pf-linuxprof-1` (kebag-logic/milan-fpga#983). Local, not pushed. The pin to it is **provisional**: no archive sha256 until it is published.

Changed:
- milan-ctrld now serves milan-fpga's AECP, unchanged. It boots as the split platform does:
  - the generated image is verified against its CRC;
  - AECP and its face are composed;
  - the saved state is applied through AECP's owner;
  - ADP starts last.
- The host AEM store is generated from the board's own config, `milan-linux/config/endstation_pb2_bridge.yaml`, using the host profile:
  - one AVB_INTERFACE;
  - STREAM_INPUT 0 AAF 48 kHz 8 ch 32-bit, STREAM_INPUT 1 CRF, STREAM_OUTPUT 0 AAF;
  - ADP 1 source and 2 sinks;
  - entity_model_id pinned `0x001BC50AC1000100`.
- The board's MAC goes into the image's mac-derived fields only after the generated image is verified.
- The face's windows are answered from the board's datapath, gPTP and media blocks.
- The journal moves to the KLJ3 layout.

Validation:
- `make check` rc 0: `config/` current, `test_media`, `test_gptp`, peer 27/27, aecp 87/87, talker, listener, nvm.
- The aecp test, on a veth pair, covers:
  - READ_DESCRIPTOR of all 32 descriptors;
  - GET_MILAN_INFO, ACQUIRE (NOT_SUPPORTED) and LOCK;
  - the Milan v1.2 5.4 set the firmware implements;
  - GET_COUNTERS over known values;
  - BIND_RX on STREAM_INPUT 0 and STREAM_INPUT 1.
- aarch64 cross build: `milan-ctrld` sha256 `04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb`, reproduced by two clean builds; `entity.conf` sha256 `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c`.

Acceptance: met, except as listed under the open risks. Nothing was installed on a board.

Open risks and questions:
- The CRF input binds and is counted among the sinks, but the media plane does not clock from it; AECP accepts it as clock source anyway. **No Milan conformance is claimed.** `milan-linux/AECP-STATUS.md` lists every gap: map edits NOT_SUPPORTED, one stream format, AS path only where the gPTP daemon determines it, TIMESTAMP_UNCERTAIN not counted, no SRP, REBOOT not implemented, timing from a stated profile.
- The pin's sha256 and the Buildroot hash line wait for the published archive. The Buildroot build is unverified until then.
