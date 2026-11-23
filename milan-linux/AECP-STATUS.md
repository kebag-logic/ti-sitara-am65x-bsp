# PocketBeagle 2 AECP: round 1 stopped

Status: STOP, pending an upstream descriptor-generation profile for the board.
ETA: implementation cannot be scheduled until that profile and the stream
inventory are settled. This is evidence for issue #29, not a completed AECP
integration or a claim of Milan compliance.

The examined milan-fpga revision is
`854bd7236792ce1adbd10c43b13a6a5002ad3bb6`. Its GitHub archive SHA-256 is
`771dd977c21eb2c3428641d951e3455ea22681f6d0b203e7c51d28ac68ebbd38`.
The archive and its generator dependencies were fetched and the following
probes run against that unmodified source. The production pin remains unchanged:
an incomplete pin migration would leave the existing build broken.

## Required upstream boundary

Both `sw/firmware/ctrl/adp/adp_entity.py` and
`sw/firmware/ctrl/aecp/aecp_entity.py` call the same builder's `load_config`.
At this revision, `sw/builder/endstation_builder.py:4006` accepts only `arty`
and `ax7101` board targets. `load_config` at line 4625 also requires the
fabric gPTP owner. A configuration selecting the board is refused, as is
an otherwise accepted diagnostic configuration with `fabric_gptp: false`.
The bridge actually uses Linux's CPTS PHC and flexptpd, as described by its
startup script and the board's checked-in `avb.env`.

The smallest necessary upstream change identified here is a descriptor-only
Linux platform profile in the existing builder/config loader. It must accept
the host's audio, clock and interface facts without imposing FPGA pin/clock
constraints or a fabric gPTP owner, and feed the existing ADP, AEM image and
shape generators unchanged. Existing FPGA profiles must retain their checks.
Adding just a board-name alias would not remove the gPTP-owner constraint.
This is a generator/schema change, not a C build flag. No such change was
made here, and no need to change the portable AECP command core was established.

## Stream inventory needs clarification

The request specifies two AAF sources and two sinks. The current daemon runs
one talker and one listener, both index 0 (`src/milan_mediad.c:262` and `:287`),
at 48 kHz, eight channels, signed 32-bit AAF samples, six samples per packet.
`milan-bridge.sh` does not override the channel count or select another stream.
Its current generated ADP counts of two each describe the older one-AAF plus
one-CRF shape; they do not demonstrate two active AAF streams per direction.
The listener also requires the configured channel count (`src/listener.c:212`).
Advertising additional stream formats or clock sources requires corresponding
media support; a descriptor image alone does not supply it.

Milan v1.2 sections 7.2.2 and 7.2.3 require a CRF input for an AAF talker,
and CRF input/output for a listener with two AAF inputs per clock domain.
Consequently:

| Interpretation | Generated total outputs/inputs | Firmware shape check |
| --- | --- | --- |
| Two AAF outputs, one AAF input and one CRF input, no CRF output | 2/2 | Accepted |
| Two AAF outputs and two AAF inputs, with CRF input/output | 3/3 | Refused |
| Two AAF outputs and two AAF inputs, no CRF | Not generated | Builder refuses missing CRF output |

For 3/3, `sw/firmware/ctrl/loop/ctrl_loop.h:397` emits its `THE SHAPES`
error. Its timing rows only cover 2/2 and 9/9, at one or two interfaces.
The generated `CTRL_SRP_SOURCES` and `CTRL_SRP_SINKS` must describe the actual
image; overriding them to another shape or dropping the generated header
would silently use timing assumptions for a different entity. If 3/3 is the
intended inventory, an additional upstream change must supply a justified
timing row for that shape, or an explicit host scheduling profile, with its
own checks. The present probe establishes a compile refusal, not timing
measurements or bounds for Linux.

## Reproduction

Unpack the archive of the revision above and place its pinned generator
dependencies under it:

| Directory | Revision |
| --- | --- |
| `protocol-processor` | `801ef2283ec793e35a2590fc6593c90313a64041` |
| `gptp-processor` | `1616fc8f062fbf58c35ad31674b5b233207bbdb8` |

With PyYAML and a host C compiler available, run from `milan-linux`:

```sh
python3 -B tests/probe-aecp-prerequisites.py /path/to/milan-fpga /tmp/pb2-aecp-probe
```

The probe writes each input, generated shape header and complete command log
outside the firmware tree. It requires the named generator refusals and the
3/3 compile refusal, plus a successful 2/2 control. Its diagnostic inputs
inherit FPGA settings solely to isolate each check; they are not deployable
configurations for the board. A successful probe is evidence of the blocker,
not a passing firmware gate or an enumeration test.

The pin move would also require `third_party/tsn-c-stack` at
`715589c1db7cf67e2f3259fd7083ebc7f0923376`: ADP, ACMP, MAAP and the wire layer
have moved there. The host and Buildroot archive recipes currently omit it.

## Work not completed

No production pin change, AECP integration, generated board configuration,
descriptor image, `make check`, firmware gate, veth enumeration test or
aarch64 cross build is claimed. No board was contacted or installed, and
no branch was pushed. IEEE 1722.1-2021 section 7.2.1's required Configuration
descriptor remains the original unresolved issue. Work stopped at the
explicit instruction to report any needed upstream change beyond a build flag.
