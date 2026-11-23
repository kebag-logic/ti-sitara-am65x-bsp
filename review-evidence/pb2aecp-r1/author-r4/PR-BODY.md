The board advertised AEM_SUPPORTED, but milan-ctrld served no AECP, so a controller could not read the Configuration descriptor IEEE 1722.1-2021 7.2.1 requires. This branch makes milan-ctrld serve milan-fpga's AECP, unchanged, from a host AEM store generated from the board's own end-station config. milan-mediad keeps the Milan counters of all three streams, the CRF input's included. Each binding zeroes its input's counters however fast bindings follow one another, and an input that is not received reports none of them valid.

Refs #29. **This PR does not close #29**: two of its acceptance criteria are open (the pin on dev-soft-core with the full firmware gate, and a controller enumerating the board), and a third is only partly met (the install). The table below gives each one's state. **No Milan conformance is claimed for the board**: the media plane does not clock from the CRF input. `milan-linux/AECP-STATUS.md` lists what is met and what is not.

## Acceptance (#29)

| Criterion | State |
| --- | --- |
| Pin: milan-fpga moved to dev-soft-core, with `make check` and the firmware gate | **open.** The pin is milan-fpga PR #986's reviewed head (`58f7673b`, two POSITIVE reviews), not a dev-soft-core merge. #986 has not landed yet. `make check` passes at the pin. milan-fpga's firmware gate at the pin was run in part: selected arms passed; gate 1b, the gate shards and the mutation campaigns were not completed (milan-fpga#983's receipts) |
| Host test: enumeration against the daemon on a veth pair | met: `tests/run-netns-aecp.sh` (107 checks), and a controller on la_avdecc 4.3.1.1 (`make check-compat`) that finds the entity Milan 1.2 compatible with no compatibility event, warning or error |
| Board: a controller enumerates the board without errors | **open.** The owner's controller enumerated the board running the f405a57 build and reported `[Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor`. The 49b2035 build is now installed on the board. Read from a controller there, STREAM_INPUT 0 and 1 report all ten counters valid and STREAM_OUTPUT 0 its five. Its enumeration by the owner's controller is not recorded. This head is not installed on the board |
| Install: under the RAUC flow, or a documented manual install | **partly met.** A manual install is documented in the receipts: which files, and that milan-ctrld, milan-mediad and milan-dp go in together. The 49b2035 build was installed on the board that way; this head is not. The Buildroot packages are in the defconfig, but the image build has not been run with the published archive |

## What changed

