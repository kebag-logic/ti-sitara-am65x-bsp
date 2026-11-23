Status: REVIEW READY, both steps. The BSP's pin to milan-fpga is provisional: no archive sha256 until `677d605f` is published.
ETA: done for this round. What remains needs others: the manager pushing `pf-linuxprof-1` (then the pin's sha256 and the Buildroot hash line), the hosted gates, review.

[A830], round 2. Nothing is pushed. No board was contacted or installed.

| Step | Repository, branch | Head | Packet |
| --- | --- | --- | --- |
| 1, milan-fpga #983 | `$LANES/linux-profile`, `pf-linuxprof-1` from `dev-soft-core` 854bd723 | `677d605f37c3a80654df1dd2cde053b57242845b` | [linuxprof/](linuxprof/REVIEW-READY.md) |
| 2, BSP #29 | `$LANES/pb2-aecp`, `pb2-aecp-1` from 382f1221 | `cce8c9a4143548c80800b2def821431ff45e3698` | [REVIEW-READY.md](REVIEW-READY.md), [PR-BODY.md](PR-BODY.md), [receipts/](receipts/INDEX.md) |

## Step 1: milan-fpga #983

- The builder has a descriptor-only host profile, `board.target: host`
  (`board.gptp_owner: host-phc`, `link_mbps`, the stated gPTP dataset, the
  `usb_audio` family). It feeds `emit_aem_overlay`, the AEM image,
  `adp_entity.py`, `aecp_entity.py` and `srp_entity.py`. Every shared check is
  unchanged, and `build()` refuses the profile by name. An FPGA target naming
  `gptp_owner` is refused.
- `ctrl_loop.h` has a `(1, 1, 2)` row under `CTRL_HOST_PLATFORM` only, with the
  1x1/1if figures as a stated host scheduling profile. FPGA rows and the
  `#error` are untouched, and no build input in the tree defines the macro
  (gate 41 holds that). `endstation_arty_current`'s all-fabric shape is also
  (1, 1, 2) and stays refused without the macro.
- Results:
  - identity: 170/170 FPGA artifacts byte-identical, tracked tree clean;
  - gates 41 and 42: profile, generators, shape row, section 6 key map, 28/28
    planted defects;
  - builder: 114/115 gates passed once each;
  - docs and quality: 24/24, including the bare-metal scope gate;
  - `ctrl_loop.h`: token-identical at base and head;
  - firmware arms: 21 + 18 + 8.
- Not completed: gate 1b, `ci_builder_legs.py`'s own union, and the firmware
  shards and mutation campaigns. Each needs more than the 10-minute bound on a
  foreground command; detaching was refused.
- Material assumption, stated publicly: the tree is bare-metal only (#259), so
  the profile uses the tree's host-program vocabulary and no false
  `fabric_gptp` key.

## Step 2: BSP #29

- The pin is `677d605f`, **provisional**, built from the local tree. The
  tsn-c-stack and lwSRP archives are published and hash-checked.
- `config/endstation_pb2_bridge.yaml` uses the host profile with the ruled
  inventory, the entity name and the pinned `0x001BC50AC1000100`.
  `tools/gen-config.sh` generates `entity.conf`, the AEM image, the shape
  header and the store's shape.
- milan-ctrld boots as the split platform: the image verified, then the board's
  identity in its copy, AECP and its face, the saved state through AECP's
  owner, ADP last. `src/aecp_far.c` answers the face from the system. The
  journal moves to KLJ3.
- `make check` rc 0: peer 27/27, aecp 87/87, plus the unit, talker, listener
  and nvm suites.
- aarch64 cross build: `milan-ctrld` `04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb`
  (receipts/aarch64/).
- The CRF input binds and is counted among the sinks, but milan-mediad does not
  clock from it. That is recorded in `AECP-STATUS.md`, with no Milan
  conformance claimed, alongside the other gaps: map edits NOT_SUPPORTED, one
  stream format, AS path limited, no SRP, REBOOT not implemented.

## Issue comments

- BSP #29: [A830] REVIEW READY, https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7099190088 ([text](receipts/comment-bsp-29.md))
- milan-fpga #983: [A830] REVIEW READY, https://github.com/kebag-logic/milan-fpga/issues/983#issuecomment-7099191077 ([text](linuxprof/receipts/comment-milan-fpga-983.md))

REVIEW READY 677d605f37c3a80654df1dd2cde053b57242845b cce8c9a4143548c80800b2def821431ff45e3698 04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb
