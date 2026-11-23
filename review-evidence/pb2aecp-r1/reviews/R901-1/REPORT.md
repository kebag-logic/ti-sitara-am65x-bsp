[R901] NEGATIVE - exact head f405a5754afbe30313116be4662adb9d79567f9e

Independent external review of issue #29 / PR #30, round R901-1. Tree: `16e0c4c9fb07f3b837ab45fa227c66f65912a6c7`; source base: `b6d8594c1aaff38b5e9d6b13213d90ef5e4024d4`. All five lenses were applied. Two executable probes establish a runtime defect and a test blind spot. Frozen acceptance also remains incomplete. No source change, commit, push, board operation or GitHub write was performed.

**Scope and authorities examined**

The BSP has no tracked AGENTS.md, CONTRIBUTING.md or docs/README. The supplied operating instructions, root README, milan-linux/README.md and AECP-STATUS.md were the entry points. Relevant imported authorities were read from the published firmware pin `677d605f37c3a80654df1dd2cde053b57242845b`: AGENTS.md review rules, CONTRIBUTING.md review/verification/privacy rules, docs/README.md, REQUIREMENTS.md, ENDSTATION_BUILDER section 6, MAILBOX_CONTRACT's face registers, the AECP owner/API and split-platform boot implementation. The mailbox model's ANSWER/SILENT/HOLD and commit contracts were checked against the host adapter.

