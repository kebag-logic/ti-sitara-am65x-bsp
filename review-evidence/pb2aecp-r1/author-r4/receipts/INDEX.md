# Round 4 receipts (BSP #29, PR #30)

BSP branch `pb2-aecp-1`, head `ed21f9b36950e405e402695185e712c0a92b116a` (tree
`672aa8b2f981fb0ed14662174a678650732738cf`), on the published
`afc350641dc2d93cee39bb4c62258b6d55f00ccb`:

- `88cddb6d9e78934db197722293f2d7ae20bed07f`: the reset at each binding (R901-F4), validity only for a received input (R901-F5), PDUs too far ahead interrupt as a gap does; their tests
- `ad9c0f09f4883d5fd82196081293ff933d6f8a71`: AECP-STATUS and README, the acceptance rows as they stand (R901-F3)
- `ed21f9b36950e405e402695185e712c0a92b116a`: the counter and receive-failure suites capture only AVTP control frames; each immediate rebind is checked to settle again

Local commits, not pushed. No board, bench or remote host was contacted.
milan-fpga is pinned at `66b4def34dcbc0d371ce4eb339d3d0226875befa` (PR #986's round-2 head,
archive sha256 `c5b3b01c19db771bb6e6c379a7510489c5450b8214d9df5499b8d4a4957c0ee2`), unchanged
from `afc35064`. It was fetched and verified by `tools/fetch-milan-fpga.sh --with-submodules`
([logs/fetch-milan-fpga.log](logs/fetch-milan-fpga.log)). Every run below used that tree, and
an export of the head (`git archive`), not the working checkout. [git-state.txt](git-state.txt)
has the state.

## The aarch64 binaries

Built from the head's export, twice from empty output directories, with identical results
([logs/cross-build-1.log](logs/cross-build-1.log), [logs/cross-build-2.log](logs/cross-build-2.log)):

    make MILAN_FPGA=<66b4def3 tree> O=<out> CC=aarch64-linux-gnu-gcc \
         CFLAGS=-I<sysroot>/include LDFLAGS=-L<sysroot>/lib all

aarch64-linux-gnu-gcc 16.2.0, GNU ld 2.47. The sysroot is round 3's: alsa-lib 1.2.15.3 from the
release tarball (sha256 `7b079d614d582cade7ab8db2364e65271d0877a37df8757ac4ac0c8970be861e`),
for its headers and `libasound.so.2` only. Each binary needs at most GLIBC_2.38, as round 3's do.
milan-mediad's ALSA symbols are ALSA_0.9 and ALSA_0.9.0, and it now also uses
`pthread_setname_np` (GLIBC_2.34).

