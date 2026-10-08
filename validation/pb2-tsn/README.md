# PB2 TSN validation kit

The checks every ticket of the PocketBeagle 2 Ethernet Cap TSN work closes on
(issue #1). Each script prints the numbers it judged and exits **0** on pass,
**1** on fail and **2** on a usage error, so a check is one command and its
verdict is the exit code.

| Script | Runs on | Checks it serves |
|---|---|---|
| `check-rt.sh` | PB2 (BusyBox `sh`) | F1.2 |
| `check-gptp.sh` | PB2 (BusyBox `sh`) | F2.2, F2.3, F2.5, F2.6 |
| `check-shaper.sh` | PB2 (BusyBox `sh`) | F3.1, F3.4 (drops) |
| `aaf-analyze.py` | tap host, I210 peer, or the PB2's own capture copied off | A3.1, A3.2, A3.6, F3.4, F4.3, B1.3 |
| `ramp-gen.py` | host that plays into the gadget | F5.2, A3.4, A4.2, A5.2, B2.1 |
| `ramp-check.py` | anywhere | F5.2, A3.4, A4.2, A5.2, B2.1 |
| `latency-peer.py` | I210 peer | A3.5, A3.6, B2.4, B3.1 |
| `selftest.sh` | any host with `python3` | V1.1, V1.2, V1.3 |

The Python scripts use the standard library only and are run as
`python3 -I script.py`; each puts its own directory on `sys.path` for the two
shared modules, `avtplib.py` (pcap/pcapng, Ethernet/VLAN, AVTP AAF, WAV) and
`ramplib.py` (the counting ramp).

---

## The bench

Roles, not machines. Evidence names these roles, never addresses or host names.

| Role | What it does |
|---|---|
| gPTP grandmaster | the time base every other role follows |
| AVB switch | an 802.1AS bridge with MSRP/MVRP; the PB2's link goes through it |
| Milan reference device | a Milan v1.2 talker and listener of known behaviour |
| I210 peer | a gPTP-synced host whose Intel I210 timestamps **every** received frame in hardware; the latency instrument |
| tap host | a passive tap on the PB2's link and the host that records it |
| controller | an ATDECC controller (binds streams, reads counters) |
| USB host | plays into / records from the PB2's UAC2 gadget with `aplay`/`arecord` |

The PB2 cannot be its own latency instrument: am65-cpsw timestamps only PTP
frames in hardware (it refuses `HWTSTAMP_FILTER_ALL`), so AVTP frame times are
taken on the I210 peer.

## Recording evidence

For every check, post on its ticket:

1. the check ID (`F2.3`, `A3.5`, ...);
2. the exact command;
3. its output, or the log it wrote, ending in the `RESULT:` line;
4. for a capture, its `sha256sum`, and where the file is kept.

Thresholds the tickets mark *(proposed)* are the scripts' defaults; a change the
owner makes is passed as an option, and the option goes into the evidence.

Copy this directory to the board for the three shell scripts (they need nothing
outside BusyBox, `cyclictest`, `linuxptp`, `tc` and `ethtool`, all in the image).

---

## check-rt.sh — scheduling latency (F1.2)

```sh
# start the USB audio load from the USB host first (aplay of an 8 ch ramp into the gadget)
./check-rt.sh --duration 1h --load --iperf-peer <peer> --max-us 100
./check-rt.sh --parse /tmp/check-rt-<date>.txt            # judge a saved run again
```

`cyclictest -m -S -p 95 -i 200 -h 400 -q`, one thread per CPU; `--load` adds
`stress-ng --cpu 3 --io 1 --vm 1 --vm-bytes 64M`, `--iperf-peer` an
`iperf3 --bidir` client. **Pass:** every CPU's maximum <= `--max-us` (100 us
proposed). The raw output is kept (`--out`, default `/tmp/check-rt-<date>.txt`)
and its histogram is the evidence.

## milan-gptp-watch — the offset from the wire, whatever the gPTP daemon

`check-gptp.sh` asks ptp4l through `pmc`. `milan-gptp-watch` (installed with the
bridge, built from `milan-linux/src/gptp_watch.c`) asks no daemon. It takes
the board's own hardware receive time of each Sync from the grandmaster's side,
and the time the matching Follow_Up carries, so it judges any gPTP stack the
same way:

```sh
milan-gptp-watch -i eth0 -d 600 -w 30 -l 203 -g <gm clock identity>   # F2.3 over 10 min
```

