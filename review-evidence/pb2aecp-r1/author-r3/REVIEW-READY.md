# [A830] Round 3 review handoff: BSP #29 / PR #30, the delta from f405a57

| Repository | Branch | Base | Head | Target |
| --- | --- | --- | --- | --- |
| BSP | `pb2-aecp-1` | `f405a5754afbe30313116be4662adb9d79567f9e` | `49b2035605729e74d1fc664fb25304402e758ea4` | `pb2-tsn` |

Two local commits, not pushed: `65fc798b` (the code and its tests) and
`49b20356` (the la_avdecc check and the docs). milan-fpga's pin is unchanged
(`677d605f`, PR #986's published head). No board, bench or remote host was
touched. Receipts: [receipts/INDEX.md](receipts/INDEX.md). Full PR body:
[PR-BODY.md](PR-BODY.md).

## R901-1 findings

| Finding | Resolution | Where | Check |
| --- | --- | --- | --- |
| F1 MAJOR, stale gPTP data creates CLOCK_DOMAIN transitions | Fixed at the cause. The status is live only while the block's `updates` is seen to move. A first read, a remapped block and a replacement block are baselines. 2 s without a move expires it, whether the block is unchanged, unreadable or has an odd `seq`; the old code skipped that check on read failures. Expiry is one UNLOCKED, asCapable cleared, and no LOCKED until a move. The grandmaster the control plane announces is left as it was | `src/softfab.c` `poll_gptp`, `src/softfab.h` | `tests/aecp.py` `check_gptp_faults`: stopped publisher (6.5 s), `seq` left odd, replaced by a live block, replaced by a dead block, each with exact LOCKED/UNLOCKED deltas and the AVB_INFO flags. R901's `gptp-stale-test`: rc 0 (LOCKED 5 before and after, UNLOCKED 4 to 5). flexptpd republishes every 31 ms heartbeat, so the 2 s rule holds on the board |
| F2 MINOR, enumeration does not test runtime identity substitution | The DUT MAC is `02:1a:2b:3c:4d:5e`. The test reads the placeholder from the config and checks the DUT's differs. ENTITY, AVB_INTERFACE (MAC, clock identity) and now ADP's entity_id are checked against it | `tests/run-netns-aecp.sh`, `tests/aecp.py` | R901's identity mutant, run with the suite's own wrapper: rc 1, exactly the three identity checks fail. The pristine daemon: rc 0 |
| F3 MAJOR, the completion claim is ahead of frozen acceptance | "Closes #29" is gone; the body says Refs #29 and that it does not close it. Each of #29's four criteria is stated with its state: Pin **open** (PR head, not a dev-soft-core merge; firmware gate partial), Host test met, Board **open** (the owner's controller found the counters error on the f405a57 build; this head is not installed), Install **partly met** (Buildroot image not built with the archive; manual install list in receipts) | `PR-BODY.md`, `milan-linux/AECP-STATUS.md` "Acceptance (bsp#29)" | read against #29's acceptance |
| R1 RESIDUE, review-round reference in a source comment | `# The stream inventory (bsp#29):` | `config/endstation_pb2_bridge.yaml:15` | `make check-config`: current |
| R2 RESIDUE, archive availability prose | The README paragraph and AECP-STATUS's opening pin paragraph now say what the reviewer gave. The final paragraph reads "The Buildroot image build has not been run with the published archive." | `milan-linux/README.md`, `milan-linux/AECP-STATUS.md` | read against the pin |

## The round-3 tasks

1. **STREAM_INPUT 0, TIMESTAMP_UNCERTAIN.** The AAF listener reads tu and mr
   from each PDU (`aaf.h`, `aaf.c`). TIMESTAMP_UNCERTAIN and MEDIA_RESET count
   per Milan v1.2 Table 5.6: observation intervals in which tu was set, or mr
   toggled. IEEE 1722.1-2021 7.4.42.2.4 (Table 7-157) counts the same events
   per PDU and per toggle. The listener's own jumps are no longer called
   MEDIA_RESET. They interrupt the playback, so they count STREAM_INTERRUPTED,
   once for a run of them, and stay reported raw as `jumps`. An mr toggle places
   the stream again (IEEE 1722-2016 4.4.4.3). PDUs with tu set are played
   without steering (4.4.4.7). PDUs past the EARLY bound are not played. All
   ten counters are reported valid (0xF3F).
2. **STREAM_INPUT 1, CRF.** milan-mediad has a CRF receive path
   (`src/crf.c`, `src/crf_rx.c`, role `crf`, on by default). It receives the
   bound CRF stream and counts all ten counters. MEDIA_LOCKED is 25 stamps in a
   row on the 2 ms grid of the Milan format; it never clocks the media. Without
   the role, or with milan-mediad absent, STREAM_INPUT 1 has none valid.
   Both inputs' counters are zeroed when the input goes from not bound to bound
   (Milan v1.2 5.3.8.10), through a per-sink bind count in the datapath block.
   GET_COUNTERS reads zero until milan-mediad has zeroed for the new binding.
   STREAM_OUTPUT 0 now has all five counters of Milan v1.2 Table 5.4 (MEDIA_RESET
   and TIMESTAMP_UNCERTAIN from the bits it sends, both 0), which la_avdecc's
   Milan 1.2 check requires once the STREAM_INPUT error is gone.
   Milan v1.3 is not in the standards here, so v1.2 5.3.8.10 / Table 5.6 was
   used. la_avdecc 4.3.1.1 cites "Milan 1.3 - 5.3.8.10" and requires the same
   ten STREAM_INPUT counters at the same bits, so no v1.3 difference shows for
   STREAM_INPUT. For STREAM_OUTPUT, v1.3 entities (GET_MILAN_INFO
   specification_version 1.3 or later) are judged by the IEEE 1722.1 layout.
   This entity is not one.
3. **Acceptance as the controller sees it.** `tests/milan-compat/` is a
   controller on la_avdecc v4.3.1.1 (`6d61a92e`), built locally twice: with the
   library's defaults, and with its five leniencies off. It runs in
   `make check-compat`, unbound and with both inputs bound to running AAF and
   CRF streams. All four runs are Milan 1.2 compatible, with zero compatibility
   events, warnings or errors. The full output is in the receipts. It fails the
   f405a57 build with the owner's exact error, and fails this head without the
   CRF path, so it can fail. Nothing is left that could not be fixed.
4. **make check, the veth suites, the aarch64 build.** `make check` rc 0
   (peer 26, aecp 107, counters 63, unit tests, talker, listener, nvm). The
   aarch64 build of all four programs is reproduced by two clean builds.
   `milan-ctrld` is `9fe4012fb698f97295fd14cb4e44e238be2e4427720aaf962dcdf4433cf708b2`
   and `milan-mediad` is `162c8223b2ffc52484381e061e691e1467815294ba502a2637d4d27ac59a8b77`.
   The install list is in the receipts. milan-ctrld, milan-mediad and milan-dp
   go in together, since the datapath and media block layouts changed.

## Validation at the head

| Check | Result |
| --- | --- |
| `make check` | rc 0 |
| `make check-compat`, library defaults and leniencies off | rc 0, rc 0 |
| the compat program at f405a57; at the head without `crf` | FAIL, FAIL (the expected clause) |
| R901 probes | identity mutant rc 1 (3 identity checks), pristine rc 0, stale rc 0 |
| aarch64 cross build twice | identical |
| `git diff --check f405a57..HEAD` | clean |

## Open risks and questions

- Board behaviour is unverified: the CRF thread shares the isolated media core,
  one priority below the listener, and has never run on the board.
- The 500 ms observation interval and the CRF lock rule (25 stamps, 1 us per
  step) are implementation choices Milan leaves open. They are stated in
  AECP-STATUS.md.
- milan-mediad was cross-built against alsa-lib 1.2.15.3's headers. It links
  `libasound.so.2` dynamically and uses only ALSA_0.9 symbols. The Buildroot
  image's alsa-lib was not checked.
- Pin and Board acceptance stay open, as above.
