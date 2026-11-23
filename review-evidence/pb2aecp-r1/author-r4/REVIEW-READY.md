# [A830] Round 4 review handoff: BSP #29 / PR #30, the delta from afc35064

| Repository | Branch | Base | Head | Target |
| --- | --- | --- | --- | --- |
| BSP | `pb2-aecp-1` | `afc350641dc2d93cee39bb4c62258b6d55f00ccb` | `ed21f9b36950e405e402695185e712c0a92b116a` (tree `672aa8b2f981fb0ed14662174a678650732738cf`) | `pb2-tsn` |

Three local commits, not pushed: `88cddb6d` (the code and its tests), `ad9c0f09` (AECP-STATUS,
README), `ed21f9b3` (the suites' capture, a settle check). milan-fpga's pin is unchanged
(`66b4def3`, PR #986's round-2 head, archive sha256 `c5b3b01c…`). No board, bench or remote host
was touched. Receipts: [receipts/INDEX.md](receipts/INDEX.md). Full PR body: [PR-BODY.md](PR-BODY.md).

## R901-2 findings

| Finding | Resolution | Where | Check |
| --- | --- | --- | --- |
| F4 MAJOR: an accepted unbind/rebind burst misses the counter reset | Fixed at the owner boundary. The soft fabric compares each sink's BINDING.BOUND in the publication block after every firmware write. ACMP writes it before its BIND_RX or UNBIND_RX response (MAILBOX_CONTRACT; the processor's `acmp_bound_o`). Each edge goes to milan-ctrld's `sink_bound()`, which updates `bound` and `binds` in the datapath block inside that write, so every binding is counted however many one service pass makes. `publish_bound()`'s once-a-pass level sample is gone. GET_COUNTERS now compares the bind counts as it answers (`input_counters()`), not from the copy polled every 100 ms, so a new binding reads zero from the moment it is made. A restarted milan-ctrld carries each sink's count from the block it takes over, so a binding restored at boot moves past the count milan-mediad last zeroed for. milan-mediad drains its socket when it takes up a new binding, so the old binding's queued PDUs are not counted for the new one | `src/softfab.c` `bound_edges`, `src/softfab.h`, `src/milan_ctrld.c` `sink_bound`, `src/aecp_far.c` `input_counters`, `src/datapath.c` `milan_dp_create`, `src/listener.c`, `src/crf_rx.c` | `counters.py` burst: both daemons held; UNBIND/BIND ×2 on input 0, ×1 on input 1, queued; GET_COUNTERS queued with them (judged by the order of the answers), and at once after the last BIND_RX response with milan-mediad still held; `binds` +2/+1; zero while held and after resume. Five immediate rebinds: GET_COUNTERS sent on seeing the BIND_RX response. Saved-state suite: restored binding `binds 1 -> 2`, unbound restart keeps it. R901-2's burst probe: rc 0. At `afc35064` all of these fail, and three mutants of the head fail (cache 7, drain 1, carry 2) |
| F5 MINOR: failed AAF receive initialization leaves all counters valid | Fixed at the cause. Both receiving threads set up their socket through `rxsock_open()` (interface, socket, filter, bind, receive timeout). They publish `MILAN_SI_VALID` only once that succeeds. A failure closes the descriptor, publishes none valid, and records the step and errno. The main loop logs it once (`listener on <if>: <step>: <error>; STREAM_INPUT 0 is not received, none of its counters valid`). milan-mediad runs on with its other streams, with no retry, as stated in AECP-STATUS | `src/rxsock.c`, `src/rxsock.h`, `src/listener.c`, `src/crf_rx.c`, `src/milan_mediad.c` | `tests/rxfail.py` (in `make check`): each of the five steps made to fail in each input by an LD_PRELOAD library (`tests/rxfail.c`, by thread name). The failed input reads none valid and the other `0xf3f`; milan-mediad is alive; the step is logged; the opened fd is closed. Also a control with nothing failing, and the missing interface (both none valid). R901-2's startup probe: rc 0 |
| F6 MINOR: the PR gives the previous config checksum as unchanged | Corrected in the body. `entity.conf` at the head is `532966e8…`, regenerated at the re-pin, with only the revision comment changed. `e273ff97…` is the `49b2035` file, generated at `677d605f`. Earlier receipts are attributed to `49b2035`/`677d605f` | `PR-BODY.md` Evidence | `sha256sum` of the file at `49b2035`, `afc35064` and the head; `git diff 49b2035 afc3506 -- milan-linux/config/entity.conf` shows the one comment line |
| F3 MAJOR (retained in part): the acceptance rows | Stated as they stand. Pin **open**: #986 is in review with a `ctrl_loop.h` fix in progress, not landed. Host met. Board **open**: the 49b2035 build is installed, a controller reads all ten input counters valid on both inputs and five on the output, and the owner's enumeration is not recorded. This head is not installed. Install **partly met**: the documented manual install was used for the 49b2035 build, and the image build has not been run. Nothing is claimed for #986's landing or the board enumeration | `PR-BODY.md` Acceptance, `milan-linux/AECP-STATUS.md` "Acceptance (bsp#29)" | read against #29's acceptance |

## Beyond the findings

- **PDUs too far ahead now interrupt as a gap does.** On both inputs, a PDU stamped past the early
  bound is not processed. Before, the interruption check ran only for other PDUs or on a receive
  timeout. A stream arriving every 2 ms under a 5 ms timeout might never reach it, so R901-2's
  early-only CRF check depended on scheduling: here it failed in 2 of 3 runs on the `afc35064`
  binaries. Now each such PDU runs the check (`stop_playback()`, `interrupt()`). `counters.py`'s
  "1.5 s of PDUs all stamped 300 ms ahead" checks one STREAM_INTERRUPTED on each input and the
  CRF unlock, and fails at `afc35064`.
- **The suites' capture.** `peer.Wire.control_only()` filters the test capture to AVTP control
  frames. Without it, a run at `ad9c0f09` missed a BIND_RX response under the DUT's stream data.
  It is used by `counters.py` and `rxfail.py` only.

## Validation at the head

| Check | Result |
| --- | --- |
| `make check`, twice | rc 0, rc 0 (peer 26, aecp 107, counters 104, rxfail 44, nvm 7, units, talker, listener) |
| `make check-compat`, library defaults and leniencies off | rc 0, rc 0: four runs, Milan 1.2, zero compatibility events, warnings or errors |
| R901-2 probes (burst, startup) | rc 0, rc 0; at `afc35064` rc 1, rc 1 |
| R901-1 probes | identity mutant rc 1 (the three identity checks), pristine rc 0, gPTP-stale rc 0 |
| the new checks at `afc35064`; three mutants | counters 12 failed, rxfail 38, nvm 2; cache 7, drain 1, carry 2 |
| aarch64 cross build twice | identical: `milan-ctrld` `b1254bb0…`, `milan-mediad` `8bef6f76…`, `milan-dp` `c2824e61…`, `milan-gptp-watch` unchanged |
| `git diff --check afc3506..HEAD` | clean |

Two `make check` runs at `ad9c0f09` failed, and both are kept in the receipts. One was the
unchanged listener suite's rate bound (8109.5/s against 8100); it passes alone. The other was the
capture miss above, fixed by `ed21f9b3`.

## Open risks and questions

- Board behaviour of this head is unverified: it is not installed. The edge check runs after every
  firmware register write in milan-ctrld (16 loads), which was not measured on the board.
- An input whose receive setup fails stays not received until milan-mediad restarts. It is
  logged, with no retry. A retry was left out to keep the failure visible and the counters'
  lifetime simple.
- The drain at a new binding drops what the socket holds at that moment. In normal running the
  socket's filter is closed while the sink is not listening, so that is at most the PDUs of one
  10 ms check.
- An existing step, `80 ms held, then the backlog`, counted two interruptions in two runs on other
  builds (`afc35064`, the cache mutant) and never at the head. It is host timing, but it is noted.
- Pin and Board acceptance stay open; Install is partly met.
