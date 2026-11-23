[R901] NEGATIVE - exact head a5cd7c1e48019f89583e8df930299d457e016fe3

External independent delta review of issue #29 / PR #30, R901-3. Tree `5af6661a4d4492295704e0ceef2b263c1164bb70`; source base `b6d8594c1aaff38b5e9d6b13213d90ef5e4024d4`; previous reviewed head `afc350641dc2d93cee39bb4c62258b6d55f00ccb`.

All five lenses were applied. F4 and F5 are fixed at their causes. F6's obsolete checksum claim is corrected and historical evidence is now explicitly scoped; the new head's config checksum is recorded below. Exact-head native checks and independent aarch64 builds pass. The negative verdict is solely retained MAJOR F3: frozen upstream/source-gate/board acceptance remains outstanding. One wording residue and one optional publication improvement are included.

**Reconstruction and scope**

The BSP has no tracked AGENTS.md, CONTRIBUTING.md or docs/README. Entry points were the supplied instructions, root README, milan-linux/README.md and AECP-STATUS.md. Imported authorities examined at pin `58f7673b8c57a80230e716bc1774ff9cc2f71ddc` were AGENTS.md, CONTRIBUTING.md review/verification rules, docs/README.md, REQUIREMENTS.md, ENDSTATION_BUILDER section 6, MAILBOX_CONTRACT's publication/face registers and the pinned processor integrator guide section 7.1. These establish BINDING.BOUND publication before a response, input-bank zeroing on every not-bound-to-bound transition, validity only for counters kept, Milan v1.2 Tables 5.6/5.16 and IEEE 1722.1-2021 7.4.42's read interface.

The [frozen issue](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29) requires the dev-soft-core pin and firmware gate, host enumeration on veth, successful board enumeration, and RAUC or documented manual installation. The [scope ruling](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7096504049) replaces the initial 2/2 inventory with one AAF input, one CRF input and one AAF output, ADP 1 source/2 sinks. It permits disclosing missing CRF clock following without claiming Milan conformance; it does not waive upstream landing, firmware validation or board enumeration.

The base-to-head diff and history establish the full integration context. Detailed review covered the 26-file delta since afc35064: 88cddb6 changes edge/reset/receive behavior and regressions; ad9c0f0 updates documentation; ed21f9b fixes test capture filtering and checks rebind settlement; a5cd7c1 re-pins the dependency and changes two generated revision comments. Untouched boot ordering, image validation/substitution, journal migration and other face commands retain prior coverage. No BSP RTL or submodule gitlink changes occur. The RTL lens covers the host fabric replacement and its ordering/state contracts.

Public execution receipts were examined after the independent source pass. This identity's earlier findings were then reconciled at their causes. No other reviewer's report or private author material was used. `independent-verdict.md` records the preliminary conclusion; the ledger below supersedes its pending classifications.

**R901-F3 — MAJOR, retained in part — Conformance, Tests — frozen completion acceptance remains outstanding.**

- Artifact: issue #29 Acceptance/scope ruling; PR #30 Acceptance; `milan-linux/AECP-STATUS.md:183` and `:185`; upstream #983 source evidence.
- Authority/evidence: the pin is PR #986's reviewed published head `58f7673b`, not its dev-soft-core merge. Upstream remains unmerged. The [latest upstream author receipt](https://github.com/kebag-logic/milan-fpga/issues/983#issuecomment-7102194723) explicitly leaves the full builder suite, gate 1b, firmware shards and mutation campaigns unrun. The owner's controller enumeration of installed build 49b2035 is not recorded; this head is not installed. Reading mandatory masks or passing veth compatibility does not satisfy board enumeration.
- Impact: #29 cannot be considered complete under its frozen acceptance. The false closure/completion language remains resolved; the PR correctly says it does not close #29. F3's former Docs attribution stays cleared.
- Required outcome: land the authorized upstream dependency, update/review the resulting pin/archive, supply the required author source firmware evidence, and obtain successful board enumeration through the authorized owner process. Alternatively, an explicit public owner ruling must change the frozen criteria and track deferred work. The documented manual install alternative is present; a full Buildroot build is not added as a new condition of that alternative.
- Verification: reconcile the resulting source identity, firmware receipts and installed-board/controller result with each frozen acceptance row. No manager source bank is requested; final candidate validation is a separate duty.

