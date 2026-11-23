[R901] NEGATIVE - exact head afc350641dc2d93cee39bb4c62258b6d55f00ccb

External independent delta review of issue #29 / PR #30, R901-2. Tree `2930873abd5b2fdad7d11e72d0abc758222517d6`; source base `b6d8594c1aaff38b5e9d6b13213d90ef5e4024d4`; previous reviewed head `f405a5754afbe30313116be4662adb9d79567f9e`.

All five lenses were applied. The previous stale-gPTP defect and identity-test blind spot are fixed at their causes. Two new executable probes demonstrate a missed counter reset and counters advertised after their receive thread has failed. The PR also gives an obsolete config checksum. Frozen upstream/firmware/board acceptance remains open, now accurately disclosed.

**Reconstruction and scope**

There is no tracked BSP AGENTS.md, CONTRIBUTING.md or docs/README. Entry points were the supplied instructions, root README, milan-linux/README.md and AECP-STATUS.md. Imported authority came from the published firmware pin: AGENTS.md, CONTRIBUTING.md, docs/README.md, REQUIREMENTS.md, ENDSTATION_BUILDER section 6, MAILBOX_CONTRACT's face/binding registers, and the pinned processor integrator guide section 7.1. The latter maps Milan v1.2 5.3.8.10/Table 5.6 and 5.4.2.25/Table 5.16 to input counter events, reset edges and validity, and IEEE 1722.1-2021 7.4.42 to the read interface. Output Tables 5.4/5.17 and the distinct IEEE layout were checked too.

The [frozen issue](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29) requires the dev-soft-core pin and firmware gate, veth enumeration, successful board enumeration, and RAUC or documented manual installation. The [public scope ruling](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7096504049) replaces the original 2/2 inventory with one AAF input, one CRF input and one AAF output. It permits disclosure of missing CRF clock following with no Milan conformance claim. It does not waive upstream landing, the firmware gate or board enumeration.

The base-to-head diff/history establishes the AECP integration context; the detailed independent pass concentrated on the 41-file delta since f405a57 and changed dependencies. Commits 65fc798 and 49b2035 add counters, freshness fixes and compatibility tests. afc3506 changes the pin/hash and two generated revision comments only. Unchanged image validation/substitution, journal migration, boot ordering and face command behavior retain their previous reviewed scope, supplemented by the current regression. No BSP RTL or submodule gitlink changes occur. The RTL lens covers the host implementation of the fabric contract and its state transitions.

Public executable evidence was examined after the independent source pass. This identity's prior public findings were then reconciled. No other reviewer's report or private author material was read. `independent-verdict.md` records the independent conclusion and ledger; no other review was needed.

**R901-F4 — MAJOR — Conformance, RTL, Robustness, Tests — an accepted unbind/rebind burst misses the mandatory counter reset.**

