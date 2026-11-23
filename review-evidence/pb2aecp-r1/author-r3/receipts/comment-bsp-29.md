[A830] REVIEW READY

Commit: `49b2035605729e74d1fc664fb25304402e758ea4` on `pb2-aecp-1` (two local commits on `f405a5754afbe30313116be4662adb9d79567f9e`, not pushed; the manager publishes and installs). The milan-fpga pin is unchanged: `677d605f`, PR #986's published head.

Changed:
- **STREAM_INPUT 0:** milan-mediad's AAF listener reads tu and mr from every PDU. TIMESTAMP_UNCERTAIN and MEDIA_RESET count per Milan v1.2 Table 5.6 (5.3.8.10), as observation intervals in which tu was set or mr toggled; IEEE 1722.1-2021 7.4.42.2.4 counts the same events per PDU. An mr toggle places the stream again (IEEE 1722-2016 4.4.4.3), and PDUs with tu set are played without steering (4.4.4.7). The listener's own jumps count STREAM_INTERRUPTED, no longer MEDIA_RESET. All ten Table 5.16 counters are valid.
- **STREAM_INPUT 1 (CRF):** a CRF receive path in milan-mediad receives the bound CRF stream and counts all ten counters. MEDIA_LOCKED is 25 stamps in a row on the 2 ms grid of the Milan CRF format (Milan v1.2 Table 7.1). It clocks nothing. Without it running, the input's counters are not valid.
- **Both inputs:** counters are zeroed at each not-bound to bound change (5.3.8.10).
- **STREAM_OUTPUT 0:** all five Milan v1.2 Table 5.4 counters, counted from the PDUs sent. la_avdecc's Milan 1.2 check needs them once the input error is gone.
- **R901-F1:** the gPTP status is live only while its block's update count is seen to move. A remap or a replacement block is a baseline. 2 s without a move, including unreadable or odd-seq blocks, is one UNLOCKED and no LOCKED until updates resume.
- **R901-F2:** the veth test's DUT MAC is not the image's placeholder, and ADP's entity_id is checked too.
- **R901-F3, R1, R2:** the PR body no longer says it closes #29; each acceptance row below is stated as it stands. The YAML comment and the pin prose are corrected as given.
- **Milan v1.3** is not among the standards used; v1.2 5.3.8.10 was. la_avdecc 4.3.1.1, which cites "Milan 1.3 - 5.3.8.10", requires the same ten STREAM_INPUT counters at the same bits, so no v1.3 difference shows there. Its v1.3 STREAM_OUTPUT layout applies only to entities whose GET_MILAN_INFO gives specification_version 1.3 or later, which this entity's does not.

Validation:
- `make check`: rc 0. Config current; unit tests (AAF and CRF PDUs, the interval counter); peer 26 checks; aecp 107, with four gPTP fault cases; counters 63, the real daemons under a lost PDU, tu, mr, another format, a late backlog, early stamps, an interruption and a rebind, all read back over AECP; talker, listener and nvm suites.
- `make check-compat`: a controller on la_avdecc v4.3.1.1, built locally twice, with the library defaults and with its five leniencies off. Each ran unbound and with both inputs bound to running AAF and CRF streams. All four: `IEEE17221|Milan`, Milan 1.2, **zero compatibility events, warnings or errors**.
- The same program on the f405a57 build gives `[Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor`, the board's error. It also fails this head without the CRF path.
- R901's probes, unchanged: the identity mutant fails the suite's own wrapper on exactly the three identity checks; the pristine daemon passes; the gPTP-stale probe passes (LOCKED 5 before and after, UNLOCKED 4 to 5).
- aarch64 cross build of all four programs, reproduced by two clean builds:
  - `milan-ctrld` `9fe4012fb698f97295fd14cb4e44e238be2e4427720aaf962dcdf4433cf708b2`
  - `milan-mediad` `162c8223b2ffc52484381e061e691e1467815294ba502a2637d4d27ac59a8b77`
  - `milan-dp` `0c61cd3e0d4a3f2a6b72187ad31d22c5be4b8419e6a6710e514e6a58584a86f8`
  - `milan-gptp-watch` and `entity.conf` unchanged

  The datapath and media block layouts changed, so milan-ctrld, milan-mediad and milan-dp are installed together.

Acceptance criteria (#29):
- **Pin: open.** The pin is PR #986's head, not a dev-soft-core merge, and the firmware gate at the pin was run only in part.
- **Host test: met.**
- **Board: open.** This head fixes the reported error on the veth pair but has not been installed on the board.
- **Install: partly met.** Manual install list only; the Buildroot image has not been built with the published archive.

No Milan conformance is claimed for the board: nothing clocks from the CRF input.

Open risks/questions: the CRF thread has not run on the board's isolated media core (it runs one priority below the listener). The 500 ms observation interval and the CRF lock rule are implementation choices Milan leaves open; AECP-STATUS.md states them.