**R901-R3 — RESIDUE — Docs — the tracked acceptance sentence still describes the previous upstream head.**

- Location: `milan-linux/AECP-STATUS.md:183`.
- Authority/evidence: it says “published round-2 head” and a `ctrl_loop.h` fix “in progress”. The pin now names 58f7673b, whose header separation is present; the PR body accurately describes that pin.
- Impact: obsolete explanatory prose. The row remains OPEN and the partial-gate limitation remains unchanged. No measurement, test result, generated artifact, acceptance verdict or conformance claim needs changing, so this is residue.
- Exact fix: replace the first two sentences of the State cell with “**open.** The pin is PR #986's published head, not its merge into dev-soft-core, which has not happened.” Preserve the following make-check and partial-firmware-gate statements.
- Verification: compare the revised sentence to milan-fpga.pin and retain OPEN and the outstanding gates. The manager carries this to the residue checklist; it does not leave a lens unclean.

**R901-S1 — SUGGESTION — Tests, Docs, Robustness — distinguish the redacted script receipt from an installable script.**

- Artifact: public `author-r4/receipts/aarch64/milan-bridge.sh:40`, its SHA256SUMS and publication manifest; `packet-script-note.txt`.
- Evidence: publication deliberately replaced source default `$VALIDATION_STORAGE/journal.bin` with `$VALIDATION_STORAGE/journal.bin`. Its published hash is `83cc00f8…`, while SHA256SUMS records original `6cf38b8d…`. The manifest correctly identifies the redaction and both hashes. With both variables unset, the redacted default would be `/journal.bin`.
- Impact: the evidence copy should not be mistaken for a deployment artifact. The install list says this unchanged script need not be replaced; required binaries are separate and tracked source is correct. This is not a source regression or a failed required installation step.
- Suggested outcome: label this copy non-deployable and link the exact source script for optional installation, or publish an unchanged deployment copy. Keep the original/public hash distinction.
- Verification: any designated deployment copy must hash to the source and retain its journal default. This optional clarification does not affect the verdict or lens cleanliness.

**Prior findings disposition**

| ID | Disposition at this head | Cause-level evidence |
|---|---|---|
| R901-F1, MAJOR | CLOSED, retained | Freshness implementation unchanged. Exact-head AECP suite passes stopped/odd/replaced-block cases. Public unchanged stale probe reports LOCKED 5 to 5, UNLOCKED 4 to 5. |
| R901-F2, MINOR | CLOSED, retained | Distinct DUT MAC and ADP agreement remain; exact-head enumeration passes. Public identity mutant fails exactly ENTITY identity, interface MAC and clock identity; pristine passes. |
| R901-F3, MAJOR | PARTLY RESOLVED / RETAINED | Honest open rows and no closure claim; upstream landing, required source firmware evidence and board enumeration still open. |
| R901-F4, MAJOR | CLOSED | softfab.c:98/118 observes every BOUND edge before TX flush; milan_ctrld.c:257 publishes it; aecp_far.c:110 compares the live generation; datapath.c:18 carries counts across restart; both receivers drain old queued packets. Exact-head burst/immediate reads/restore and original probe pass; three published mutants fail as intended. |
| R901-F5, MINOR | CLOSED | listener.c:162/167 and crf_rx.c:139/144 publish validity only after rxsock_open succeeds. rxsock.c closes failed sockets and reports one diagnostic. All five setup failures on both inputs pass the 44-check regression; original absent-interface probe passes. |
| R901-F6, MINOR | CLOSED | PR Evidence scopes its checksum list to ed21f9b3/66b4def3, replaces the old e273ff claim with correct historical 532966, and explains the manager re-pin separately. Exact-head entity.conf is a34f5f78… below; regeneration passes. No historical hash is treated as the current file's. |
| R901-R1, RESIDUE | CLOSED, retained | YAML inventory comment contains issue ID, no review-round reference. No new prohibited reviewer/round/lane identifiers found in added source comments. |
| R901-R2, RESIDUE | CLOSED, retained | Published archive/hash/fetch prose remains correct. R3 above is a different stale acceptance sentence. |

**Reviewer-owned ledger**

