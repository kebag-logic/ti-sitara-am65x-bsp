Status: REVIEW READY (13:31 CET). Every task of the round is done and verified at the head. Nothing is pushed, no board or remote host was touched, and the [A830] REVIEW READY comment is posted on #29.
ETA: done for this round. What remains needs others: the manager publishing the head, editing the PR body (PR-BODY.md), installing the three binaries on the board and enumerating it with the owner's controller; #986's merge and the pin's move; the full firmware gate; review.

[A830], round 3. Checkout `$LANES/pb2-aecp`, branch `pb2-aecp-1`.

| | |
| --- | --- |
| Base | `f405a5754afbe30313116be4662adb9d79567f9e` (= the published `pb2-aecp-1`) |
| Head | `49b2035605729e74d1fc664fb25304402e758ea4` (tree `2a5536586e8c3b81a2c40a61810c85367e56eaf9`) |
| Commits | `65fc798b` the counters, the CRF receive path, gPTP freshness, the distinct DUT MAC; `49b20356` the la_avdecc check, AECP-STATUS, READMEs |
| Packet | [REVIEW-READY.md](REVIEW-READY.md), [PR-BODY.md](PR-BODY.md), [receipts/INDEX.md](receipts/INDEX.md), [receipts/aarch64/](receipts/aarch64/SHA256SUMS) |
| Comment | https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7101193761 ([text](receipts/comment-bsp-29.md)) |

## Done

1. **STREAM_INPUT 0.** TIMESTAMP_UNCERTAIN comes from the tu bit, MEDIA_RESET from mr toggles, counted per Milan v1.2 Table 5.6's observation intervals. The listener places the stream again on a toggle. aecp_far reports all ten valid (0xF3F).
2. **STREAM_INPUT 1 (CRF).** A CRF receive path in milan-mediad (`src/crf_rx.c`) counts the ten counters as they apply to CRF, and never clocks the media. Without it, the input has none valid. Both inputs are zeroed at bind. STREAM_OUTPUT 0 has all five of Table 5.4.
3. **la_avdecc v4.3.1.1** was built locally twice (defaults; leniencies off). `tests/milan-compat/` (`make check-compat`) passes in all four runs (unbound, and with bound, running AAF and CRF streams): Milan 1.2, zero compatibility events, warnings or errors. It reproduces the board's error on the f405a57 build, and fails this head without the CRF path. Nothing is left unfixable. Milan v1.3 is not in $WORKSPACE_HOME/standards: v1.2 5.3.8.10 was used, and AECP-STATUS states what the controller's v1.3 citation implies.
4. **Builds and tests.** `make check` rc 0 (peer 26, aecp 107, counters 63, units, talker, listener, nvm). The aarch64 build of all four programs is reproduced twice. milan-mediad links the target's `libasound.so.2`; alsa-lib 1.2.15.3 was built in scratch for its headers.
5. **R901-1.** F1 is fixed at the cause and its fault test passes, the reviewer's stale probe too. F2: the reviewer's identity mutant fails the suite. F3: the claims match what ran, and the acceptance rows are stated: Pin open, Host met, Board open, Install partly met. R1 and R2 are applied as written.

## For the manager's install

Install `milan-ctrld`, `milan-mediad` and `milan-dp` **together**, then restart the bridge: the datapath block (v2) and the media block (v3) changed layout. The list and checksums are in receipts/INDEX.md. After the install, `milan-dp` should show `milan_stream_input0=valid:f3f`, `milan_stream_input1=valid:f3f` and `milan_stream_output0=valid:01f`.

## Scratch

`$VALIDATION_STORAGE/pb2aecp-a830`: `lavdecc/` (source, `build/`, `build-strict/`), `milan-compat*` binaries, `sysroot-aarch64/` (alsa-lib), `cross-r3-1/` and `-2/`, `check-r3/`, `r901-probes/`, `base-f405a57/` (the negative control's build).

REVIEW READY 49b2035605729e74d1fc664fb25304402e758ea4 9fe4012fb698f97295fd14cb4e44e238be2e4427720aaf962dcdf4433cf708b2 162c8223b2ffc52484381e061e691e1467815294ba502a2637d4d27ac59a8b77