The [issue body](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29) requires a pin on dev-soft-core with make check and the firmware gate, host enumeration on veth, successful board enumeration, and RAUC or documented manual installation. The [maintainer scope ruling](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/29#issuecomment-7096504049) supersedes the initial 2/2 inventory: STREAM_INPUT 0 is AAF 48 kHz/8 channels/32 bits, STREAM_INPUT 1 is CRF, and STREAM_OUTPUT 0 is AAF. ADP must advertise 1 source/2 sinks. It requires upstream #983 to land first. It permits recording the missing CRF clock following and claiming no Milan conformance; it does not waive board enumeration or the firmware gate.

[Upstream #983](https://github.com/kebag-logic/milan-fpga/issues/983) authorizes the descriptor-only host profile and host scheduling row without RTL/core changes. Its relevant interface authority is [ENDSTATION_BUILDER section 6 at the pin](https://github.com/kebag-logic/milan-fpga/blob/677d605f37c3a80654df1dd2cde053b57242845b/docs/ENDSTATION_BUILDER.md#6-the-descriptor-only-host-profile). Clause references were traced through these public authorities and the implementation; this review does not claim an independent audit of the complete standards texts.

The complete 26-file delta and three-commit history were examined. `382f122` records prerequisites; `cce8c9a` integrates AECP; `f405a57` publishes the archive hash and updates pin commentary. The final commit changes only milan-fpga.pin and the Buildroot hash file. No RTL file or BSP submodule gitlink changes in this delta. The RTL lens nevertheless covers the host replacement of the fabric interface and its state transitions.

**Findings**

**R901-F1 — MAJOR — Conformance, RTL, Robustness, Tests — stale gPTP data creates fictitious CLOCK_DOMAIN lock transitions.**

- Location: `milan-linux/src/softfab.c:191`, `:198`; `milan-linux/src/aecp_far.c:292`; `milan-linux/tests/aecp.py:493`.
- Authority/evidence: issue #29's command-serving scope; IEEE 1722.1-2021 7.4.42.2 and Milan v1.2 5.4.2.25 counter semantics; the adapter's own documented promise to count the gPTP lock transitions. At timeout, poll_gptp closes the mapping, resets gptp_fresh_ms and clears gptp_seen. On the next poll it reopens the same unchanged block and sets gptp_seen again, despite no new update. aecp_far_poll interprets these bookkeeping changes as unlock/lock events. The independent veth fault probe stops the fake publisher's updates, retaining its last consistent block. GET_COUNTERS changes from LOCKED=1/UNLOCKED=0 to LOCKED=4/UNLOCKED=3 after 6.5 seconds. See [gptp-stale-test.log](gptp-stale-test.log), with return code 1; the original suite passes before the added assertion.
- Impact: a stopped publisher can generate recurring false clock recovery events and counter notifications. Controllers receive apparently valid transition measurements that never happened. The new gptp_seen consumers make this an introduced behavior even though some polling code predates the delta. This is separate from the explicitly disclosed absence of CRF clock following.
- Required outcome: expiration must remain expired until genuinely fresh publisher data is observed. Reopening a mapping must not itself restore lock validity. Read failures, unchanged blocks and publisher restarts must have explicit freshness behavior, and the new counters must not fabricate transitions.
- Verification: keep a locked status block unchanged across multiple stale intervals and verify no new LOCKED counts; then resume updates and verify only the actual recovery. Include unreadable/odd-sequence and replacement-block cases in the regression. Re-run the command suite.

**R901-F2 — MINOR — Tests — enumeration does not test runtime identity substitution.**

- Location: `milan-linux/tests/run-netns-aecp.sh:27`, `milan-linux/tests/aecp.py:273`, `:315`; compare `config/endstation_pb2_bridge.yaml:51` and `src/milan_ctrld.c:460`.
- Authority/evidence: the host-profile contract requires verifying the generated image before replacing its three MAC-derived fields; ENTITY must agree with ADP (IEEE 1722.1-2021 Table 7-2), and AVB_INTERFACE must carry the board's MAC/clock identity (Table 7-13). The wrapper uses `02:00:00:00:00:01`, exactly the generated placeholder. In a disposable copy, omitting all three board_image substitutions still passes all 87 original checks: [identity-mutant-original-test.log](identity-mutant-original-test.log), rc 0. Changing only the fixture MAC to `02:1a:2b:3c:4d:5e` makes that mutant fail exactly the three identity assertions: [identity-mutant-distinct-test.log](identity-mutant-distinct-test.log), rc 1. The pristine exact-head daemon passes the same distinct-MAC fixture: [identity-pristine-distinct-test.log](identity-pristine-distinct-test.log), rc 0.
- Impact: the gate claims runtime identity coverage while it can accept a daemon that serves the placeholder identity on every physical board. This finding is a demonstrated test gap, not a claim that the current substitution implementation is broken.
- Required outcome: run enumeration with a DUT MAC different from the generated placeholder and retain checks of all three fields and ADP agreement.
- Verification: the pristine daemon passes; omission of any runtime identity substitution is detected. [run-probes.py](run-probes.py) reproduces the demonstrated all-three mutation without editing the reviewed checkout.

**R901-F3 — MAJOR — Conformance, Tests, Docs — the published completion claim is ahead of frozen acceptance.**

- Artifact: issue #29 Acceptance; its maintainer scope ruling; PR #30's “Closes #29 for the control plane”; `milan-linux/milan-fpga.pin:6`; `milan-linux/AECP-STATUS.md:108`; upstream PR #986's stated gate limits.
- Authority/evidence: the ruling says to land #983 first, then pin, generate and integrate. [PR #986](https://github.com/kebag-logic/milan-fpga/pull/986) is still open/unmerged in the inspected public snapshot. This pin is its head, not its dev-soft-core merge. The author reports selected firmware-gate arms, with the mutation shards not run. The board acceptance has no execution receipt; the author and PR explicitly say nothing was installed on a board. The public comments contain no decision replacing those criteria with a control-plane-only closure. See `public/issue-29.json`, `public/issue-29-comments.json`, `public/pr-30.json` and `public/upstream-pr.json`.
- Impact: accepting or closing #29 now would silently narrow its frozen acceptance. Host enumeration and cross compilation do not establish physical controller enumeration or a complete firmware gate.
- Required outcome: retain these criteria as open. Land the authorized upstream dependency and update the reviewed pin; publish the required source firmware-gate evidence; obtain the board enumeration result through the authorized owner process. If scope is to be deferred, obtain an explicit public owner ruling and track the deferred acceptance. Make the PR completion language match that decision. The documented RAUC/package integration is present, but no complete Buildroot image execution is claimed here.
- Verification: re-review the resulting exact head and its complete gate receipts against the issue. Board evidence must identify the installed image and successful controller enumeration. This finding requests no manager source bank: the author owns source-head gate receipts; the manager's later current-dev candidate banks are a separate duty.

**R901-R1 — RESIDUE — Docs — remove a review-round reference from a source comment.**

- Location: `milan-linux/config/endstation_pb2_bridge.yaml:15`.
- Authority/evidence: the supplied owner comment rule prohibits new review-round references in source comments. The added line reads `# The stream inventory (bsp#29, the ruling of round 1):`.
- Impact: wording residue only; no code, measurement, generated artifact or conformance change is requested.
- Exact fix: replace it with `# The stream inventory (bsp#29):`. Preserve the behavior and clause explanations below it.
- Verification: inspect the added source comments for review-round, lane and reviewer references. This does not make a lens unclean or affect the verdict.

**R901-R2 — RESIDUE — Docs — archive availability prose was not refreshed with the final pin commit.**

- Location: `milan-linux/README.md:215`; `milan-linux/AECP-STATUS.md:9` and `:108`.
- Authority/evidence: the prose says the archive is unpublished, the hash is `provisional`, a local checkout is needed and Buildroot cannot download it. At this head the pin and package hash contain the published hash; [fetch.log](fetch.log) records a successful verified fetch.
- Impact: obsolete setup wording; this correction changes no executed test result, generated artifact or conformance claim. The full Buildroot build remains unverified.
- Exact fix: replace the README paragraph with “The pin names the published head of milan-fpga PR #986. tools/fetch-milan-fpga.sh verifies its archive hash; no local checkout is required. The pin must move to the upstream merge commit after #986 lands.” Replace AECP-STATUS's opening pin paragraph with the same facts, retaining the #983 profile reference. In its final paragraph replace “The Buildroot build waits for the published archive.” with “The Buildroot image build has not been run with the published archive.” Retain the existing board and partial-firmware-gate limitations.
- Verification: prose agrees with the pin, package hashes and fetch behavior. This does not make a lens unclean or affect the verdict.

**Reviewer-owned ledger**

| lens | CLEAN/UNCLEAN | examined artifacts and result | covering round | exact head |
|---|---|---|---|---|
| Conformance | UNCLEAN | issue #29/ruling; generated 1/2 inventory, model ID and 32 descriptors; daemon image checks; veth command results. F1 counters and F3 acceptance remain open. | R901-1 | f405a5754afbe30313116be4662adb9d79567f9e |
| RTL | UNCLEAN | No RTL delta. Makefile compiles imported firmware unchanged; host shape is forced into every unit. Compared load/compose/restore/open/release ordering to ctrl_plat.c; face bit packing, index limits and SILENT/commit semantics to mailbox contract 2.3 and mbx_model.h. F1 freshness state transition is incorrect. | R901-1 | f405a5754afbe30313116be4662adb9d79567f9e |
| Robustness | UNCLEAN | Checked CRC-before-substitution, model/count checks, bounded stream/word selectors, refused edits, missing-data behavior, KLJ2 size detection/read-only migration and KLJ3 file ranges. Distinct-MAC control passes; stale-publisher fault proves F1. | R901-1 | f405a5754afbe30313116be4662adb9d79567f9e |
| Tests | UNCLEAN | Native build, config regeneration, media/gPTP units, peer 27/27, AECP 87/87, cross build; inspected published full make-check and migration receipts. F1 missing fault coverage, F2 surviving identity mutation and F3 incomplete required evidence. | R901-1 | f405a5754afbe30313116be4662adb9d79567f9e |
| Docs | UNCLEAN | README, AECP-STATUS, config comments, pin/hash/fetch and Buildroot integration, issue and PR claims. Nonconformance limits are explicitly documented. F3 completion claim remains open; R1/R2 are nonblocking wording residue. | R901-1 | f405a5754afbe30313116be4662adb9d79567f9e |

**Execution and published evidence**

All review drivers remained in the foreground and joined their work. Independent lightweight checks and veth probes ran concurrently; each build used `make -j16`, with at most one such build active. Temporary downloads, generated files, builds and mutation copies live only under this packet's `scratch/`. No tracked source was mutated.

| Receipt | Result |
|---|---|
| fetch.log / fetch.rc | rc 0; firmware, tsn-c-stack and lwSRP archives checked against the pin; generator dependencies fetched at their pinned revisions |
| native-build.log / native-build.rc | rc 0; all four host programs and both unit programs built |
| generated-config.log / generated-config.rc | rc 0; all four generated config files equal regeneration |
| media-unit.log, gptp-unit.log | both rc 0 |
| control-veth.log | rc 0, 27 checks pass |
| aecp-veth.log | rc 0, 87 checks pass, all 32 descriptors enumerated, both inputs bound, legacy journal migration checked |
| identity-mutant-original-test.log | rc 0 despite omitted substitutions: demonstrates F2 |
| identity-mutant-distinct-test.log | rc 1, exactly three identity failures: positive detection control |
| identity-pristine-distinct-test.log | rc 0: implementation control |
| gptp-stale-test.log | rc 1, false lock transition assertion: demonstrates F1 |
| cross-build.log | rc 0; aarch64 control daemon, datapath viewer and gPTP watcher built; target media daemon not built |
| binary-checksums.txt | aarch64 milan-ctrld `04e44ea074b28eacc31218f34964e0d6f7f35b1ab0c88466dfee53d817070cfb`; entity.conf `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c`; both match published receipts |
| integrity.log / integrity.rc | rc 0; head/tree, index, 259 tracked blob bytes/modes, clean worktree and required gitlinks verified |

The [published evidence packet](https://github.com/kebag-logic/ti-sitara-am65x-bsp/tree/80a41913fd9aae9698e746dc669236787b21a629/review-evidence/pb2aecp-r1) was read after the independent delta pass. Its INDEX/git-state identify `cce8c9a`, not `f405a57`. Its make-check log includes successful talker, listener and 20-cut journal tests; those were inspected, not rerun in this review. The two heads have the same implementation and generated config bytes; the final delta is pin-publication metadata. The exact-head reviewer checks above supplement that evidence rather than relabeling it. The six copied author receipts match their published manifest SHA-256 values (integrity.log). The full firmware gate is not present in these receipts; the upstream public PR explicitly lists its omissions.

The exact-head hosted workflow query returned no pull-request-triggered runs. This is an observation about that query, not a successful hosted gate or proof that no other event contexts exist. No skipped context was counted as executed. Hosted/act acceptance belongs to the manager.

**Prior findings and independence**

The independent verdict and ledger were written to `independent-verdict.md` before the final review-history queries. PR #30's public timeline contains the review-start comment, and the review and review-thread queries return empty lists. There are no prior public reviewer FINDINGS to resolve or retain in those results. No other reviewer's report was read. The earlier author's prerequisite diagnosis is resolved in part: host generation and the authorized 1/2 inventory now work, and the unpublished-archive problem is fixed. Upstream landing, complete firmware evidence and board acceptance remain retained in F3. The public manager scope/start comments were examined; no manager source-bank receipt is claimed or inferred.

**Limits and pending manager duties**

This is a source-head review. It is not validation of the final current-dev merge candidate. The task identifies live dev as `e275d7950ef3385753e6a15d2e191ba855b171ed`; that candidate has not been built by this reviewer. The manager must build the current-dev candidate at the merge turn, run the required builder/native banks, own hosted/act acceptance, publish receipts on the PR and reconcile all independent reviews. Those future banks do not retroactively supply missing author source-head evidence.

No full parent, protocol-processor, gPTP, synthesis, builder or firmware bank was run. No RTL simulator was invoked; the architecture/interface review does not claim new RTL execution evidence or upstream FPGA artifact identity. No hardware, physical calibration, deployment or complete Buildroot image test was performed. Physical calibration is NOT RUN; field skips and host timings are not hardware proof. The disclosed CRF-clock, dynamic-map, format, AS-path, SRP and reboot limits remain limitations, not conformance approval.

The BSP linux gitlink remains `ab05a5e6d5f1391c5a6ba6a6a0546a693100d922`, uninitialized and empty as received. The pin's protocol-processor `801ef2283ec793e35a2590fc6593c90313a64041`, gptp-processor `1616fc8f062fbf58c35ad31674b5b233207bbdb8`, tsn-c-stack `715589c1db7cf67e2f3259fd7083ebc7f0923376` and lwSRP `08ee86e812a0eb5e1dc6cd61b494d8baf3491348` agree with the public upstream gitlink metadata. Dependency archives used for tests are disposable extractions, not claimed initialized git submodules.

Portable reproduction: run `run-focused.py REPO FIRMWARE PACKET`, then `run-probes.py REPO FIRMWARE PACKET`; FIRMWARE is the tree fetched by the repository's `tools/fetch-milan-fpga.sh FIRMWARE --with-submodules`, and must reside under PACKET/scratch. Set TMPDIR under PACKET/scratch for fetching. `verify-tree.py REPO PACKET` verifies the reviewed checkout and saved public receipts. MANIFEST.sha256 lists the publishable files; scratch is excluded. R1/R2 belong on the manager's residue checklist; F1/F2/F3 require resolution and re-review.

R901-1 FINISHED
