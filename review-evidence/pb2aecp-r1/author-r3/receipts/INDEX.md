# Round 3 receipts (BSP #29, PR #30)

BSP branch `pb2-aecp-1`, head `49b2035605729e74d1fc664fb25304402e758ea4` (tree
`2a5536586e8c3b81a2c40a61810c85367e56eaf9`), on `f405a5754afbe30313116be4662adb9d79567f9e`:

- `65fc798b135fd66a5eff59e6f3418e9959c031da`: the stream counters milan-mediad keeps for GET_COUNTERS; the gPTP status live only while flexptpd updates it; a DUT MAC distinct from the image's
- `49b2035605729e74d1fc664fb25304402e758ea4`: the la_avdecc controller check (`make check-compat`), AECP-STATUS and the READMEs

Local commits, not pushed. No board, bench or remote host was contacted.
milan-fpga is pinned at `677d605f37c3a80654df1dd2cde053b57242845b` (PR #986's published head), unchanged.

## The aarch64 binaries

Built from the head's clean tree, twice from empty output directories, with
identical results (logs/cross-build-1.log, logs/cross-build-2.log):

    make MILAN_FPGA=.milan-fpga O=<out> CC=aarch64-linux-gnu-gcc \
         CFLAGS=-I<sysroot>/include LDFLAGS=-L<sysroot>/lib all

aarch64-linux-gnu-gcc 16.2.0, GNU ld 2.47. The sysroot holds alsa-lib 1.2.15.3, cross-built from the
official release tarball (`alsa-lib-1.2.15.3.tar.bz2`, sha256
`7b079d614d582cade7ab8db2364e65271d0877a37df8757ac4ac0c8970be861e`) for its headers and
`libasound.so.2` only: milan-mediad links `libasound.so.2` dynamically, so the board's own alsa-lib
serves it. Each binary needs at most GLIBC_2.38, as the round-2 milan-ctrld running on the board does;
milan-mediad's ALSA symbols are versioned ALSA_0.9 and ALSA_0.9.0. Its ALSA code (`src/audio_alsa.c`) is
unchanged since the image's build.

| File | sha256 | Changed since round 2 |
| --- | --- | --- |
| `milan-ctrld` | `9fe4012fb698f97295fd14cb4e44e238be2e4427720aaf962dcdf4433cf708b2` | yes (round 2: `04e44ea0...`) |
| `milan-mediad` | `162c8223b2ffc52484381e061e691e1467815294ba502a2637d4d27ac59a8b77` | yes (round 2 did not cross-build it; the board runs the image's) |
| `milan-dp` | `0c61cd3e0d4a3f2a6b72187ad31d22c5be4b8419e6a6710e514e6a58584a86f8` | yes (round 2: `8defbfe9...`) |
| `milan-gptp-watch` | `8ea48e0503e9a29ba215c21e8e3f00fa2cd9676ec65056f7814b1376428b888c` | no, byte-identical |
| `entity.conf` | `e273ff975fc157b2f48939532c203abbd3464f476d1f02edd52e93ee5752556c` | no, byte-identical |
| `milan-bridge.sh` | `6cf38b8dac1ae9946afe0e5524ac1c372747764f26a5602e9e54cb75badbb488` | comment only |

[aarch64/SHA256SUMS](aarch64/SHA256SUMS) lists them.

## Install list

| Board path | From | Must |
| --- | --- | --- |
| `/usr/sbin/milan-ctrld` | `aarch64/milan-ctrld` | yes |
| `/usr/sbin/milan-mediad` | `aarch64/milan-mediad` | yes |
| `/usr/sbin/milan-dp` | `aarch64/milan-dp` | yes |
| `/usr/bin/milan-gptp-watch` | `aarch64/milan-gptp-watch` | no (unchanged) |
| `/etc/milan/entity.conf` | `aarch64/entity.conf` | no (unchanged) |
| `/usr/sbin/milan-bridge.sh` | `aarch64/milan-bridge.sh` | no (a comment changed) |

**milan-ctrld, milan-mediad and milan-dp go in together.** The datapath block
moved to version 2 (each sink's bound state and bind count) and the media block
to version 3 (the Milan counters, the CRF receive path). Each program refuses a
block of another version, so a new milan-ctrld with the image's milan-mediad
would leave milan-mediad waiting for its datapath block, and GET_COUNTERS with
no stream counter valid. Stop both daemons (`/usr/sbin/milan-bridge.sh stop`),
replace the three, start again (`/usr/sbin/milan-bridge.sh start`, or reboot).
milan-bridge.sh starts milan-mediad without `-r`, so all three roles run,
the CRF receive path among them. The saved-state journal (KLJ3) and the AEM image
are unchanged.

On the board, `milan-dp` then prints `milan_stream_input0`, `milan_stream_input1`
and `milan_stream_output0` with `valid:f3f`, `valid:f3f` and `valid:01f`.

## Validation at the head

| Check | Result | Receipt |
| --- | --- | --- |
| `make check` | rc 0 in 2 min 46 s: `config/` current; `test_media`, `test_gptp`; peer 26 checks; aecp 107; counters 63; talker, listener, nvm suites | [logs/make-check.log](logs/make-check.log) |
| `make check-compat`, library defaults | rc 0: unbound, and `streams`; PASS, Milan 1.2, zero events, warnings or errors | [logs/compat-default-options.log](logs/compat-default-options.log) |
| `make check-compat`, leniencies off | rc 0: the same | [logs/compat-strict-options.log](logs/compat-strict-options.log) |
| the same program, build at f405a57 | FAIL: `[Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor` | [logs/compat-f405a57-negative.log](logs/compat-f405a57-negative.log) |
| the same program, milan-mediad without `crf` | FAIL, the same clause: STREAM_INPUT 1 none valid | [logs/compat-without-crf-negative.log](logs/compat-without-crf-negative.log) |
| R901's identity mutant (all three substitutions removed), the suite's own wrapper | rc 1: exactly the three identity checks fail | [logs/r901-probe-identity-mutant-original-test.log](logs/r901-probe-identity-mutant-original-test.log) |
| R901's pristine daemon, distinct MAC | rc 0 | [logs/r901-probe-identity-pristine-distinct-test.log](logs/r901-probe-identity-pristine-distinct-test.log) |
| R901's gPTP-stale probe | rc 0: LOCKED 5 before and after 6.5 s of a stopped publisher; UNLOCKED 4 to 5 | [logs/r901-probe-gptp-stale-test.log](logs/r901-probe-gptp-stale-test.log) |
| aarch64 cross build, twice | rc 0, identical | [logs/cross-build-1.log](logs/cross-build-1.log), [logs/cross-build-2.log](logs/cross-build-2.log) |
| `git diff --check f405a57..HEAD` | clean | |

R901's probes are its `run-focused.py` and `run-probes.py`, run unchanged against the head's checkout,
with their outputs in a scratch packet.

## la_avdecc

L-Acoustics' avdecc at tag `v4.3.1.1` (commit `6d61a92e7f264c69f23cdc38f50d31114e567aa0`, the latest
4.3.x tag; it reports itself `4.3.1-beta1`, built outside its release scripts), submodules at their
gitlinks. Two CMake builds (CMake 4.5.0, Ninja 1.13.2, GCC 16.2.1, Release) of
`la_avdecc_controller_static`:

- library defaults: `IGNORE_INVALID_CONTROL_DATA_LENGTH`, `IGNORE_INVALID_NON_SUCCESS_AEM_RESPONSES`,
  `IGNORE_NEITHER_STATIC_NOR_DYNAMIC_MAPPINGS`, `CONTINUE_MISBEHAVE_AEM_RESPONSES` and
  `ALLOW_GET_AUDIO_MAP_UNSOL` on (the program prints them as IICDL IINSAR INSNDM CMAR AGAMU);
- strict: those five `OFF`.

`tests/milan-compat/build.sh` links the program against either.

## The compatibility run's full output

### Library defaults: unbound, then `streams`

```text
tests/milan-compat/build.sh $VALIDATION_STORAGE/pb2aecp-a830/lavdecc/src $VALIDATION_STORAGE/pb2aecp-a830/lavdecc/build $VALIDATION_STORAGE/pb2aecp-a830/check-r3/milan-compat
tests/milan-compat/run-netns-compat.sh $VALIDATION_STORAGE/pb2aecp-a830/check-r3 $VALIDATION_STORAGE/pb2aecp-a830/check-r3/milan-compat config/entity.conf 15
--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---
la_avdecc 4.3.1-beta1, controller library 4.3.1-beta1, compiled with: IICDL IINSAR AGAMU ARBAP RDNCY JSN INSNDM CMAR RDNCY RDNCY2018 JSN CBR; on veth1
online: entity 021a2bfffe3c4d5e model 001bc50ac1000100, compatibility IEEE17221|Milan, Milan 1.2.0.0, subscribed to unsolicited notifications, GET_DYNAMIC_INFO not used
settling 15 s
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=4
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=6
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=8
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=10
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=12
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=14
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=16
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=18
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=20
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=22
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=24
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=26
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=28
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=30
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=32
final: entity 021a2bfffe3c4d5e, compatibility IEEE17221|Milan, Milan 1.2.0.0, 0 compatibility events, 0 warnings/errors logged, 0 query errors, fatal enumeration error no
model STREAM_INPUT 0 'Stream In 0' state 0 talker ffffffffffffffff/65535, format 0205022002006000, counters 1=0 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=0
model STREAM_INPUT 1 'CRF' state 0 talker ffffffffffffffff/65535, format 041060010000bb80, counters 1=0 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=0
model STREAM_OUTPUT 0 format 0205022002006000, counters (Milan 1.2 layout) 1=1 2=0 4=0 8=0 16=32
model CLOCK_DOMAIN 0 source 0, counters 1=1 2=0
model AVB_INTERFACE 0 clock identity 021a2bfffe3c4d5e, gPTP grandmaster 3cc0c6fffe0a0001, AS path 2 entries, counters 1=1 2=0 32=1
verdict: PASS - Milan 1.2.0.0 compatible, zero compatibility events, warnings or errors
--- milan-dp ---
link_up=1
gm_id=3cc0c6fffe0a0001
gm_domain=0
maap_valid=1
maap_base=91e0f00000b4
maap_count=1
source0=stream_id:021a2b3c4d5e0000 dest_mac:91e0f00000b4 vlan:2 dest_mac_valid:1
sink0=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0 bound:0 binds:0
sink1=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0 bound:0 binds:0
milan_stream_output0=valid:01f binds:0 1 0 0 0 33 0 0 0 0 0 0 0
milan_stream_input0=valid:f3f binds:0 0 0 0 0 0 0 0 0 0 0 0 0
milan_stream_input1=valid:f3f binds:0 0 0 0 0 0 0 0 0 0 0 0 0
--- ctrld.log
AEM image: 5784 bytes, 32 descriptors, generated CRC e937544f verified; this board's identity 021a2bfffe3c4d5e, CRC 5b73f169
saved state at boot (/tmp/tmp.GoxJuOEYSc/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
MAAP: no addresses
entity 021a2bfffe3c4d5e on veth0 (mac 021a2b3c4d5e, model 001bc50ac1000100, 1 sources, 2 sinks, no SRP domain): AECP open
link up, grandmaster 0000000000000000 domain 0
link up, grandmaster 3cc0c6fffe0a0001 domain 0
MAAP: 1 addresses from 91e0f00000b4
entity 021a2bfffe3c4d5e mac 021a2b3c4d5e | link up gm 3cc0c6fffe0a0001 asCapable 1 | loop passes 4172 events 1898 rx 54 bad 0 | fabric rx 54 admitted 54 tx 79 err 0 lost 0 filter_mismatch 0
AECP open, unlocked 0000000000000000 | face commits 1 late 0 unanswered 0 | far answers 957 silent 0 commits 1 edits refused 0 | media block read
AECP core: ready 1 response_owed 0 notify 0 owed 0 waiting 0 refusing 0 | busy drops 0 malformed 0 ignored 0
ingress rate drops: adp 0 acmp 0 aecp 0 maap 0 | rx drops: adp 0 acmp 0 aecp 0 maap 0
MAAP valid base 91e0f00000b4 count 1
saved state now (/tmp/tmp.GoxJuOEYSc/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
sink 0: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
sink 1: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
departed
--- mediad.log
veth0 has no PTP clock: gPTP time is CLOCK_TAI
waiting for milan-ctrld's datapath block /milan-dp-compat-1905392
talker on veth0, 8 channels, PTO 2000000 ns, level 48..192 frames, PHC -1
listener on veth0, 8 channels, USB IN pipeline 0 ns
CRF input (sink 1) on veth0: counted, not clocked from
crf idle stream 0000000000000000 | PDUs 0 seq 0 late 0 early 0 unsupported 0 tu 0 mr toggles 0 | locked 0 unlocked 0 interrupted 0 grid breaks 0 | rate +0.000 ppm
listener idle stream 0000000000000000 | FRAMES_RX 0 SEQ_NUM_MISMATCH 0 LATE 0 EARLY 0 UNSUPPORTED 0 MEDIA_LOCKED 0 MEDIA_UNLOCKED 0 jumps 0 STREAM_INTERRUPTED 0 tu 0 mr toggles 0 | underruns 0 pitch 1000000 | presentation error 0..0 avg 0 ns, least margin 0 ns
talker idle stream 021a2b3c4d5e0000 -> 91e0f00000b4 | PDUs 132374 underruns 0 overruns 0 late 115627 send errors 0 | level 46..52 avg 48.07 (target 48) pitch 1000000 | worst send delay 87578 ns | gPTP rate +0.000 ppm residual 0 ns
compat: PASS
tests/milan-compat/run-netns-compat.sh $VALIDATION_STORAGE/pb2aecp-a830/check-r3 $VALIDATION_STORAGE/pb2aecp-a830/check-r3/milan-compat config/entity.conf 15 streams
--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---
la_avdecc 4.3.1-beta1, controller library 4.3.1-beta1, compiled with: IICDL IINSAR AGAMU ARBAP RDNCY JSN INSNDM CMAR RDNCY RDNCY2018 JSN CBR; on veth1
online: entity 021a2bfffe3c4d5e model 001bc50ac1000100, compatibility IEEE17221|Milan, Milan 1.2.0.0, subscribed to unsolicited notifications, GET_DYNAMIC_INFO not used
settling 15 s
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=8
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=8
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=10
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=10
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=10
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=12
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=12
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=12
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=14
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=14
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=14
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=16
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=16
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=16
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=18
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=18
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=18
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=20
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=20
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=20
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=22
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=22
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=22
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=24
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=24
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=24
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=26
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=26
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=26
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=28
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=28
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=28
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=30
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=30
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=30
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=32
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=32
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=32
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=34
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=34
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=34
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=36
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=38
final: entity 021a2bfffe3c4d5e, compatibility IEEE17221|Milan, Milan 1.2.0.0, 0 compatibility events, 0 warnings/errors logged, 0 query errors, fatal enumeration error no
model STREAM_INPUT 0 'Stream In 0' state 2 talker 020000fffe000002/0, format 0205022002006000, counters 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
model STREAM_INPUT 1 'CRF' state 2 talker 020000fffe000002/1, format 041060010000bb80, counters 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
model STREAM_OUTPUT 0 format 0205022002006000, counters (Milan 1.2 layout) 1=1 2=0 4=0 8=0 16=38
model CLOCK_DOMAIN 0 source 0, counters 1=1 2=0
model AVB_INTERFACE 0 clock identity 021a2bfffe3c4d5e, gPTP grandmaster 3cc0c6fffe0a0001, AS path 2 entries, counters 1=1 2=0 32=1
verdict: PASS - Milan 1.2.0.0 compatible, zero compatibility events, warnings or errors
--- milan-dp ---
link_up=1
gm_id=3cc0c6fffe0a0001
gm_domain=0
maap_valid=1
maap_base=91e0f00000b4
maap_count=1
source0=stream_id:021a2b3c4d5e0000 dest_mac:91e0f00000b4 vlan:2 dest_mac_valid:1
sink0=stream_id:0200000000020000 dest_mac:91e0f000ab00 vlan:2 listening:1 bound:1 binds:1
sink1=stream_id:0200000000020001 dest_mac:91e0f000ab01 vlan:2 listening:1 bound:1 binds:1
milan_stream_output0=valid:01f binds:0 1 0 0 0 39 0 0 0 0 0 0 0
milan_stream_input0=valid:f3f binds:1 1 0 0 0 0 0 0 0 0 0 0 38
milan_stream_input1=valid:f3f binds:1 1 0 0 0 0 0 0 0 0 0 0 37
--- ctrld.log
AEM image: 5784 bytes, 32 descriptors, generated CRC e937544f verified; this board's identity 021a2bfffe3c4d5e, CRC 5b73f169
saved state at boot (/tmp/tmp.e76qAGrR7A/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
MAAP: no addresses
entity 021a2bfffe3c4d5e on veth0 (mac 021a2b3c4d5e, model 001bc50ac1000100, 1 sources, 2 sinks, no SRP domain): AECP open
link up, grandmaster 0000000000000000 domain 0
link up, grandmaster 3cc0c6fffe0a0001 domain 0
MAAP: 1 addresses from 91e0f00000b4
sink 0: bound, talker 020000fffe000002/0, acmp status 0
sink 0: listening to stream 0200000000020000 at 91e0f000ab00, VLAN 2
sink 0: bound, talker 020000fffe000002/0, acmp status 0
sink 0: bound, talker 020000fffe000002/0, acmp status 0
sink 1: bound, talker 020000fffe000002/1, acmp status 0
sink 1: listening to stream 0200000000020001 at 91e0f000ab01, VLAN 2
sink 1: bound, talker 020000fffe000002/1, acmp status 0
sink 1: bound, talker 020000fffe000002/1, acmp status 0
entity 021a2bfffe3c4d5e mac 021a2b3c4d5e | link up gm 3cc0c6fffe0a0001 asCapable 1 | loop passes 4944 events 2279 rx 58 bad 0 | fabric rx 58 admitted 58 tx 113 err 0 lost 0 filter_mismatch 0
AECP open, unlocked 0000000000000000 | face commits 5 late 0 unanswered 0 | far answers 2334 silent 0 commits 5 edits refused 0 | media block read
AECP core: ready 1 response_owed 0 notify 0 owed 0 waiting 0 refusing 0 | busy drops 0 malformed 0 ignored 0
ingress rate drops: adp 0 acmp 0 aecp 0 maap 0 | rx drops: adp 0 acmp 0 aecp 0 maap 0
MAAP valid base 91e0f00000b4 count 1
saved state now (/tmp/tmp.e76qAGrR7A/journal.bin): slot A seq 1 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 1 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
sink 0: state 7 bound 1 talker 020000fffe000002/0 started 1 status 0
sink 1: state 7 bound 1 talker 020000fffe000002/1 started 1 status 0
departed
--- mediad.log
veth0 has no PTP clock: gPTP time is CLOCK_TAI
waiting for milan-ctrld's datapath block /milan-dp-compat-1906665
talker on veth0, 8 channels, PTO 50000000 ns, level 48..192 frames, PHC -1
listener on veth0, 8 channels, USB IN pipeline 0 ns
CRF input (sink 1) on veth0: counted, not clocked from
crf active, locked stream 0200000000020001 | PDUs 9289 seq 0 late 0 early 0 unsupported 0 tu 0 mr toggles 0 | locked 1 unlocked 0 interrupted 0 grid breaks 0 | rate +0.000 ppm
listener active, locked stream 0200000000020000 | FRAMES_RX 38238 SEQ_NUM_MISMATCH 0 LATE 0 EARLY 0 UNSUPPORTED 0 MEDIA_LOCKED 1 MEDIA_UNLOCKED 0 jumps 0 STREAM_INTERRUPTED 0 tu 0 mr toggles 0 | underruns 0 pitch 1000016 | presentation error -103460..19603 avg 11745 ns, least margin 49892457 ns
talker idle stream 021a2b3c4d5e0000 -> 91e0f00000b4 | PDUs 156297 underruns 0 overruns 0 late 135848 send errors 0 | level 47..50 avg 48.24 (target 48) pitch 1000000 | worst send delay 61169 ns | gPTP rate +0.000 ppm residual 0 ns
--- talker.log
talker: 2 PROBE_TX answered, 38432 AAF and 9608 CRF PDUs sent
compat: PASS
```

### Leniencies off: unbound, then `streams`

```text
tests/milan-compat/build.sh $VALIDATION_STORAGE/pb2aecp-a830/lavdecc/src $VALIDATION_STORAGE/pb2aecp-a830/lavdecc/build-strict $VALIDATION_STORAGE/pb2aecp-a830/check-r3/milan-compat
tests/milan-compat/run-netns-compat.sh $VALIDATION_STORAGE/pb2aecp-a830/check-r3 $VALIDATION_STORAGE/pb2aecp-a830/check-r3/milan-compat config/entity.conf 15
--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---
la_avdecc 4.3.1-beta1, controller library 4.3.1-beta1, compiled with: ARBAP RDNCY JSN RDNCY RDNCY2018 JSN CBR; on veth1
online: entity 021a2bfffe3c4d5e model 001bc50ac1000100, compatibility IEEE17221|Milan, Milan 1.2.0.0, subscribed to unsolicited notifications, GET_DYNAMIC_INFO not used
settling 15 s
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=4
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=6
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=8
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=10
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=12
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=14
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=16
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=18
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=20
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=22
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=24
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=26
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=28
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=30
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=32
final: entity 021a2bfffe3c4d5e, compatibility IEEE17221|Milan, Milan 1.2.0.0, 0 compatibility events, 0 warnings/errors logged, 0 query errors, fatal enumeration error no
model STREAM_INPUT 0 'Stream In 0' state 0 talker ffffffffffffffff/65535, format 0205022002006000, counters 1=0 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=0
model STREAM_INPUT 1 'CRF' state 0 talker ffffffffffffffff/65535, format 041060010000bb80, counters 1=0 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=0
model STREAM_OUTPUT 0 format 0205022002006000, counters (Milan 1.2 layout) 1=1 2=0 4=0 8=0 16=32
model CLOCK_DOMAIN 0 source 0, counters 1=1 2=0
model AVB_INTERFACE 0 clock identity 021a2bfffe3c4d5e, gPTP grandmaster 3cc0c6fffe0a0001, AS path 2 entries, counters 1=1 2=0 32=1
verdict: PASS - Milan 1.2.0.0 compatible, zero compatibility events, warnings or errors
--- milan-dp ---
link_up=1
gm_id=3cc0c6fffe0a0001
gm_domain=0
maap_valid=1
maap_base=91e0f00000b4
maap_count=1
source0=stream_id:021a2b3c4d5e0000 dest_mac:91e0f00000b4 vlan:2 dest_mac_valid:1
sink0=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0 bound:0 binds:0
sink1=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0 bound:0 binds:0
milan_stream_output0=valid:01f binds:0 1 0 0 0 33 0 0 0 0 0 0 0
milan_stream_input0=valid:f3f binds:0 0 0 0 0 0 0 0 0 0 0 0 0
milan_stream_input1=valid:f3f binds:0 0 0 0 0 0 0 0 0 0 0 0 0
--- ctrld.log
AEM image: 5784 bytes, 32 descriptors, generated CRC e937544f verified; this board's identity 021a2bfffe3c4d5e, CRC 5b73f169
saved state at boot (/tmp/tmp.GdnM7JdBbf/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
MAAP: no addresses
entity 021a2bfffe3c4d5e on veth0 (mac 021a2b3c4d5e, model 001bc50ac1000100, 1 sources, 2 sinks, no SRP domain): AECP open
link up, grandmaster 0000000000000000 domain 0
link up, grandmaster 3cc0c6fffe0a0001 domain 0
MAAP: 1 addresses from 91e0f00000b4
entity 021a2bfffe3c4d5e mac 021a2b3c4d5e | link up gm 3cc0c6fffe0a0001 asCapable 1 | loop passes 4233 events 1917 rx 54 bad 0 | fabric rx 58 admitted 45 tx 79 err 0 lost 0 filter_mismatch 0
AECP open, unlocked 0000000000000000 | face commits 1 late 0 unanswered 0 | far answers 957 silent 0 commits 1 edits refused 0 | media block read
AECP core: ready 1 response_owed 0 notify 0 owed 0 waiting 0 refusing 0 | busy drops 0 malformed 0 ignored 0
ingress rate drops: adp 0 acmp 0 aecp 4 maap 0 | rx drops: adp 0 acmp 0 aecp 0 maap 0
MAAP valid base 91e0f00000b4 count 1
saved state now (/tmp/tmp.GdnM7JdBbf/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
sink 0: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
sink 1: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
departed
--- mediad.log
veth0 has no PTP clock: gPTP time is CLOCK_TAI
waiting for milan-ctrld's datapath block /milan-dp-compat-1908498
talker on veth0, 8 channels, PTO 2000000 ns, level 48..192 frames, PHC -1
listener on veth0, 8 channels, USB IN pipeline 0 ns
CRF input (sink 1) on veth0: counted, not clocked from
crf idle stream 0000000000000000 | PDUs 0 seq 0 late 0 early 0 unsupported 0 tu 0 mr toggles 0 | locked 0 unlocked 0 interrupted 0 grid breaks 0 | rate +0.000 ppm
listener idle stream 0000000000000000 | FRAMES_RX 0 SEQ_NUM_MISMATCH 0 LATE 0 EARLY 0 UNSUPPORTED 0 MEDIA_LOCKED 0 MEDIA_UNLOCKED 0 jumps 0 STREAM_INTERRUPTED 0 tu 0 mr toggles 0 | underruns 0 pitch 1000000 | presentation error 0..0 avg 0 ns, least margin 0 ns
talker idle stream 021a2b3c4d5e0000 -> 91e0f00000b4 | PDUs 133929 underruns 0 overruns 0 late 117107 send errors 0 | level 46..52 avg 48.23 (target 48) pitch 1000000 | worst send delay 78729 ns | gPTP rate +0.000 ppm residual 0 ns
compat: PASS
tests/milan-compat/run-netns-compat.sh $VALIDATION_STORAGE/pb2aecp-a830/check-r3 $VALIDATION_STORAGE/pb2aecp-a830/check-r3/milan-compat config/entity.conf 15 streams
--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---
la_avdecc 4.3.1-beta1, controller library 4.3.1-beta1, compiled with: ARBAP RDNCY JSN RDNCY RDNCY2018 JSN CBR; on veth1
online: entity 021a2bfffe3c4d5e model 001bc50ac1000100, compatibility IEEE17221|Milan, Milan 1.2.0.0, subscribed to unsolicited notifications, GET_DYNAMIC_INFO not used
settling 15 s
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=9
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=8
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=10
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=11
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=10
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=12
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=12
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=12
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=14
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=14
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=14
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=16
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=16
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=16
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=18
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=18
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=18
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=20
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=20
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=20
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=22
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=22
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=22
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=24
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=24
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=24
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=26
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=26
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=26
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=28
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=28
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=28
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=30
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=30
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=30
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=32
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=32
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=32
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=34
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=34
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=34
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=36
counters STREAM_INPUT 0 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
counters STREAM_INPUT 1 (valid bit value=count): 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=38
final: entity 021a2bfffe3c4d5e, compatibility IEEE17221|Milan, Milan 1.2.0.0, 0 compatibility events, 0 warnings/errors logged, 0 query errors, fatal enumeration error no
model STREAM_INPUT 0 'Stream In 0' state 2 talker 020000fffe000002/0, format 0205022002006000, counters 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
model STREAM_INPUT 1 'CRF' state 2 talker 020000fffe000002/1, format 041060010000bb80, counters 1=1 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=36
model STREAM_OUTPUT 0 format 0205022002006000, counters (Milan 1.2 layout) 1=1 2=0 4=0 8=0 16=38
model CLOCK_DOMAIN 0 source 0, counters 1=1 2=0
model AVB_INTERFACE 0 clock identity 021a2bfffe3c4d5e, gPTP grandmaster 3cc0c6fffe0a0001, AS path 2 entries, counters 1=1 2=0 32=1
verdict: PASS - Milan 1.2.0.0 compatible, zero compatibility events, warnings or errors
--- milan-dp ---
link_up=1
gm_id=3cc0c6fffe0a0001
gm_domain=0
maap_valid=1
maap_base=91e0f00000b4
maap_count=1
source0=stream_id:021a2b3c4d5e0000 dest_mac:91e0f00000b4 vlan:2 dest_mac_valid:1
sink0=stream_id:0200000000020000 dest_mac:91e0f000ab00 vlan:2 listening:1 bound:1 binds:1
sink1=stream_id:0200000000020001 dest_mac:91e0f000ab01 vlan:2 listening:1 bound:1 binds:1
milan_stream_output0=valid:01f binds:0 1 0 0 0 39 0 0 0 0 0 0 0
milan_stream_input0=valid:f3f binds:1 1 0 0 0 0 0 0 0 0 0 0 38
milan_stream_input1=valid:f3f binds:1 1 0 0 0 0 0 0 0 0 0 0 37
--- ctrld.log
AEM image: 5784 bytes, 32 descriptors, generated CRC e937544f verified; this board's identity 021a2bfffe3c4d5e, CRC 5b73f169
saved state at boot (/tmp/tmp.NZmeVDi2Hj/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
MAAP: no addresses
entity 021a2bfffe3c4d5e on veth0 (mac 021a2b3c4d5e, model 001bc50ac1000100, 1 sources, 2 sinks, no SRP domain): AECP open
link up, grandmaster 0000000000000000 domain 0
link up, grandmaster 3cc0c6fffe0a0001 domain 0
MAAP: 1 addresses from 91e0f00000b4
sink 0: bound, talker 020000fffe000002/0, acmp status 0
sink 0: listening to stream 0200000000020000 at 91e0f000ab00, VLAN 2
sink 0: bound, talker 020000fffe000002/0, acmp status 0
sink 0: bound, talker 020000fffe000002/0, acmp status 0
sink 1: bound, talker 020000fffe000002/1, acmp status 0
sink 1: listening to stream 0200000000020001 at 91e0f000ab01, VLAN 2
sink 1: bound, talker 020000fffe000002/1, acmp status 0
sink 1: bound, talker 020000fffe000002/1, acmp status 0
entity 021a2bfffe3c4d5e mac 021a2b3c4d5e | link up gm 3cc0c6fffe0a0001 asCapable 1 | loop passes 4981 events 2297 rx 58 bad 0 | fabric rx 59 admitted 46 tx 113 err 0 lost 0 filter_mismatch 0
AECP open, unlocked 0000000000000000 | face commits 5 late 0 unanswered 0 | far answers 2334 silent 0 commits 5 edits refused 0 | media block read
AECP core: ready 1 response_owed 0 notify 0 owed 0 waiting 0 refusing 0 | busy drops 0 malformed 0 ignored 0
ingress rate drops: adp 0 acmp 0 aecp 1 maap 0 | rx drops: adp 0 acmp 0 aecp 0 maap 0
MAAP valid base 91e0f00000b4 count 1
saved state now (/tmp/tmp.NZmeVDi2Hj/journal.bin): slot A seq 1 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 1 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
sink 0: state 7 bound 1 talker 020000fffe000002/0 started 1 status 0
sink 1: state 7 bound 1 talker 020000fffe000002/1 started 1 status 0
departed
--- mediad.log
veth0 has no PTP clock: gPTP time is CLOCK_TAI
waiting for milan-ctrld's datapath block /milan-dp-compat-1909696
talker on veth0, 8 channels, PTO 50000000 ns, level 48..192 frames, PHC -1
listener on veth0, 8 channels, USB IN pipeline 0 ns
CRF input (sink 1) on veth0: counted, not clocked from
crf active, locked stream 0200000000020001 | PDUs 9385 seq 0 late 0 early 0 unsupported 0 tu 0 mr toggles 0 | locked 1 unlocked 0 interrupted 0 grid breaks 0 | rate +0.000 ppm
listener active, locked stream 0200000000020000 | FRAMES_RX 38620 SEQ_NUM_MISMATCH 0 LATE 0 EARLY 0 UNSUPPORTED 0 MEDIA_LOCKED 1 MEDIA_UNLOCKED 0 jumps 0 STREAM_INTERRUPTED 0 tu 0 mr toggles 0 | underruns 0 pitch 999996 | presentation error -13172..36338 avg -5705 ns, least margin 49859495 ns
talker idle stream 021a2b3c4d5e0000 -> 91e0f00000b4 | PDUs 157785 underruns 0 overruns 0 late 137733 send errors 0 | level 45..50 avg 48.52 (target 48) pitch 1000000 | worst send delay 60082 ns | gPTP rate +0.000 ppm residual 0 ns
--- talker.log
talker: 2 PROBE_TX answered, 38839 AAF and 9710 CRF PDUs sent
compat: PASS
```

### Negative control: the build at f405a57 (library defaults)

```text
--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---
la_avdecc 4.3.1-beta1, controller library 4.3.1-beta1, compiled with: IICDL IINSAR AGAMU ARBAP RDNCY JSN INSNDM CMAR RDNCY RDNCY2018 JSN CBR; on veth1
log COMPAT: [0x021a2bfffe3c4d5e] [{}] {}
log COMPAT: [0x021a2bfffe3c4d5e] Entity not fully Milan compliant
online: entity 021a2bfffe3c4d5e model 001bc50ac1000100, compatibility IEEE17221, Milan 0.0.0.0, subscribed to unsolicited notifications, GET_DYNAMIC_INFO not used
compatibility event: [Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor (IEEE17221|Milan -> IEEE17221, Milan 1.2.0.0 -> 0.0.0.0)
settling 10 s
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=17600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=25600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=33600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=41600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=49600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=57600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=65600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=73600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=81600
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 16=89600
final: entity 021a2bfffe3c4d5e, compatibility IEEE17221, Milan 0.0.0.0, 1 compatibility events, 2 warnings/errors logged, 0 query errors, fatal enumeration error no
final event: [Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor
model STREAM_INPUT 0 'Stream In 0' state 0 talker ffffffffffffffff/65535, format 0205022002006000, counters 1=0 2=0 4=0 8=0 16=0 256=0 512=0 1024=0 2048=0
model STREAM_INPUT 1 'CRF' state 0 talker ffffffffffffffff/65535, format 041060010000bb80, counters
model STREAM_OUTPUT 0 format 0205022002006000, counters (Milan 1.2 layout) 1=1 2=0 16=89600
model CLOCK_DOMAIN 0 source 0, counters 1=1 2=0
model AVB_INTERFACE 0 clock identity 021a2bfffe3c4d5e, gPTP grandmaster 3cc0c6fffe0a0001, AS path 2 entries, counters 1=1 2=0 32=1
verdict: FAIL
--- milan-dp ---
link_up=1
gm_id=3cc0c6fffe0a0001
gm_domain=0
maap_valid=1
maap_base=91e0f00000b4
maap_count=1
source0=stream_id:021a2b3c4d5e0000 dest_mac:91e0f00000b4 vlan:2 dest_mac_valid:1
sink0=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0
sink1=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0
--- ctrld.log
AEM image: 5784 bytes, 32 descriptors, generated CRC e937544f verified; this board's identity 021a2bfffe3c4d5e, CRC 5b73f169
saved state at boot (/tmp/tmp.Tlg3ggKyiQ/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
MAAP: no addresses
entity 021a2bfffe3c4d5e on veth0 (mac 021a2b3c4d5e, model 001bc50ac1000100, 1 sources, 2 sinks, no SRP domain): AECP open
link up, grandmaster 3cc0c6fffe0a0001 domain 0
MAAP: 1 addresses from 91e0f00000b4
entity 021a2bfffe3c4d5e mac 021a2b3c4d5e | link up gm 3cc0c6fffe0a0001 asCapable 1 | loop passes 3111 events 1385 rx 54 bad 0 | fabric rx 54 admitted 54 tx 72 err 0 lost 0 filter_mismatch 0
AECP open, unlocked 0000000000000000 | face commits 1 late 0 unanswered 0 | far answers 756 silent 0 commits 1 edits refused 0 | media block read
AECP core: ready 1 response_owed 0 notify 0 owed 0 waiting 0 refusing 0 | busy drops 0 malformed 0 ignored 0
ingress rate drops: adp 0 acmp 0 aecp 0 maap 0 | rx drops: adp 0 acmp 0 aecp 0 maap 0
MAAP valid base 91e0f00000b4 count 1
saved state now (/tmp/tmp.Tlg3ggKyiQ/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
sink 0: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
sink 1: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
departed
--- mediad.log
veth0 has no PTP clock: gPTP time is CLOCK_TAI
waiting for milan-ctrld's datapath block /milan-dp-compat-1867413
talker on veth0, 8 channels, PTO 2000000 ns, level 48..192 frames, PHC -1
listener on veth0, 8 channels, USB IN pipeline 0 ns
listener idle stream 0000000000000000 | FRAMES_RX 0 SEQ_NUM_MISMATCH 0 LATE 0 EARLY 0 UNSUPPORTED 0 MEDIA_LOCKED 0 MEDIA_UNLOCKED 0 MEDIA_RESET 0 STREAM_INTERRUPTED 0 | underruns 0 pitch 1000000 | presentation error 0..0 avg 0 ns, least margin 0 ns
talker idle stream 021a2b3c4d5e0000 -> 91e0f00000b4 | PDUs 92399 underruns 0 overruns 0 late 80628 send errors 0 | level 42..48 avg 48.33 (target 48) pitch 1000000 | worst send delay 242064 ns | gPTP rate +0.000 ppm residual 0 ns
compat: FAIL
```

### Negative control: milan-mediad without its CRF receive path (`MEDIAD_ROLES=talker,listener`)

```text
--- milan-compat on veth1, for entity 021a2bfffe3c4d5e ---
la_avdecc 4.3.1-beta1, controller library 4.3.1-beta1, compiled with: IICDL IINSAR AGAMU ARBAP RDNCY JSN INSNDM CMAR RDNCY RDNCY2018 JSN CBR; on veth1
log COMPAT: [0x021a2bfffe3c4d5e] [{}] {}
log COMPAT: [0x021a2bfffe3c4d5e] Entity not fully Milan compliant
online: entity 021a2bfffe3c4d5e model 001bc50ac1000100, compatibility IEEE17221, Milan 0.0.0.0, subscribed to unsolicited notifications, GET_DYNAMIC_INFO not used
compatibility event: [Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor (IEEE17221|Milan -> IEEE17221, Milan 1.2.0.0 -> 0.0.0.0)
settling 5 s
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=4
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=6
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=8
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=10
counters STREAM_OUTPUT 0 (Milan 1.2 layout, valid bit value=count): 1=1 2=0 4=0 8=0 16=12
final: entity 021a2bfffe3c4d5e, compatibility IEEE17221, Milan 0.0.0.0, 1 compatibility events, 2 warnings/errors logged, 0 query errors, fatal enumeration error no
final event: [Milan 1.3 - 5.3.8.10] Milan mandatory counters missing for STREAM_INPUT descriptor
model STREAM_INPUT 0 'Stream In 0' state 0 talker ffffffffffffffff/65535, format 0205022002006000, counters 1=0 2=0 4=0 8=0 16=0 32=0 256=0 512=0 1024=0 2048=0
model STREAM_INPUT 1 'CRF' state 0 talker ffffffffffffffff/65535, format 041060010000bb80, counters
model STREAM_OUTPUT 0 format 0205022002006000, counters (Milan 1.2 layout) 1=1 2=0 4=0 8=0 16=12
model CLOCK_DOMAIN 0 source 0, counters 1=1 2=0
model AVB_INTERFACE 0 clock identity 021a2bfffe3c4d5e, gPTP grandmaster 3cc0c6fffe0a0001, AS path 2 entries, counters 1=1 2=0 32=1
verdict: FAIL
--- milan-dp ---
link_up=1
gm_id=3cc0c6fffe0a0001
gm_domain=0
maap_valid=1
maap_base=91e0f00000b4
maap_count=1
source0=stream_id:021a2b3c4d5e0000 dest_mac:91e0f00000b4 vlan:2 dest_mac_valid:1
sink0=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0 bound:0 binds:0
sink1=stream_id:0000000000000000 dest_mac:000000000000 vlan:0 listening:0 bound:0 binds:0
milan_stream_output0=valid:01f binds:0 1 0 0 0 13 0 0 0 0 0 0 0
milan_stream_input0=valid:f3f binds:0 0 0 0 0 0 0 0 0 0 0 0 0
milan_stream_input1=valid:000 binds:0 0 0 0 0 0 0 0 0 0 0 0 0
--- ctrld.log
AEM image: 5784 bytes, 32 descriptors, generated CRC e937544f verified; this board's identity 021a2bfffe3c4d5e, CRC 5b73f169
saved state at boot (/tmp/tmp.kRIqZ5ih3O/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
MAAP: no addresses
entity 021a2bfffe3c4d5e on veth0 (mac 021a2b3c4d5e, model 001bc50ac1000100, 1 sources, 2 sinks, no SRP domain): AECP open
link up, grandmaster 0000000000000000 domain 0
link up, grandmaster 3cc0c6fffe0a0001 domain 0
MAAP: 1 addresses from 91e0f00000b4
entity 021a2bfffe3c4d5e mac 021a2b3c4d5e | link up gm 3cc0c6fffe0a0001 asCapable 1 | loop passes 2055 events 875 rx 54 bad 0 | fabric rx 54 admitted 54 tx 67 err 0 lost 0 filter_mismatch 0
AECP open, unlocked 0000000000000000 | face commits 1 late 0 unanswered 0 | far answers 594 silent 0 commits 1 edits refused 0 | media block read
AECP core: ready 1 response_owed 0 notify 0 owed 0 waiting 0 refusing 0 | busy drops 0 malformed 0 ignored 0
ingress rate drops: adp 0 acmp 0 aecp 0 maap 0 | rx drops: adp 0 acmp 0 aecp 0 maap 0
MAAP valid base 91e0f00000b4 count 1
saved state now (/tmp/tmp.kRIqZ5ih3O/journal.bin): slot none seq 0 (A 0, B 0) | bindings blank, rest blank | phase 1 dirty 0 pending 0 | commits 0 ok, 0 failed, 0 skipped | unread 0x0 read faults 0 legacy 0x0
sink 0: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
sink 1: state 0 bound 0 talker 0000000000000000/0 started 0 status 0
departed
--- mediad.log
veth0 has no PTP clock: gPTP time is CLOCK_TAI
waiting for milan-ctrld's datapath block /milan-dp-compat-1870162
talker on veth0, 8 channels, PTO 2000000 ns, level 48..192 frames, PHC -1
listener on veth0, 8 channels, USB IN pipeline 0 ns
listener idle stream 0000000000000000 | FRAMES_RX 0 SEQ_NUM_MISMATCH 0 LATE 0 EARLY 0 UNSUPPORTED 0 MEDIA_LOCKED 0 MEDIA_UNLOCKED 0 jumps 0 STREAM_INTERRUPTED 0 tu 0 mr toggles 0 | underruns 0 pitch 1000000 | presentation error 0..0 avg 0 ns, least margin 0 ns
talker idle stream 021a2b3c4d5e0000 -> 91e0f00000b4 | PDUs 52458 underruns 0 overruns 0 late 45799 send errors 0 | level 45..48 avg 46.27 (target 48) pitch 1000000 | worst send delay 93848 ns | gPTP rate +0.000 ppm residual 0 ns
compat: FAIL
```

## Logs

| File | What |
| --- | --- |
| logs/make-check.log | `make check` at the head |
| logs/compat-*.log | the compatibility runs above |
| logs/r901-probe-*.log | R901's probes, and the mutant's build |
| logs/cross-build-1.log, -2.log | the two aarch64 builds |
