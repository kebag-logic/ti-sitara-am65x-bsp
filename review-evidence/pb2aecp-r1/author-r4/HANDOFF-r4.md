Status: REVIEW READY (2026-11-23). Every task of the round is done and verified at the head. Nothing is pushed, no board, bench or remote host was touched, `$LANES/linux-profile` was not touched, and the [A830] REVIEW READY comment is posted on #29.
ETA: done for this round. What remains needs others: the manager publishing the head, editing the PR body (PR-BODY.md) and installing the three binaries on the board; the owner's enumeration of the board; #986's merge (its `ctrl_loop.h` fix) and the pin's move; the full firmware gate; review.

[A830], round 4. Checkout `$LANES/pb2-aecp`, branch `pb2-aecp-1`.

| | |
| --- | --- |
| Base | `afc350641dc2d93cee39bb4c62258b6d55f00ccb` (= the published `pb2-aecp-1`) |
| Head | `ed21f9b36950e405e402695185e712c0a92b116a` (tree `672aa8b2f981fb0ed14662174a678650732738cf`) |
| Commits | `88cddb6d` the reset at each binding, validity after receive setup, early PDUs interrupt, their tests; `ad9c0f09` AECP-STATUS and README; `ed21f9b3` the suites' capture filtered to control frames, a settle check |
| Pin | milan-fpga `66b4def3`, unchanged |
| Packet | [REVIEW-READY.md](REVIEW-READY.md), [PR-BODY.md](PR-BODY.md), [receipts/INDEX.md](receipts/INDEX.md), [receipts/aarch64/](receipts/aarch64/SHA256SUMS) |
| Comment | https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7103250160 ([text](receipts/comment-bsp-29.md)) |

## Done

1. **R901-F4.** Each BINDING.BOUND edge in the publication block is seen in the firmware write that makes it (`softfab.c` `bound_edges`). That write comes before the ACMP response. milan-ctrld updates `bound` and `binds` there, so a burst in one pass counts every binding. GET_COUNTERS compares the counts live, not from the 100 ms poll. A restart carries the counts, so a restored binding is new. milan-mediad drains its socket at a new binding. The reviewer's burst probe passes. The suite's burst covers both inputs, repeated queued transitions, and GET_COUNTERS before milan-mediad resumes, plus five immediate rebinds and the saved-state count. All of it fails at `afc35064`, and three mutants fail too (cache 7, drain 1, carry 2).
2. **R901-F5.** Validity is published only after `rxsock_open()` succeeds. A failure closes the fd, publishes none valid and is logged once with its step, and milan-mediad runs on. `tests/rxfail.py` fails each of the five steps in each input via an LD_PRELOAD shim, plus a control and the missing interface. The reviewer's startup probe passes.
3. **R901-F6.** The PR body gives `entity.conf` `532966e8…`, regenerated at the re-pin with only the comment changed, and attributes `e273ff97…` to `49b2035`.
4. **R901-F3.** The rows as they stand: Pin open (#986 in review, `ctrl_loop.h` fix in progress), Host met, Board open (49b2035 installed; a controller reads 10/10/5 valid; the owner's enumeration not recorded; this head not installed), Install partly met. Nothing is claimed for the landing or the enumeration.
5. **Beyond the findings.** PDUs past the early bound now run the interruption check on both inputs. R901-2's early-only CRF check was timing dependent here: 2 of 3 failed on `afc35064`.
6. **Validation.** `make check` rc 0 twice (peer 26, aecp 107, counters 104, rxfail 44, nvm 7). `check-compat` rc 0 with library defaults and with leniencies off (4 runs, zero events). The R901-1 and R901-2 probes all behave as required. aarch64 build twice, identical. Two failed `make check` runs at `ad9c0f09` are kept and explained in the receipts (the listener rate bound; a capture miss, fixed by `ed21f9b3`).

## For the manager's install

Install `milan-ctrld`, `milan-mediad` and `milan-dp` together: stop the bridge, replace the three, start it. Block layouts are unchanged (datapath 2, media 3). `milan-gptp-watch`, `entity.conf` (its comment only) and `milan-bridge.sh` need not change. After the start, `milan-dp` should show `valid:f3f`, `valid:f3f`, `valid:01f`. The list and checksums are in receipts/INDEX.md.

## Scratch

`$VALIDATION_STORAGE/pb2aecp-a830/r4`: `fpga/` (the pinned 66b4def3 tree), `head-src/` (the head's export), `base-src/` and `base/` (afc35064 and its build), `check-1/`, `check-2/`, `compat-default/`, `compat-strict/`, `cross-1/`, `cross-2/`, `mut/` (the three mutants), `lavdecc-build/` (library defaults, rebuilt), `r901-probes-*`, `r901-prior-head/`, `logs/`. The in-checkout `milan-linux/.milan-fpga` is still the old `677d605f` tree (untracked); nothing here used it.

REVIEW READY ed21f9b36950e405e402695185e712c0a92b116a b1254bb0de7c27f5f9b5a3fb2b3731f1828c0d213e04a7b8d51220cc650bb6b4 8bef6f7693e34341c80a4e7138bdca1d8790d61d337850a47e43a048ec833f27
