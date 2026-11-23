# [A830] Round 2 review handoff: AECP on the board (#29) and milan-fpga #983

| Repository | Branch | Head | Target |
| --- | --- | --- | --- |
| BSP | `pb2-aecp-1` | `cce8c9a4143548c80800b2def821431ff45e3698` | `pb2-tsn` |
| milan-fpga | `pf-linuxprof-1` | `677d605f37c3a80654df1dd2cde053b57242845b` | `dev-soft-core` (packet in [linuxprof/](linuxprof/REVIEW-READY.md)) |

Both are local commits; nothing is pushed. Nothing was installed on a board.
The BSP's pin to milan-fpga is **provisional** (local tree, no archive sha256)
until `677d605f` is published.

## BSP changes (`cce8c9a4` on `382f1221`)

- `milan-linux/milan-fpga.pin`, `tools/fetch-milan-fpga.sh`,
  `br2-external/package/milan-fpga-src/`: the provisional pin; tsn-c-stack and
  lwSRP fetched and hash-checked where their gitlinks sit.
- `milan-linux/config/endstation_pb2_bridge.yaml` plus the four generated files
  (`entity.conf`, `aecp_entity_gen.h`, `srp_entity_gen.h`, `nvm_shape_gen.h`)
  and `tools/gen-config.sh` with `--check`.
- `milan-linux/src/milan_ctrld.c`: the split platform's boot with AECP and its
  face, the verified image with the board's identity, consistency refusals.
- `milan-linux/src/aecp_far.c`, `.h`: the face's windows answered from the
  system.
- `milan-linux/src/softfab.c`, `.h`: the gPTP status kept for the far side.
- `milan-linux/src/nvm_file.c`, `.h`: the KLJ3 journal, the KLJ2 file moved
  aside, the in-memory journal.
- `milan-linux/Makefile`: the split image's unit set, the shape header and
  `CTRL_HOST_PLATFORM` in every unit, `check-config`, the AECP test.
- `milan-linux/tests/aecp.py`, `run-netns-aecp.sh`; round 1's probe removed.
- `milan-linux/README.md`, `AECP-STATUS.md`, `README.pb2-tsn.md`, `avb.env`.

## Validation (BSP head)

| Check | Result |
| --- | --- |
| `make check` | rc 0: `config/` current, `test_media`, `test_gptp`, peer 27/27, aecp 87/87, talker, listener, nvm |
| aarch64 cross build, `make CC=aarch64-linux-gnu-gcc` | `milan-ctrld` `04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb`, the same from two clean builds |
| `git diff --check` | clean |

## Acceptance (round 2 instructions for #29)

| Item | Status |
| --- | --- |
| Pin to the `pf-linuxprof-1` head | met, provisional (no archive sha256 yet) |
| AECP in milan-ctrld as the bare-metal build uses it, host AEM store fed by the generated image | met; deviations stated: no SRP (#14), no receive-monitor contexts (no AAF input above 0), no reset port, the daemon's own pool |
| Board config through the profile, never restated, with the specified inventory, name and pinned model id | met |
| CRF input binds and counts | binds and is counted among the sinks; GET_COUNTERS answers with none valid; milan-mediad does not clock from it: recorded, no Milan conformance claimed |
| `make check`; veth enumeration (READ_DESCRIPTOR of all, GET_MILAN_INFO, ACQUIRE/LOCK, 5.4 set, BIND_RX on SI 0 and 1) | met |
| aarch64 cross build of `milan-ctrld` and the new `entity.conf` | met |
| No board install | met |

Open risks and questions:

- The pin must move to the published archive's sha256. The Buildroot build is
  unverified until then.
- The host shape row's figures are a stated profile, not measured on the board.
- The listed Milan gaps (AECP-STATUS.md) are known and not fixed here.
- milan-fpga's firmware gate shards and its gate 1b did not complete within
  this session's command bound (linuxprof receipts).