- Location: `milan-linux/src/milan_ctrld.c:252` and `:703`; `src/aecp_far.c:54`; `tests/counters.py:294`.
- Authority/evidence: Milan v1.2 5.3.8.10 requires zeroing the input bank on each not-bound-to-bound transition. The pinned processor [integrator contract section 7.1](https://github.com/Mister-M-alt/protocol-processor-control-plane-avb-milan/blob/801ef2283ec793e35a2590fc6593c90313a64041/docs/guides/integrator.md#counters-face) states the same rule. `publish_bound()` samples only the final bound level after `ctrl_loop_service()`, which can consume several ACMP records. Starting bound, unbind followed by bind within one pass ends bound again, so the generation never increments.
- Reproduction: `run-edge-probes.py` runs exact-head unmodified daemons in a disposable network namespace. After accumulating counters, it briefly stops only the control daemon, queues UNBIND_RX and BIND_RX, and resumes it. Both responses are SUCCESS. `edge-probes.log` records `binds:1` before and after; EARLY_TIMESTAMP stays 4, while FRAMES_RX continues 10 to 11. A separated unbind/rebind positive control increments the generation and clears EARLY_TIMESTAMP. The pause makes an ordinary receive backlog reproducible.
- Impact: the new binding inherits the previous binding's measurements. The far-side generation comparison cannot suppress them because the generation did not change. Both input indices use this publication loop; the wire probe exercises input 1.
- Required outcome: observe every accepted binding transition at its owner boundary, including multiple transitions per service pass, and publish the new counter generation before the new binding's counters can be served. Preserve saved-state boot handling and lock/unlock invariants.
- Verification: retain burst and separated-transition controls; cover both inputs, repeated queued transitions and GET_COUNTERS before media processing resumes. The existing test waits 500 ms between transitions and misses this case. Re-run make check and compatibility validation after repair.

**R901-F5 — MINOR — Conformance, RTL, Robustness, Tests — failed AAF receive initialization leaves all counters valid.**

- Location: `milan-linux/src/listener.c:143` through `:154`; compare the explicit invalidation in `src/crf_rx.c:128`.
- Authority/evidence: IEEE 1722.1-2021 7.4.42 validity; the pinned integrator contract's rule to claim only counters actually kept; and this change's media.h/AECP-STATUS promise that a stream without a running counting program has none valid. The listener publishes `MILAN_SI_VALID` before opening/configuring its receive socket. Failure then returns from the thread without clearing that mask. The main process keeps publishing its last report.
- Reproduction: `run-startup-probe.py` keeps the control daemon on a valid private interface and starts the media daemon against an absent interface. `startup-probe.log` confirms the media process remains alive, input 0 reports `valid=0xf3f`, and input 1 correctly reports `valid=0`. Both receive paths encountered the same initialization failure; input 0 has no running receiver.
- Impact: a failed receive path presents apparently valid, permanently zero diagnostics. This branch also covers socket, filter, bind and receive-timeout setup failures, not just a misspelled interface.
- Required outcome: publish validity only after successful receive initialization, or clear/publish it on failure; close any opened descriptor. Make the failure visible to the process/operator, or provide a defined retry path, without claiming live counting.
- Verification: add an initialization-failure regression asserting input 0's mask clears, retain a successful initialization control, and exercise the other setup failure branches through focused injection where practical.

**R901-F6 — MINOR — Tests, Docs — the PR gives the previous config checksum as unchanged at this head.**

- Artifact: PR #30 Evidence, aarch64 checksum list; `milan-linux/config/entity.conf:3`; `integrity.log`.
- Authority/evidence: exact-head artifact provenance. The PR lists entity.conf as unchanged with SHA-256 `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c`. That correctly describes the archived author file at 49b2035. The re-pin changed its generated revision comment; the current file hashes to `532966e838a57e5944dc487a2d9be9779ecc37ff264f947d7e6d28a622a6dac9`. Regeneration at the current pin passes.
- Impact: an exact-head artifact check fails against the PR's checksum. Runtime entity fields are unchanged. This is not RESIDUE under the supplied rule: correction changes a published measurement and generated-artifact identity.
- Required outcome: replace the PR config checksum with the current value and describe it as regenerated with only the revision comment changed. Attribute archived source receipts to 49b2035/677d605f separately from current-head verification; preserve the historical packet.
- Verification: hash the exact-head generated config and compare the PR row. All four executable hashes match independent rebuilds at afc3506 and need no correction.

**R901-F3 — MAJOR, retained in part — Conformance, Tests — frozen completion acceptance remains outstanding.**

- Artifact: issue #29 Acceptance/scope ruling; PR #30 Acceptance; `milan-linux/AECP-STATUS.md:161`; upstream #983 public evidence comments.
- Authority/evidence: the pin remains PR #986's head 66b4def3, not its dev-soft-core merge. Public upstream receipts retain incomplete firmware-gate work. This head has no successful installed-board enumeration receipt. The owner reported the counter error on the earlier board build; host compatibility does not establish board acceptance.
- Impact: #29 cannot yet be considered complete under its frozen acceptance. The former inaccurate-completion-language component is resolved: the PR explicitly says it does not close #29 and lists open rows. Its former Docs attribution is cleared for that component. Remaining acceptance/evidence obligations are not cleared by the wording correction.
- Required outcome: land the authorized upstream dependency, re-pin/review its resulting code/archive, supply required author source-gate evidence, and obtain board enumeration through the authorized owner process; alternatively obtain a public owner decision explicitly changing the frozen criteria. The documented stop/replace-three/start manual route is present. No complete Buildroot execution is inferred or demanded as an extra condition of that documented alternative.
- Verification: compare the resulting exact head, firmware receipts and installed-board identity with the issue. The expected next #986 pin and ctrl_loop.h repair remain pending; this review judges current pin mechanics, not unseen future code. No manager source bank is requested or inferred.

**Prior findings disposition**

| Prior ID | Disposition | Cause-level evidence at afc3506 |
|---|---|---|
| R901-F1, MAJOR | CLOSED | First/remapped gPTP reads are baselines; update movement is required. Odd/unreadable/stopped/replacement cases pass. Original stale probe independently gives LOCKED 5 to 5, UNLOCKED 4 to 5 over 6.5 s; gptp-stale-test.log, rc 0. |
| R901-F2, MINOR | CLOSED | DUT MAC differs from the placeholder; ADP agreement is checked. All-three identity mutant fails exactly ENTITY identity, interface MAC and clock identity; identity-mutant-original-test.log, rc 1. Pristine distinct-MAC control passes. |
| R901-F3, MAJOR | PARTLY RESOLVED / RETAINED | False closure claim removed; upstream landing, source firmware evidence and successful board enumeration remain open as above. |
| R901-R1, RESIDUE | CLOSED | YAML now says `# The stream inventory (bsp#29):`. No new reviewer/round/lane identifiers found in added source comments. |
| R901-R2, RESIDUE | CLOSED | README and AECP-STATUS correctly describe the published archive, verified hash and future merge pin, while retaining image-build limits. |

**Reviewer-owned ledger**

| lens | CLEAN/UNCLEAN | examined artifacts | covering round | exact head |
|---|---|---|---|---|
| Conformance | UNCLEAN | Issue/ruling; builder/counter contracts; 1/2 inventory; AAF/CRF/output event sources, masks, intervals and resets; wire probes. F4/F5 and retained F3. | R901-2 delta/dispositions; unchanged portions retain R901-1 coverage | afc350641dc2d93cee39bb4c62258b6d55f00ccb |
| RTL | UNCLEAN | No RTL delta. Host face, multi-record service versus binding publication, freshness, counter widths/wrap, DP v2/media v3, CRF state machine/priority. F4/F5. | R901-2; unaffected architecture retains R901-1 coverage | afc350641dc2d93cee39bb4c62258b6d55f00ccb |
| Robustness | UNCLEAN | Stale/odd/replaced gPTP blocks; truncated/malformed CRF; missing roles; failed initialization; lost PDU, tu/mr, timing faults, interruption and rebind backlog. F4/F5. | R901-2; untouched boot/journal checks retain R901-1 coverage | afc350641dc2d93cee39bb4c62258b6d55f00ccb |
| Tests | UNCLEAN | Full make check; original identity/stale probes; new burst/startup probes; strict compatibility; published default/strict/negative controls; cross rebuilds/provenance. F3/F4/F5/F6. | R901-2 | afc350641dc2d93cee39bb4c62258b6d55f00ccb |
| Docs | UNCLEAN | AECP-STATUS, READMEs, YAML, pin/hash/fetch, packages/manual install, issue/PR claims. CRF/acceptance limits candid; config checksum wrong (F6). R1/R2 closed. | R901-2 | afc350641dc2d93cee39bb4c62258b6d55f00ccb |

**Counter and integration assessment**

Input masks 0xf3f and output mask 0x1f match the Milan v1.2 tables and inspected controller implementation. AAF and CRF each have event producers for all ten input counters: lock/unlock/interruption events and interval aggregation of sequence, mr, tu, format, late/early and received frames. Normal-path wire faults and interval unit tests exercise them. Output counters derive from starts/stops and sent PDUs; its zero MEDIA_RESET/TIMESTAMP_UNCERTAIN correspond to constant mr/clear tu on the actual packets. The documented absence of talker uncertainty signaling remains a limitation. Counter validity/lifetime are defective in F4/F5 despite normal-path counting passing.

CRF checks the declared format and tracks the nominal timestamp grid using a disclosed implementation-defined lock. It does not steer a media clock. AECP accepts CRF clock-source selection without media following it; the scope decision and current docs explicitly disclose this. Neither a controller compatibility flag nor this review is a Milan conformance claim.

Shared blocks are consistently versioned: datapath 2, media 3. C layouts, Python fixtures, viewer and adapter agree. Open-time version/size checks prevent a normally restarted mixed-version deployment from opening the wrong block. Manual installation stops both daemons and replaces milan-ctrld, milan-mediad and milan-dp together before restart. Hot replacement of an already mapped layout is not approved.

**Execution and evidence**

| Receipt | Result |
|---|---|
| fetch.log / fetch.rc | rc 0; published firmware archive c5b3b01c… verified; tsn-c-stack/lwSRP hash-checked and pinned generator dependencies fetched |
| make-check.log / make-check.rc | rc 0 at exact head: generated config current; media/gPTP units; peer 26, AECP 107, counters 63; talker/listener and 20-cut saved-state suite pass |
| identity-mutant-original-test.log / .rc | rc 1, exactly three intended identity failures; production wrapper detects the mutant |
| identity-mutant-distinct-test.log / .rc | rc 1, same three failures |
| identity-pristine-distinct-test.log / .rc | rc 0 |
| gptp-stale-test.log / .rc | rc 0; no fictitious relock |
| edge-probes.log / .rc | rc 1, two assertions prove F4; separated-transition control passes. Early-only CRF interruption check passed and is not a reported defect |
| startup-probe.log / .rc | rc 1, input 0 validity assertion proves F5; input 1 control passes |
| check-compat.log / .rc | rc 0; fresh strict library build, all five leniencies OFF; exact-head make check-compat passes unbound and streaming, both with zero compatibility events, warnings, errors and fatal enumeration errors |
| cross-build.log, media-cross.log and .rc files | rc 0; all four aarch64 programs rebuilt; target alsa-lib 1.2.15.3 fetched/hash-checked and installed only under packet scratch |
| integrity.log | 271 tracked blobs/modes, head/tree/index, BSP and required upstream gitlinks, package hash verified; all 91 published manifest hashes match |

Independent aarch64 rebuilds reproduce:

| Binary | SHA-256 |
|---|---|
| milan-ctrld | `9fe4012fb698f97295fd14cb4e44e238be2e4427720aaf962dcdf4433cf708b2` |
| milan-mediad | `162c8223b2ffc52484381e061e691e1467815294ba502a2637d4d27ac59a8b77` |
| milan-dp | `0c61cd3e0d4a3f2a6b72187ad31d22c5be4b8419e6a6710e514e6a58584a86f8` |
| milan-gptp-watch | `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` |

The [published packet at cdfcd0b](https://github.com/kebag-logic/ti-sitara-am65x-bsp/tree/cdfcd0bbb4bf5135947523ec1270b49f747ad1d6/review-evidence/pb2aecp-r1/author-r3) records author execution at 49b2035 with firmware 677d605f. Its two clean cross builds and default/strict controller runs were inspected; all four compatibility modes report zero events/warnings/errors. Its f405a57 and no-CRF negative controls fail with the owner's mandatory-input-counter error. The current controller was built from tag v4.3.1.1, commit 6d61a92e7f264c69f23cdc38f50d31114e567aa0. Its source confirms that the cited “Milan 1.3” input check tests the same ten-bit mask, and output interpretation follows the reported Milan version. No general equivalence of v1.2 and v1.3 is inferred.

The evidence commit has no embedded signature and its manifest lists no detached signature. SHA-256 consistency and public commit provenance were verified; no cryptographic-signature verification is claimed. The absent archived milan-ctrld was rebuilt successfully. Current-head executions above are distinct from older author receipts and future manager candidate evidence.

**Limits and pending duties**

The BSP linux gitlink remains ab05a5e6d5f1391c5a6ba6a6a0546a693100d922, uninitialized as received. Required upstream gitlinks match the pin: protocol-processor 801ef2283ec793e35a2590fc6593c90313a64041; gptp-processor 1616fc8f062fbf58c35ad31674b5b233207bbdb8; tsn-c-stack 715589c1db7cf67e2f3259fd7083ebc7f0923376; lwSRP 08ee86e812a0eb5e1dc6cd61b494d8baf3491348. Dependency extractions are disposable archives, not claimed initialized submodules.

No source fixes, commits, pushes, GitHub writes, other-checkout edits, privileged actions or board operations were performed. Commands remained under foreground supervision and children were joined. Independent probes overlapped; make builds used -j16, and the larger controller-library build used four workers to stay within the memory cap. No RTL simulator or prohibited full bank was run. Physical calibration is NOT RUN. Field skips, host compatibility and host timing are not hardware proof.

Direct download of the Milan v1.2 PDF was denied. Clause interpretation was traced through pinned public interface authorities and checked against the controller implementation; this is not an independent audit of complete IEEE/Milan texts. The exact-head hosted query returned no pull-request-triggered runs on its first page. No skipped context is counted as execution or acceptance.

The manager owns hosted/act acceptance and validation of the final current-dev candidate against live dev e275d7950ef3385753e6a15d2e191ba855b171ed, including builder/native banks and publishing receipts. No manager source bank exists or is inferred at this head. Later candidate duties do not replace missing author source-head firmware evidence. The next upstream pin must be regenerated, hashed and re-reviewed after the pending repair/merge. Board installation/enumeration belongs to the authorized owner process. Reconcile reviews before merge; retain #29 as open until frozen acceptance is satisfied or publicly changed.

Portable scripts and raw receipts are listed in MANIFEST.sha256. Run `run-check.sh REPO PACKET`, then the two new probe scripts with `REPO PACKET`. The unchanged prior probe driver takes `REPO FIRMWARE PACKET`; its old console note about an expected original-wrapper rc 0 describes the prior head, whereas the current corrected wrapper correctly returns 1 for the mutant. `build-media-cross.sh REPO PACKET` completes the target media build after that driver. For `run-compat.sh REPO PACKET`, first clone the public avdecc tag v4.3.1.1 recursively into `PACKET/scratch/avdecc`. Scratch contains disposable dependencies/builds/fixture copies and is excluded from publication. The reviewed clone's tracked bytes, modes and index remain at the requested head.

R901-2 FINISHED