- **The pin** is milan-fpga `58f7673b`, the reviewed round-3 head of `pf-linuxprof-1` (milan-fpga#983, the descriptor-only host profile), published as milan-fpga PR #986. `MILAN_FPGA_SHA256` is its archive's sha256 (`2d948089…`), and `tools/fetch-milan-fpga.sh` fetches and verifies it without a local checkout. The pin must move to the upstream merge commit after #986 lands. It also names tsn-c-stack (ADP, ACMP, MAAP, the wire layer) and lwSRP (the headers AECP's bridge includes), each with its archive's sha256.
- **The board's config**, `config/endstation_pb2_bridge.yaml`, is milan-fpga's host profile (`board.target: host`, `gptp_owner: host-phc`). It has one AVB_INTERFACE, STREAM_INPUT 0 AAF 48 kHz 8 ch 32-bit, STREAM_INPUT 1 CRF, and STREAM_OUTPUT 0 AAF in the same format. There is no second AAF and no CRF output, and ADP advertises 1 source and 2 sinks. It carries the specified entity name and the entity_model_id pinned to `0x001BC50AC1000100` (Milan v1.2 7.2.2, 7.2.3; IEEE 1722.1-2021 7.2.1). `tools/gen-config.sh` generates `entity.conf`, the AEM image, the shape header and the store's shape from it through milan-fpga's generators, and `make check` refuses a stale one.
- **milan-ctrld** boots as the split platform does (`sw/firmware/ctrl/plat/ctrl_plat.c`):
  - the generated image is verified against its CRC;
  - the application, AECP and contract 2.3's face are composed;
  - the saved state is applied through AECP's owner;
  - the mailbox opens with ADP held, AECP is released, and ADP starts last.

  The board's MAC goes into the image's three mac-derived fields only after the image is verified, so ADP and AECP name one entity. Every unit is compiled with the shape header and `CTRL_HOST_PLATFORM`, milan-fpga#983's host-only row of THE SHAPES. SRP is still not attached (#14).
- **The face's windows** are answered by `src/aecp_far.c` from what the board has:
  - the datapath block answers the outputs' stream info;
  - the gPTP daemon's status block answers GET_AVB_INFO and GET_AS_PATH;
  - the stream counters come from what milan-mediad counts;
  - SET_STREAM_FORMAT is supported for the format the media plane runs;
  - map edits are refused.
- **The stream counters** follow Milan v1.2 Table 5.6 (STREAM_INPUT, 5.3.8.10) and Table 5.4 (STREAM_OUTPUT, 5.3.7.7), in the quadlets of Tables 5.16 and 5.17. Most count 500 ms observation intervals in which their event happened.
  - STREAM_INPUT 0 (the AAF listener) counts TIMESTAMP_UNCERTAIN from the tu bit and MEDIA_RESET from mr toggles (IEEE 1722-2016 4.4.4.7, 4.4.4.3). It places its stream again on an mr toggle, plays PDUs with tu set without steering, and drops PDUs too far in the future to process.
  - STREAM_INPUT 1 has a CRF receive path (`src/crf_rx.c`), which clocks nothing. It counts the same header events. UNSUPPORTED_FORMAT is a PDU outside the Milan CRF format (Milan v1.2 Table 7.1). MEDIA_LOCKED is 25 stamps in a row on the format's 2 ms grid.
  - STREAM_OUTPUT 0 (the talker) counts all five Table 5.4 counters from the PDUs it sends. Its MEDIA_RESET and TIMESTAMP_UNCERTAIN stay 0, since it never toggles mr nor sets tu.
  - On either input, a run of PDUs stamped too far ahead is not played, so it interrupts the stream as a gap does (STREAM_INTERRUPTED). This is judged on each such PDU. Before, it was judged only when a receive timed out, which a stream arriving every 2 ms may never do, so the result depended on scheduling.
- **The reset at each binding** (Milan v1.2 5.3.8.10: zeroed at every not bound to bound change):
  - milan-ctrld counts the changes per sink in the datapath block where the binding is owned: the publication block's BINDING.BOUND, which ACMP writes before it answers BIND_RX or UNBIND_RX. The soft fabric sees each edge in the firmware write that makes it (`src/softfab.c`). Before, the bound level was sampled once per loop pass, so an unbind and bind served in one pass were never counted, and the new binding kept the old one's counters.
  - A binding the saved state restores at boot counts too. A restarted milan-ctrld counts on from the count the block it takes over holds, so the media plane cannot take the restored binding for the one it last zeroed for.
  - GET_COUNTERS compares the media block's count with the datapath block's as it answers, not as the far side last polled (up to 100 ms before). So a new binding reads zero from the moment it is made, before milan-mediad has zeroed for it.
  - milan-mediad zeroes an input when the count moves, and drops what its socket still holds from before.
- **An input that is not received has none valid.** Each receiving thread marks its counters valid only once its socket is set up (`src/rxsock.c`). A failed step (no such interface, socket, filter, bind, receive timeout) closes what it opened and is logged once with its step. The input then reports none valid, and milan-mediad runs on with its other streams. Before, the AAF listener published all ten valid before setting up, and kept them valid when setup failed.
- **gPTP freshness:** the gPTP daemon's status is live only while its block's update count is seen to move. A first read, a remapped block or a replacement block is a baseline, not evidence. 2 s without a move, whether the block is unchanged, unreadable or stuck mid-write, expires it: one CLOCK_DOMAIN UNLOCKED, asCapable cleared, and no LOCKED until updates resume.
- **The journal** moves to the KLJ3 layout this pin's store writes, a 640 KiB file. A KLJ2 file from the earlier image is moved aside and read only for its header. `-N none` keeps the journal in memory.
- **Tests**, in `make check` unless noted:
  - `tests/aecp.py` enumerates with a DUT MAC that is not the image's placeholder, so a daemon serving the image's identity unchanged fails it. It also checks ADP's entity_id, each stream's counters as the media block marks them, the zeroing at bind, and CLOCK_DOMAIN against a gPTP publisher that stops, sticks mid-write, or is replaced live or dead.
  - `tests/counters.py` runs the real daemons. A talker on the other end binds both inputs and puts each fault into its AAF and CRF streams, and every counter's change is read back over AECP. It also runs 1.5 s of PDUs all stamped too far ahead, and a burst: with both daemons held, UNBIND_RX and BIND_RX twice on STREAM_INPUT 0 and once on STREAM_INPUT 1, queued and served together. Each binding must be counted, and both inputs must read zero at once and after milan-mediad runs again. Last comes GET_COUNTERS sent the moment a BIND_RX response is seen, five times.
  - `tests/rxfail.py` restarts milan-mediad with each setup step made to fail in one input (`tests/rxfail.c`, an LD_PRELOAD library), and on an interface that does not exist.
  - The saved-state suite checks that a restored binding moves the bind count past the killed process's, and that an unbound restart keeps it.
  - `tests/milan-compat/` (`make check-compat`, against a la_avdecc build) is a controller on la_avdecc 4.3.x that prints every compatibility event and warning.

## Evidence

All at the author head (`ed21f9b3`) with milan-fpga `66b4def3`, unless a line says otherwise. The manager's re-pin commit on top moves the pin to `58f7673b`: config regenerated with revision lines only (`gen-config.sh --check` current), and #986's round 3 records the board's host build as byte-identical between the two (35 of 35 objects and the binary).

- `make check` passes twice (3 min 15 s, 3 min 17 s):
  - `config/` current; `test_media` (AAF and CRF PDUs byte by byte, the interval counter, the servo); `test_gptp`;
  - `peer.py`, 26 checks; `aecp.py`, 107; `counters.py`, 104; `rxfail.py`, 44;
  - talker, listener, and the saved-state power-cut suite (7 checks).
- The new checks fail at `afc35064`: the burst (no binding counted, the old counters served), the five immediate rebinds, the early-only run, the restored binding's count, and every receive setup failure (input 0 still `0xf3f` on a missing interface). Three mutants of this head fail too: the far side answering from its polled copy (seven checks), no drain of the socket (one), and no count carried across a restart (two).
- The reviewer's R901-2 probes, run unchanged, pass at this head: the burst (`binds` counted, EARLY_TIMESTAMP zeroed) with its early-only CRF check, and the startup probe (both inputs none valid on a missing interface). At `afc35064` with its own tests, they fail as reported. The R901-1 probes also pass at this head: the identity mutant fails exactly the three identity checks, the pristine daemon passes, and the gPTP-stale probe gives LOCKED 5 to 5 and UNLOCKED 4 to 5.
- `make check-compat` with la_avdecc v4.3.1.1 (`6d61a92e`), built twice: once with the library's defaults (its five leniencies on), and once with all five off. Each build ran unbound, and with both inputs bound to running AAF and CRF streams. All four runs: `IEEE17221|Milan`, Milan 1.2, zero compatibility events, warnings or errors. Earlier, at `49b2035`, the same program failed the f405a57 build with the owner's error, and failed when milan-mediad ran without its CRF receive path.
- aarch64 cross build of all four programs, milan-mediad against the target's alsa-lib, reproduced by two clean builds:
  - `milan-ctrld` `b1254bb0de7c27f5f9b5a3fb2b3731f1828c0d213e04a7b8d51220cc650bb6b4`
  - `milan-mediad` `8bef6f7693e34341c80a4e7138bdca1d8790d61d337850a47e43a048ec833f27`
  - `milan-dp` `c2824e617bc51c9ae242ddfc859e83a6364d8c2daa722c25de7a21e66ef7a529`
  - `milan-gptp-watch` `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` (unchanged)
  - `entity.conf` `532966e838a57e5944dc487a2d9be9779ecc37ff264f947d7e6d28a622a6dac9`. It was regenerated at the re-pin to `66b4def3`, and only its revision comment changed. The previous body gave `e273ff97…` as unchanged: that is the `49b2035` file, generated at `677d605f`.

  The datapath and media blocks keep their layouts (versions 2 and 3). milan-ctrld, milan-mediad and milan-dp are installed together, as before.
- Earlier receipts stay as they were: the published packet for `49b2035` (milan-fpga `677d605f`) records that head's runs, not this one's.

## Not met, not claimed

- The CRF input is received and counted, but nothing clocks from it. AECP accepts selecting it as clock source, and the media plane does not follow.
- The talker never sets tu: on a gPTP step it starts a new stream instead.
- ADD/REMOVE_AUDIO_MAPPINGS answer NOT_SUPPORTED, because the media plane has no channel router.
- SET_STREAM_FORMAT takes only the running format.
- GET_AS_PATH is answered only where the gPTP daemon determines the path.
- No SRP; REBOOT is NOT_IMPLEMENTED.
- The host shape row is a stated scheduling profile, not a measurement on the board.
- Milan v1.3 is not among the specifications used: the counters follow v1.2. AECP-STATUS.md states what the controller's v1.3 citation implies.
- An input whose receive setup fails stays not received until milan-mediad is restarted: it is logged, with no retry.
- This head is not installed on a board; the owner's enumeration of the board is not recorded; the Buildroot image build has not been run with the published archive.

Target: `pb2-aecp-1 -> pb2-tsn`.
