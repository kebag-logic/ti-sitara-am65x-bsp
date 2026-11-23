[A830] REVIEW READY

Commit: `ed21f9b36950e405e402695185e712c0a92b116a` on `pb2-aecp-1` (three local commits on `afc350641dc2d93cee39bb4c62258b6d55f00ccb`, not pushed; the manager publishes and installs). The milan-fpga pin is unchanged: `66b4def3`, PR #986's round-2 head.

Changed:
- **R901-F4, the reset at each binding:** milan-ctrld now counts each not-bound to bound change (Milan v1.2 5.3.8.10) where the binding is owned: the publication block's BINDING.BOUND, which ACMP writes before its BIND_RX or UNBIND_RX response. The soft fabric sees each edge in the firmware write that makes it, so a burst served in one loop pass counts every binding. Before, the bound level was sampled once per pass. GET_COUNTERS compares the bind counts as it answers rather than from a copy polled every 100 ms, so a new binding reads zero at once. A restarted milan-ctrld counts on from the count it finds, so a binding restored at boot is a new one. milan-mediad drops what its socket holds from before a new binding.
- **R901-F5, validity:** each receiving thread marks its counters valid only once its socket is set up. A failed step (interface, socket, filter, bind, receive timeout) closes what it opened and is logged once with its step, and the input reports none valid. milan-mediad runs on with its other streams.
- **PDUs too far ahead** now interrupt a stream as a gap does, on both inputs. Before, this was judged only on a receive timeout, which made the result depend on scheduling.
- **R901-F6:** the config checksum is corrected. `entity.conf` is `532966e838a57e5944dc487a2d9be9779ecc37ff264f947d7e6d28a622a6dac9`, regenerated at the re-pin with only its revision comment changed. `e273ff97…` was the `49b2035` file.
- **R901-F3:** each acceptance row below is stated as it stands.

Validation:
- `make check`: rc 0, twice. Peer 26 checks; aecp 107; counters 104, now also with a burst of UNBIND_RX/BIND_RX queued while both daemons are held (two bindings on input 0, one on input 1, each counted, both inputs zero at once), five GET_COUNTERS sent the moment a BIND_RX response is seen, and a run of PDUs stamped too far ahead; rxfail 44, each setup step made to fail in each input; the saved-state suite now checks the bind count of a restored binding; talker and listener suites.
- These checks fail at `afc35064`, and against three mutants that each remove one part of the F4 fix.
- The R901-2 burst and startup probes, unchanged: rc 0 at this head, rc 1 at `afc35064`. The R901-1 probes still behave as before: the identity mutant fails exactly the three identity checks, the pristine daemon passes, and gPTP-stale passes.
- `make check-compat`: la_avdecc v4.3.1.1 with the library defaults and with its five leniencies off, each unbound and with both inputs bound to running streams. All four: `IEEE17221|Milan`, Milan 1.2, **zero compatibility events, warnings or errors**.
- aarch64 cross build of all four programs, reproduced by two clean builds:
  - `milan-ctrld` `b1254bb0de7c27f5f9b5a3fb2b3731f1828c0d213e04a7b8d51220cc650bb6b4`
  - `milan-mediad` `8bef6f7693e34341c80a4e7138bdca1d8790d61d337850a47e43a048ec833f27`
  - `milan-dp` `c2824e617bc51c9ae242ddfc859e83a6364d8c2daa722c25de7a21e66ef7a529`
  - `milan-gptp-watch` unchanged

  Block layouts are unchanged. Install milan-ctrld, milan-mediad and milan-dp together: stop the bridge, replace the three, start it.
- Two earlier `make check` runs at an intermediate commit failed: the unchanged listener suite's rate bound once (8109.5/s against 8100; it passes alone), and a test capture that missed a response under stream data, now filtered to control frames. Both logs are kept.

Acceptance criteria (#29):
- **Pin: open.** The pin is PR #986's round-2 head. #986 is still in review, with a `ctrl_loop.h` fix in progress, and has not landed on dev-soft-core. The firmware gate at the pin was run only in part.
- **Host test: met.**
- **Board: open.** The 49b2035 build is installed on the board. Read from a controller, STREAM_INPUT 0 and 1 report all ten counters valid and STREAM_OUTPUT 0 its five. The owner's enumeration is not recorded. This head is not installed.
- **Install: partly met.** The documented manual install was used for the 49b2035 build; the Buildroot image has not been built with the published archive.

No Milan conformance is claimed for the board: nothing clocks from the CRF input.

Open risks/questions: this head has not run on the board, including the edge check after each firmware register write. An input whose receive setup fails stays unreceived until milan-mediad restarts, with no retry.