`-l` is the link delay to the switch (ptp4l measured 203 ns on the bench; the
watcher cannot see another process's Pdelay_Req transmit time). **Pass** when
at least 90 % of the expected Syncs were judged, all with a hardware timestamp,
|offset| <= `-t` (100 ns) for >= 99.9 % of them, and the grandmaster never
changed (and is `-g`). On the bench, against ptp4l over 60 s: mean 0 ns,
p99 8 ns, 480 of 480 Syncs; ptp4l's own report was rms 1–3 ns.

## check-gptp.sh — gPTP state and offsets (F2.2, F2.3, F2.5, F2.6)

```sh
./check-gptp.sh --expect-gm <gm clock identity>                  # F2.2: state only
./check-gptp.sh --expect-gm <gm> --window 1800                   # F2.3: sample TIME_STATUS_NP for 30 min
./check-gptp.sh --log /var/log/ptp4l.log                         # F2.3: judge a ptp4l -m log instead
./check-gptp.sh --role master                                    # F2.6: the PB2 as grandmaster
```

State through `pmc -u -b 0 -t 1` on `/var/run/ptp4lro` (`--uds`): **pass** when
`portState` is SLAVE (MASTER with `--role master`), `asCapable` 1, `gmPresent`
true and the GM identity is `--expect-gm`. Offsets are judged only after the
first lock: **pass** when |master offset| <= `--offset-ns` (100) for >=
`--fraction` (99.9 %) of samples, no sample beyond `--excursion-ns` (1000), the
path delay stays within +/-`--delay-spread-ns` (50) of its centre, and the port
state never changes. A log with only `rms ... max ...` summaries is judged on
each summary's max.

## check-shaper.sh — the CPSW egress shaper (F3.1, F3.4)

```sh
# start one generator per priority under test (skb priority 0, 2, 3), then
./check-shaper.sh --dev eth0 --expect-tcs 3 --expect-moving "0 1 2" --interval 10
```

**Pass:** the root qdisc is `mqprio`, `offloaded`, with the expected number of
traffic classes, `mode:channel` and `shaper:bw_rlimit`; `p0-rx-ptype-rrobin` is
`off` (with it on, every host packet lands in port FIFO 0 and the shaper never
sees class A or B); and over `--interval` the port's `tx_priN` counters move for
every expected priority (mqprio maps traffic class N to port priority N).
`--no-drops` also fails on any `tx_priN_drop` increment.

## aaf-analyze.py — the AAF streams of a capture (A3.1, A3.2, F3.4, F4.3)

```sh
python3 -I aaf-analyze.py cap.pcap --stream <id> \
    --expect-rate 8000 --expect-frames 28800000 --expect-step-ns 125000 --step-tol-ns 1 \
    --expect-format INT_32BIT --expect-channels 8 --expect-nsr 48000 --expect-spf 6 \
    --expect-bit-depth 32 --expect-vid 2 --expect-pcp 3 --wav payload.wav
```

Reads pcap or pcapng (ns or us timestamps, VLAN tags kept). Per stream ID:
frames and rate (on the capture clock, and on AVTP time), sequence gaps and
duplicates, frames with `tv=0`, the AVTP timestamp step (min/max/mean/stdev and
how many fall outside `--expect-step-ns` +/- `--step-tol-ns`), the arrival
interval and its |deviation| p99/p99.9, and the AAF fields (format, nsr,
channels_per_frame, bit_depth, stream_data_length and the samples per frame they
imply). **Pass:** no sequence gap or duplicate, `tv=1` on every frame, and every
`--expect-*` met. The capture-clock rate tolerance is `--rate-tol-ppm` (100) since
a tap's clock is not gPTP; `--expect-frames` (exact count, +/-1) is the 1-hour
form of A3.1. `--wav` writes the payload of one stream as a little-endian PCM WAV
(nothing is filled in for lost frames, so `ramp-check.py` sees them).

## ramp-gen.py / ramp-check.py — bit-exact audio (F5.2, A3.4, A4.2, A5.2)

```sh
python3 -I ramp-gen.py ramp.wav --channels 8 --rate 48000 --seconds 600
aplay -D hw:UAC2Gadget,0 ramp.wav                                     # USB host, into the PB2
python3 -I aaf-analyze.py cap.pcap --stream <id> --wav payload.wav    # off the wire
python3 -I ramp-check.py payload.wav --min-seconds 590
```

Sample *(n, c)* is `((n mod N) * C + c) << shift`, so every channel carries its
own index and every frame its counter. `ramp-check.py` skips leading silence or
garbage until it finds the ramp, then requires every frame to carry the next
counter on every channel. It reports dropped and repeated frames, bit errors per
channel, silent frames inside the ramp, and the first discontinuity (the file
frame index and the ramp counter). Trailing silence is the player stopping and is
not counted. **Pass:** no drop, no repeat, no bit error, no silence inside the
ramp (`--allow-silence` relaxes the last), and at least `--min-seconds` checked.
For a path that is 24-bit inside, generate and check with `--shift 8`.

## latency-peer.py — talker latency (A3.5, A3.6)

```sh
# on the I210 peer, gPTP-synced, hardware timestamps on the PHC time scale:
tcpdump -i <nic> -j adapter_unsynced --time-stamp-precision=nano -w cap.pcap
python3 -I latency-peer.py cap.pcap --pto-ns 2000000 --max-us 2000 [--transit-ns N] [--csv per-frame.csv]
```

Per AAF frame with `tv=1`: `latency = (rx_ts mod 2^32) - (avtp_timestamp - PTO)`,
wrap handled, minus `--transit-ns`. Because the AVTP timestamp is the sample's
ingress time plus the PTO, this is the bridge latency plus wire and switch
transit; measure the transit back to back and pass it as `--transit-ns`, or report
it beside. **Pass:** max <= `--max-us` (2000, the epic's target) and no negative
latency. A negative latency means the peer is not on the talker's gPTP time (or
the PTO is wrong), not an early frame. Percentiles are resolved to 100 ns; a
microsecond capture is good to 1 us.

---

## Self-test (V1.1, V1.2, V1.3)

```sh
validation/pb2-tsn/selftest.sh                 # about 15 s and ~1 GB of temporary space for V1.3
validation/pb2-tsn/selftest.sh --skip-v13
BUSYBOX=/path/to/busybox validation/pb2-tsn/selftest.sh   # the board scripts under BusyBox ash/awk
```

- **V1.1** every script gives the expected verdict on the committed samples,
  including at least one failing sample per script, and `ramp-gen.py` reproduces
  `samples/ramp-pass.wav` byte for byte.
- **V1.2** `latency-peer.py` reports known injected latencies (150 us to 3.1 ms,
  one negative, across a 32-bit wrap of the AVTP time): exactly from a
  nanosecond capture, within 1 us from a microsecond one.
- **V1.3** in a 10-minute 8-channel ramp, `ramp-check.py` finds one dropped
  frame, one repeated frame and one flipped bit (channel 5, bit 17), each at its
  file frame index and ramp counter, and nothing else.

## Samples

`samples/make-samples.py` regenerates every fixture (fixed seeds, so the output
is identical each time). They are synthetic: the text fixtures follow the output
formats of `cyclictest`, `ptp4l`, `pmc`, `tc` and `ethtool`, and the captures are
built frame by frame. When the bench produces real outputs, add them next to
these and extend the self-test to cover them.

| Fixture | Judged by | Verdict |
|---|---|---|
| `cyclictest-pass.txt` / `-fail.txt` / `-truncated.txt` | `check-rt.sh` | pass / one CPU at 137 us / no summary |
| `pmc-slave.txt` / `pmc-listening.txt` | `check-gptp.sh` | pass (and fail against a wrong GM) / fail |
| `ptp4l-pass.log` / `-fail.log` / `-summary.log` | `check-gptp.sh --log` | pass / a 1.5 us excursion and a state change / pass on summaries |
| `shaper-pass/`, `shaper-fail-{rrobin,nooffload,notmoving}/` | `check-shaper.sh --files` | pass / fail each |
| `aaf-pass.pcap` | `aaf-analyze.py` (+ `ramp-check.py` on its payload) | pass |
| `aaf-fail.pcap` | `aaf-analyze.py` (+ `ramp-check.py` on its payload) | a lost frame, two `tv=0`, two bad steps |
| `ramp-pass.wav` / `ramp-fail.wav` | `ramp-check.py` | pass / one dropped frame at counter 1000 |
| `latency-pass.pcap` / `latency-fail.pcap` | `latency-peer.py` | pass / one 2.5 ms frame |