| lens | CLEAN/UNCLEAN | examined artifacts | covering round | exact head |
|---|---|---|---|---|
| Conformance | UNCLEAN | Frozen issue/ruling; mailbox BOUND and counter authority; input resets/masks; host profile; retained F3 | R901-3 delta/dispositions; untouched portions retain R901-1/R901-2 | a5cd7c1e48019f89583e8df930299d457e016fe3 |
| RTL | CLEAN | Host publication observer, response ordering, generation comparisons, one-interface model, reset/lock pairs; upstream host header and shape reader; no BSP RTL delta | R901-3; untouched portions retain prior coverage | a5cd7c1e48019f89583e8df930299d457e016fe3 |
| Robustness | CLEAN | Receive setup failure/cleanup/diagnostics on both inputs, backlog drain, restart carry, early-PDU interruption; original probes | R901-3; untouched boot/journal/freshness retains prior coverage | a5cd7c1e48019f89583e8df930299d457e016fe3 |
| Tests | UNCLEAN | Exact-head make check, original probes, cross rebuild; manifest, positive/negative/mutation/compatibility receipts; retained F3 evidence obligations | R901-3 plus inherited unaffected tests | a5cd7c1e48019f89583e8df930299d457e016fe3 |
| Docs | CLEAN | README, AECP-STATUS, pin/hash/generated provenance, current PR/issue rows and manual install; R3 residue and S1 suggestion only | R901-3; prior corrected wording retained | a5cd7c1e48019f89583e8df930299d457e016fe3 |

**Execution and evidence**