| File | sha256 | Changed since round 3 (`49b2035`, = `afc35064`'s) |
| --- | --- | --- |
| `milan-ctrld` | `b1254bb0de7c27f5f9b5a3fb2b3731f1828c0d213e04a7b8d51220cc650bb6b4` | yes (`9fe4012f...`): the bind count at each BINDING.BOUND edge, carried across a restart; GET_COUNTERS against the count as it answers |
| `milan-mediad` | `8bef6f7693e34341c80a4e7138bdca1d8790d61d337850a47e43a048ec833f27` | yes (`162c8223...`): validity after receive setup, the setup failure logged, the drain at a new binding, early PDUs interrupt |
| `milan-dp` | `c2824e617bc51c9ae242ddfc859e83a6364d8c2daa722c25de7a21e66ef7a529` | yes (`0c61cd3e...`): it links `datapath.c`, whose writer's create changed; milan-dp only reads, so what it prints is unchanged |
| `milan-gptp-watch` | `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` | no, byte-identical |
| `entity.conf` | `532966e838a57e5944dc487a2d9be9779ecc37ff264f947d7e6d28a622a6dac9` | regenerated at the re-pin (`afc35064`): only the revision comment, `677d605f` to `66b4def3`. Round 3's `e273ff97...` is the `49b2035` file |
| `milan-bridge.sh` | `6cf38b8dac1ae9946afe0e5524ac1c372747764f26a5602e9e54cb75badbb488` | no, byte-identical |

[aarch64/SHA256SUMS](aarch64/SHA256SUMS) lists them.

## Install list

The board runs the round-3 (`49b2035`) build. The block layouts did not change this round
(datapath version 2, media version 3), but install the three together, as before.

| Board path | From | Must |
| --- | --- | --- |
| `/usr/sbin/milan-ctrld` | `aarch64/milan-ctrld` | yes |
| `/usr/sbin/milan-mediad` | `aarch64/milan-mediad` | yes |
| `/usr/sbin/milan-dp` | `aarch64/milan-dp` | yes (with the other two, so the three are one build) |
| `/usr/bin/milan-gptp-watch` | `aarch64/milan-gptp-watch` | no (unchanged) |
| `/etc/milan/entity.conf` | `aarch64/entity.conf` | no (only its comment differs from round 3's) |
| `/usr/sbin/milan-bridge.sh` | `aarch64/milan-bridge.sh` | no (unchanged) |

Stop both daemons (`/usr/sbin/milan-bridge.sh stop`), replace the three, start again
(`/usr/sbin/milan-bridge.sh start`, or reboot). The saved-state journal (KLJ3) and the AEM image
are unchanged. A binding restored at that start counts on from the bind count the old milan-ctrld
left in the datapath block.

After the start, `milan-dp` prints `milan_stream_input0`, `milan_stream_input1` and
`milan_stream_output0` with `valid:f3f`, `valid:f3f` and `valid:01f`. If an input's receive
socket could not be set up, milan-mediad's log has `listener on <if>: <step>: ...` or
`CRF input on <if>: <step>: ...`, and that input shows `valid:000`.

## Validation at the head

| Check | Result | Receipt |
| --- | --- | --- |
| `make check`, run 1 | rc 0 in 195 s: `config/` current; `test_media`, `test_gptp` (7); peer 26; aecp 107; counters 104; rxfail 44; talker 5; listener 7; nvm 7 | [logs/make-check-1.log](logs/make-check-1.log) |
| `make check`, run 2 | rc 0 in 197 s, the same counts | [logs/make-check-2.log](logs/make-check-2.log) |
| `make check-compat`, library defaults | rc 0: unbound, and `streams`; PASS, Milan 1.2, zero events, warnings or errors | [logs/compat-default-options.log](logs/compat-default-options.log) |
| `make check-compat`, leniencies off | rc 0: the same | [logs/compat-strict-options.log](logs/compat-strict-options.log) |
| R901-2's burst probe (`run-edge-probes.py`), unchanged | rc 0: both commands SUCCESS, `binds:2`, EARLY_TIMESTAMP 0 after the burst; its early-only CRF check and separated control pass | [logs/r901-edge-probes-head.log](logs/r901-edge-probes-head.log) |
| R901-2's startup probe (`run-startup-probe.py`), unchanged | rc 0: milan-mediad running, STREAM_INPUT 0 and 1 `valid 0x0` on a missing interface | [logs/r901-startup-probe-head.log](logs/r901-startup-probe-head.log) |
| R901-1's probes (`run-prior-probes.py`), unchanged | identity mutant rc 1 (exactly the three identity checks), also with the distinct MAC; pristine rc 0; gPTP-stale rc 0 (LOCKED 5 to 5, UNLOCKED 4 to 5) | [logs/r901-1-probe-*.log](logs/) |
| aarch64 cross build, twice | rc 0, identical | above |
| `git diff --check afc3506..HEAD` | clean | |

The R901 probe scripts copy the tree's `tests/` and patch their own copy. They were given the
head's export and its native build.

## The new tests fail where they should

The head's suites against the `afc35064` binaries (built from its export at the same pin):

| Suite | Result at `afc35064` | Receipt |
| --- | --- | --- |
| `counters.py` | rc 1, 12 failed: the early-only run (no STREAM_INTERRUPTED on either input); the burst (`binds` 1, 2 before and after, the old counters served at once, while held and after milan-mediad runs again); all five immediate rebinds (the old binding's FRAMES_RX served) | [logs/neg-counters-afc3506.log](logs/neg-counters-afc3506.log) |
| `rxfail.py` | rc 1, 38 failed: on a missing interface STREAM_INPUT 0 stays `0xf3f` and nothing is logged. Its threads have no names there, so no step can be injected, and those checks fail as well; the control passes | [logs/neg-rxfail-afc3506.log](logs/neg-rxfail-afc3506.log) |
| saved-state suite | rc 1, 2 failed: the restored binding's count stays 1 (`1 -> 1`); after the unbound restart it falls to 0 | [logs/neg-nvm-afc3506.log](logs/neg-nvm-afc3506.log) |
| R901-2's probes with `afc35064`'s own tests | rc 1: `burst rebind counted` and `burst rebind zeroed earlier EARLY_TIMESTAMP` fail; the startup probe's STREAM_INPUT 0 check fails | [logs/r901-edge-probes-afc3506.log](logs/r901-edge-probes-afc3506.log), [logs/r901-startup-probe-afc3506.log](logs/r901-startup-probe-afc3506.log) |

Three mutants of the head's source, each with one part of the F4 fix taken out
([mutants/](mutants/)), against the head's suites:

| Mutant | Result | Receipt |
| --- | --- | --- |
| `cache`: GET_COUNTERS from the far side's polled copy | rc 1: the two burst reads at once after the last BIND_RX response and all five immediate rebinds fail (7). An eighth failure in that run, `80 ms held, then the backlog` (two interruptions), is the timing noted below | [logs/neg-counters-mutant-cache.log](logs/neg-counters-mutant-cache.log) |
| `drain`: no drain of the socket at a new binding | rc 1: after the burst, the inputs count the backlog for the new binding (SEQ_NUM_MISMATCH, LATE_TIMESTAMP, STREAM_INTERRUPTED) | [logs/neg-counters-mutant-drain.log](logs/neg-counters-mutant-drain.log) |
| `carry`: the bind count not carried across a restart | rc 1: `1 -> 1` and `1 -> 0`, as at `afc35064` | [logs/neg-nvm-mutant-carry.log](logs/neg-nvm-mutant-carry.log) |

## Runs that failed, and timing on this host

- `make check` at `ad9c0f09`, run 1: the listener suite's `FRAMES_RX at 8000/s` measured 8109.5/s
  against its 8100 bound ([logs/make-check-ad9c0f09-run1-listener-rate.log](logs/make-check-ad9c0f09-run1-listener-rate.log)).
  The suite then passed alone twice with that build (8032.9/s, 8031.3/s) and once at `afc35064`
  (7996.6/s) ([logs/listener-rerun-1.log](logs/listener-rerun-1.log),
  [logs/listener-rerun-2.log](logs/listener-rerun-2.log), [logs/listener-afc3506.log](logs/listener-afc3506.log)).
  That suite and its bound are unchanged by this round.
- `make check` at `ad9c0f09`, run 2: `rebind 3 of 5` failed because the test's capture missed the
  BIND_RX response and the probe that came with it, so the binding never settled (ACMP status 7)
  ([logs/make-check-ad9c0f09-run2-counters-capture.log](logs/make-check-ad9c0f09-run2-counters-capture.log)).
  The capture received every frame on the link, the DUT's 8000 AAF PDUs a second among them.
  `ed21f9b3` filters it in the kernel to AVTP control frames (`peer.Wire.control_only`, used by
  `counters.py` and `rxfail.py`), and checks each rebind settles again. Both runs above are at
  `ed21f9b3`.
- `80 ms held, then the backlog` (an existing step) counted two STREAM_INTERRUPTED on input 0
  where one is allowed: once on the `afc35064` binaries, once on the `cache` mutant. It did not
  occur at the head in the two `make check` runs, nor in two separate counter-suite runs of the
  head's source and tests.
- R901-2's early-only CRF check at `afc35064` depends on scheduling there: here it failed in 2 of
  3 runs on the `afc35064` binaries, and in all 3 runs of an intermediate build without this
  round's early-PDU change. Only the last `afc35064` run's log is kept (it passed there). At the
  head it passes in the probe and in the suite's own early-only step.

## la_avdecc

L-Acoustics' avdecc at tag `v4.3.1.1` (commit `6d61a92e7f264c69f23cdc38f50d31114e567aa0`), the
same source as round 3. The leniencies-off build is round 3's. The library-defaults build was
rebuilt this round from the same source, since round 3's build tree is gone: `cmake -G Ninja
-DCMAKE_BUILD_TYPE=Release`, `ninja la_avdecc_controller_static`
([logs/lavdecc-configure-default.log](logs/lavdecc-configure-default.log),
[logs/lavdecc-build-default.log](logs/lavdecc-build-default.log)). Its cache has the five
leniencies on, and the program prints them (IICDL IINSAR INSNDM CMAR AGAMU).