The [public packet at 0aca227c](https://github.com/kebag-logic/ti-sitara-am65x-bsp/tree/0aca227c4f85303d0a61439640d48e0c1bad08ee/review-evidence/pb2aecp-r1/author-r4) records author execution at ed21f9b3 with upstream 66b4def3. All 183 manifest entries match their published SHA-256 values. The commit has no embedded signature and the packet no detached signature; hash integrity and public commit provenance were verified, not a cryptographic signature. Public manager comments on this PR are review-start notices, not source-bank receipts. The PR body's manager re-pin statement is distinct from author execution.

| Receipt | Result / interpretation |
|---|---|
| fetch.log / fetch.rc | rc 0; freshly verified archive `2d948089927a58f351176d3c1b8ef535b6533b007b400adc548ed7a5c806f2ac`; stack/lwSRP hashes verified; generator dependencies fetched |
| make-check.log / make-check.rc | rc 0 at exact head: generated config current; media/gPTP units; peer, AECP, counters, receive failures, talker/listener and 20-cut saved-state suite pass |
| edge-probes.log / edge-probes.rc | rc 0, unchanged prior probe: accepted queued burst increments binds and clears EARLY_TIMESTAMP; separated control and early-only CRF interruption pass |
| startup-probe.log / startup-probe.rc | rc 0, unchanged prior probe: media daemon alive, both failed inputs valid 0 |
| cross-build.log / cross-build.rc, cross-SHA256SUMS | rc 0, independent exact-head aarch64 build reproduces all four author binary hashes |
| shape-reader.log / shape-reader.rc | rc 0, four FPGA rows read correctly; host row separate |
| evidence-audit.log and public-author-r4 receipts | Both author make-check runs pass; four default/strict, unbound/streaming compatibility runs report zero events, warnings/errors and query errors, no fatal enumeration error |
| integrity.log | 276 tracked blob bytes/modes verified; head/tree/index clean; BSP and all four required upstream gitlinks verified |

Exact-head burst result: bind counts `[1, 2] -> [3, 3]`, proving two bindings on input 0 and one on input 1. While the media daemon is held, both immediate GET_COUNTERS banks are valid `0xf3f` and all zero. Five subsequent immediate rebind reads pass and each binding settles. Restoration moves binds `1 -> 2`; an unbound restart preserves `2 -> 2`. Early-only PDUs interrupt both inputs without waiting for a receive timeout.

Three published F4 controls discriminate: cached GET_COUNTERS gives seven intended failures plus a separately disclosed backlog-timing failure; removing socket drain fails the fresh-binding backlog assertion; removing restart carry fails two saved-state assertions. Prior-head binaries fail the burst/reset checks. The receive-failure prior-head comparison cannot inject individual steps because those threads had no names; it is not counted as ten injected failures there. Its ordinary missing-interface control independently reproduces F5. The repaired head's fault library logs actual injection and cleanup at each step.

Both failed intermediate make-check receipts at ad9c0f09 were retained and inspected. Run 1 fails the unchanged listener rate bound: 81095 PDUs/10 seconds, 8109.5/s versus 8100. Isolated reruns measured 8032.9 and 8031.3, and the old head 7996.6. Run 2 misses a BIND_RX response/probe under stream traffic, followed by an unsettled binding and zero FRAMES_RX. ed21f9b adds kernel control-frame filtering and explicit settlement checks without relaxing counter expectations. Both final author runs and this exact-head run pass. Historical failed runs remain failures; the disclosed backlog/timing sensitivity remains a test limitation.

Compatibility receipts name v4.3.1.1 source `6d61a92e7f264c69f23cdc38f50d31114e567aa0`; the library prints internal version 4.3.1-beta1. Default output includes the five leniencies; strict output omits them. Both modes enumerate unbound and streaming inputs as IEEE17221|Milan 1.2 with zero events. These runs were inspected, not repeated here. Post-author changes are the re-pin and revision comments; independent rebuilt binaries reproduce the author identities.

| Exact-head artifact | SHA-256 |
|---|---|
| milan-ctrld | `b1254bb0de7c27f5f9b5a3fb2b3731f1828c0d213e04a7b8d51220cc650bb6b4` |
| milan-mediad | `8bef6f7693e34341c80a4e7138bdca1d8790d61d337850a47e43a048ec833f27` |
| milan-dp | `c2824e617bc51c9ae242ddfc859e83a6364d8c2daa722c25de7a21e66ef7a529` |
| milan-gptp-watch | `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` |
| entity.conf | `a34f5f7897e9ae39b2f695e61ed7d589d18da214bd2c1d9309f859494e3f610f` |

The public packet omits the milan-ctrld executable; independent rebuilding supplies its identity check. Its other three archived executables match the hashes above. The historical entity.conf hashes correctly to 532966e8…; only its generated revision line differs from this head. AEM and stream-shape headers are unchanged, and config regeneration is current. The upstream re-pin preserves host scheduling figures; that profile is not a board measurement.

**Limits and pending duties**

Complete standards texts were not independently audited; clause interpretation was traced through pinned public authorities. The CRF input clocks nothing and no Milan conformance is claimed. Missing SRP, dynamic maps, other running formats, extended AS path and REBOOT retain their documented scope. Physical calibration is NOT RUN; field skips, host timing and compatibility are not hardware proof.

The exact-head hosted query returned no pull-request-triggered runs. That is a query result, not a conclusion about every possible context. No skipped context is counted as execution. The manager owns hosted/act acceptance and final current-dev candidate validation, including builder/native banks and publishing receipts, against live dev `e275d7950ef3385753e6a15d2e191ba855b171ed`. No manager source bank exists or is requested/inferred at this head. Upstream candidate validation also owes the service-budget harness identified in its public receipt. Candidate duties do not substitute for frozen source/board acceptance.

The BSP linux gitlink remains `ab05a5e6d5f1391c5a6ba6a6a0546a693100d922`, uninitialized as received. Upstream gitlinks match the pin: processor `801ef2283ec793e35a2590fc6593c90313a64041`, gPTP `1616fc8f062fbf58c35ad31674b5b233207bbdb8`, stack `715589c1db7cf67e2f3259fd7083ebc7f0923376`, lwSRP `08ee86e812a0eb5e1dc6cd61b494d8baf3491348`. Extracted dependencies are archives, not claimed initialized submodules.

No source fixes, commits, pushes, GitHub writes, other-checkout edits, privileged actions, hardware operations or prohibited banks were performed. No simulator was used. Builds used at most 16 workers; native tests, original probes and cross building overlapped under foreground supervision, with every process completed. Disposable trees stay under packet scratch. Source bytes, modes and index are unchanged.

Portable reproduction: `sh run-fetch.sh REPO PACKET`, `sh run-check.sh REPO PACKET`, then `python3 run-edge-probes.py REPO PACKET` and `python3 run-startup-probe.py REPO PACKET`; `sh cross-build.sh REPO PACKET` reproduces target binaries. `verify-evidence.py` checks the fetched evidence commit and `verify-checkout.py` checks source integrity. MANIFEST.sha256 lists publishable scripts/receipts; scratch is excluded.

R901-3 FINISHED
