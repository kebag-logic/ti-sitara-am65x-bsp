# PocketBeagle 2 Ethernet Cap: TSN foundation and the USB-to-Milan bridge

This file turns the PocketBeagle 2 with the Kebag-Logic Ethernet Cap rev B
([README.pb2-ethcap.md](README.pb2-ethcap.md)) into a TSN end station. The goal
is a USB-to-Milan audio bridge:

```
host aplay   ->  UAC2 gadget (PB2)  ->  AAF stream on eth0  ->  any Milan listener
host arecord <-  UAC2 gadget (PB2)  <-  AAF stream on eth0  <-  any Milan talker
```

The target is a bridge latency under 2 ms, measured from USB ingress at the
PB2 to the AAF frame leaving `eth0`, without the presentation time offset. The
work is tracked in [issue #1](https://github.com/kebag-logic/ti-sitara-am65x-bsp/issues/1).
This file covers what exists so far: the TSN foundation (#2 to #7), and on
top of it path A, the native Milan bridge (§3).

> **Status:** brought up on a rev B board on the bench (§5). The
> foundation runs at the first boot, and the control plane answers the real
> Milan entities. Streams through the AVB switch wait for SRP (#14). Each
> ticket closes only on the evidence its *Validation* section names.

---

## 0. The commands, in order

From the BSP root. Steps 1–3 of `README.tdm8-pb2.md` §0 (sources,
`./build.sh PB2`, `bootloader`) come first, once.

```sh
# kernel (PREEMPT_RT) + the four device trees, "ethcap" the default label
PB2_DEFAULT_LABEL=ethcap ./build-tdm8-uac2-pb2.sh image

# rootfs: the AVB service, the gPTP config, the shaper, the gadget profile
make -C ../buildroot O=$PWD/../buildroot-pb2 \
     BR2_EXTERNAL=$PWD/br2-external bb_pocketbeagle2_avb_defconfig
make -C ../buildroot O=$PWD/../buildroot-pb2 BR2_JLEVEL=32

./build-tdm8-uac2-pb2.sh sdimage          # -> res/spare-sd-pb2/pocketbeagle2-ethcap.img
```

After the boot, `/etc/init.d/S95avb status` reports each layer below.

---

## 1. What the foundation adds

| Layer | Ticket | Where | What |
|---|---|---|---|
| Real-time kernel | #3 | `res/kl-pb2-am62.config` | `PREEMPT_RT`, `HZ_1000`, `NO_HZ_FULL`, `RCU_NOCB_CPU`, performance governor |
| CPU isolation | #3 | `build-tdm8-uac2-pb2.sh` (`PB2_ETHCAP_APPEND`) | the `ethcap` label only: `isolcpus=nohz,domain,managed_irq,3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2` |
| IRQ placement | #3 | `/usr/sbin/avb-irq.sh` | CPSW and USB interrupts on `AVB_IRQ_CPU` (2) |
| gPTP | #4, #19 | `/etc/avb/flexptpd.conf`, `/usr/sbin/avb-gptp.sh` | `flexptpd` (802.1AS end station) on the CPTS clock of `eth0`, its status in `/dev/shm/flexptpd.eth0` |
| Egress | #5 | `/usr/sbin/avb-shaper.sh` | `eth0.2`, priority-to-PCP map, am65-cpsw class A/B shaper |
| Service | #6 | `/etc/init.d/S95avb`, `/etc/avb/avb.env` | all of the above in order, then the stack named by `AVB_STACK` |
| USB gadget | #7 | `/etc/avb/uac2-milan.env`, `S99usb_gadgets` | the bridge's UAC2 + ECM gadget, 125 us packets |
| Validation kit | #2 | `validation/pb2-tsn/` | the scripted checks every ticket is judged with |

Under the `tdm8`, `tdm8-async` and `notdm8` labels there is no `eth0`.
`S95avb` logs one line and does nothing, and `S99usb_gadgets` builds the TDM8
gadget as before. The real-time kernel is the same for every label; only
`ethcap` isolates CPU 3.

### 1.1 Real-time kernel and CPU 3 (#3)

The bridge services USB every 125 us and sends 8000 AAF frames/s. The kernel
is `PREEMPT_RT`, which is in mainline since 6.12 but still sits behind
`CONFIG_EXPERT`. Its release is `7.1.0-tdm8-pb2-rt`, so it and its modules sit
beside an earlier `7.1.0-tdm8-pb2` (PREEMPT) kernel on a card updated in place. `build-tdm8-uac2-pb2.sh config` refuses a `.config` that lost
`PREEMPT_RT`, `NO_HZ_FULL`, `RCU_NOCB_CPU` or `HZ_1000`.

The `ethcap` label keeps CPU 3 for the media plane. CPU 3 runs no scheduler
tick and no RCU callbacks, takes no IRQ, and is left out of load balancing. The
default is in `PB2_ETHCAP_APPEND`; set it empty to boot the cap without
isolation:

```sh
PB2_ETHCAP_APPEND= PB2_DEFAULT_LABEL=ethcap ./build-tdm8-uac2-pb2.sh image
```

`avb-irq.sh` then pins the interrupts the bridge's traffic uses to
`AVB_IRQ_CPU`, so CPUs 0 and 1 serve everything else. `AVB_IRQ_MATCH` names
them: the CPSW at `8000000.ethernet`, and the USB controller, which registers
as `dwc3` and `xhci-hcd`. On `PREEMPT_RT` each handler is an `irq/<n>-<name>`
thread that follows its IRQ's affinity; the kernel refuses `sched_setaffinity`
on IRQ threads.

The CPSW's TX and RX channel interrupts (`8000000.ethernet-tx0/-tx1/-rx0`) come
through the K3 interrupt aggregator (MSI-INTA), which refuses an affinity. Their
threads keep CPUs 0–3 in their mask. On the bench they run on CPUs 0 and 1
only, because CPU 3 has a scheduling domain of its own. `avb-irq.sh status`
shows each IRQ and where its threads may run.

Mainline `PREEMPT_RT` has no `/sys/kernel/realtime` (that file came with the
out-of-tree patch set): `uname -v` reads `SMP PREEMPT_RT`.

### 1.2 gPTP (#4, #19)

gPTP is `flexptpd`: our fork of flexPTP,
[kebag-logic/flexPTP](https://github.com/kebag-logic/flexPTP) (branch `gptp`),
with an IEEE 802.1AS-2020 time-aware end station on top of flexPTP's IEEE 1588
clock. It covers:

- the peer delay mechanism, with neighborRateRatio and asCapable;
- the 802.1AS BMCA, with path trace and the announce and sync receipt timeouts;
- Sync taken only from the selected master;
- signaling;
- Avnu's reverse sync.

It replaced linuxptp (#19 has the evaluation that chose it), and the image
carries no `ptp4l` and no `phc2sys`.

`avb-gptp.sh start` runs it on the CPTS hardware clock of `eth0`, every thread
at SCHED_FIFO 53 (`AVB_GPTP_PRIO`):

```sh
flexptpd -i eth0 -c /etc/avb/flexptpd.conf -P 53 -q    # event log: /var/log/flexptpd.log
```

`/etc/avb/flexptpd.conf` holds flexPTP CLI commands, applied once the gPTP
preset is loaded:
- `priority1 248`, so a bench grandmaster wins;
- asCapable's `neighborPropDelayThresh` of 800 ns and 9 allowed lost responses;
- the event logs;
- reverse sync, commented out: it is for a tester linked directly to the PB2.

The CPTS delivers TX timestamps through a work item. flexptpd waits up to 20 ms
for one, matched to its message by content, and gives up on it without
stalling.

flexptpd steers the PHC onto the grandmaster and publishes the port's state in
`/dev/shm/flexptpd.eth0`. `milan-ctrld` reads it for ADP's grandmaster, and so
does `avb-gptp.sh status`, through `milan-dp -g`:

```sh
milan-dp -g flexptpd.eth0     # port_state, as_capable, gm_identity, mean_link_delay_ns, time_error_ns, ...
```

Nothing steers the system clocks. The media plane models the PHC against
`CLOCK_MONOTONIC_RAW` itself (milan-linux `src/gptp_time.c`).

`check-gptp.sh` judges the state from that block and the offsets from the wire
(`milan-gptp-watch`), so neither depends on the daemon's own word.

`post-build.sh` still removes linuxptp's `S65ptp4l` and `S66phc2sys`, should a
configuration bring the package back. Their stock setup (UDPv4, end-to-end) is
not gPTP, and they would start before the shaper takes `eth0` down (§1.3).

### 1.3 Egress: VLAN, priority map and shaper (#5)

am65-cpsw has no CBS qdisc offload. It shapes in hardware through `mqprio` in
channel mode with `shaper bw_rlimit`, in whole Mbit/s. Two things must hold
before any traffic class is shaped, and both change only while every CPSW port
is down:

* **`p0-rx-ptype-rrobin` off.** With it on (the driver's default), the host
  port serves its TX channels round robin and sends all host traffic to port
  FIFO 0 (`am65_cpsw_nuss_set_p0_ptype()`).
* **TX channels = traffic classes** (`ethtool -L eth0 tx N`, which returns
  `EBUSY` while the port is up).

So `avb-shaper.sh start` takes `eth0` down and up once when either is wrong,
and only then. Then it creates `eth0.2` with `egress-qos-map 3:3 2:2` and
installs the qdisc. With one class A stream of 8 channels:

```
tc qdisc replace dev eth0 root handle 100: mqprio num_tc 2 \
   map 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 queues 1@0 1@1 \
   hw 1 mode channel shaper bw_rlimit min_rate 0 17mbit max_rate 0 17mbit
```

The class A reservation comes from `avb.env`. Each frame takes 66 bytes of
overhead on the wire: preamble and SFD 8, tagged header 18, AAF header 24, FCS
4, gap 12. Add the payload of 6 samples x 8 channels x 4 bytes = 192, and one
frame is 258 bytes. At 8000 frames/s that is 16.512 Mbit/s, rounded up to the
shaper's step: 17 Mbit/s. Setting `AVB_CLASS_B_MBIT` adds a third traffic class
for priority 2. `AVB_CLASS_A_MBIT` overrides the computation.

| skb priority (`SO_PRIORITY`) | Traffic class | On the wire |
|---|---|---|
| 3 (`AVB_CLASS_A_PRIO`) | TC1 (TC2 with class B), shaped | VLAN 2, PCP 3 |
| 2 (`AVB_CLASS_B_PRIO`) | TC1, only with class B | VLAN 2, PCP 2 |
| anything else | TC0, unshaped | untagged: ssh, iperf3, ... |

`eth0` is also the management link. Run `avb-shaper.sh start` (or `S95avb
restart`) from `usb0` or the serial console, or expect an ssh session over
`eth0` to stall for the few seconds autonegotiation takes.

`taprio` with `flags 0x2` (EST) is the alternative the hardware offers. It is
not used: AVB class A/B is a rate reservation, not a time schedule. There is no
ETF/launch-time offload on this MAC.

### 1.4 The service and stream multicast (#6)

`/etc/init.d/S95avb` runs after the network and sshd, and before
`S99usb_gadgets`, in this order:

1. `avb-irq.sh`;
2. `avb-shaper.sh start` (which may bounce `eth0`);
3. `ip link set eth0 allmulticast on`;
4. `avb-gptp.sh start`;
5. the stack: `AVB_STACK=native` runs `/usr/sbin/milan-bridge.sh` once path A
   installs it, `pipewire` is path B (not wired yet), and `none` stops here.

On multicast: the ALE VLAN entry that am65-cpsw creates for `eth0.2` floods
unregistered multicast to no port, so stream frames never reach the host
(`am65_cpsw_nuss_ndo_slave_add_vid()`: `unreg_mcast` is 0 for a non-zero VID).
Registered groups (`ip maddr`, `PACKET_ADD_MEMBERSHIP`) are written to the ALE
without a VLAN. `allmulticast` puts the host in every VLAN's unregistered flood,
which is what made streams arrive on the MYIR AM62x. Check F4.3 settles whether
the registered groups alone are enough. `AVB_ALLMULTI=no` turns it off.

`S95avb status` prints every layer; `S95avb stop` undoes them, in reverse.

### 1.5 The bridge's USB gadget (#7)

`S99usb_gadgets` (shared by every board) builds the gadget from the profile that
`AVB_GADGET_ENV` names, whenever `/etc/avb/avb.env` sets it and
`AVB_INTERFACE` exists. On the PB2 that means under `ethcap`. The profile,
`/etc/avb/uac2-milan.env`, is a `tdm8.env`-shaped file passed to
`tdm8-uac2.sh gadget-up`. No alsaloop bridge starts, because the media plane
owns the PCMs:

| Setting | Value | Why |
|---|---|---|
| rate, channels, format | 48 kHz, 8, `S32_LE` | the Milan AAF base format |
| `TDM8_HS_BINT` | 1 | one isochronous packet per 125 us microframe. `f_uac2` picks bInterval 3 (500 us) for 8 x 32-bit on its own |
| `TDM8_REQ_NUMBER` | 4 | requests queued per direction: 500 us of the board-to-host path. Host-to-board data completes packet by packet |
| `c_sync` | async | the host follows the feedback endpoint, which the media clock servo steers through `Capture Pitch 1000000` |
| ECM | yes | `usb0` stays the second way in, at 192.168.8.12/24 (host 192.168.8.1), MACs `…:30`/`…:32`: a TDM8 PocketBeagle 2 on the same host keeps 192.168.7.12, so the two never collide |

`tdm8-uac2.sh` gained `TDM8_HS_BINT`, `TDM8_PRODUCT`, `TDM8_FUNCTION_NAME` and
`TDM8_CONFIG_NAME`, which default to what it wrote before, so the TDM8 gadgets
of every board are unchanged. On the host the bridge shows up as
"PocketBeagle 2 USB-Milan bridge":

```sh
aplay -l | grep -i milan
aplay -D hw:<card>,0 -c 8 -f S32_LE -r 48000 --period-size=48 --buffer-size=192 test.wav
```

---

## 2. Validation

Every check has an ID (`F2.3` is check 3 of ticket F2, #4), and its evidence goes
on that ticket. The scripts are in [`validation/pb2-tsn/`](validation/pb2-tsn/README.md).
Evidence names the bench roles: gPTP grandmaster, AVB switch (802.1AS bridge),
Milan reference device, gPTP-synced peer with an Intel I210, passive tap on the
PB2 link, ATDECC controller.

| Ticket | Checks | Run on the board |
|---|---|---|
| #3 F1 | F1.1 `uname -v` shows `PREEMPT_RT` (mainline has no `/sys/kernel/realtime`); F1.2 `check-rt.sh` for 1 h under load, max <= 100 us; F1.3 README.pb2-ethcap.md §5; F1.4 30 min TDM8 bridge on the RT kernel | `uname -v; cat /proc/cmdline` |
| #4 F2 | F2.1 `ethtool -T`; F2.2 SLAVE within 10 s; F2.3 offset <= 100 ns at 99.9 % over 30 min; F2.4 the media time base within 1 us of the PHC (G6.2, #25, no phc2sys any more); F2.5 GM change; F2.6 PB2 as GM | `avb-gptp.sh status`, `check-gptp.sh` |
| #19 G | gPTP on our flexPTP fork: G1 Linux platform, G2 peer delay and asCapable, G3 BMCA and grandmaster, G4 signaling, G5 reverse sync, G6 the bridge without linuxptp, G7 conformance suite and bench runs (#20 to #26) | the fork's `tests/conformance/run.sh`, `check-gptp.sh`, `milan-gptp-watch` |
| #5 F3 | F3.1 offload accepted, per-queue counters; F3.2 tags on the tap; F3.3 rate cap; F3.4 isolation under `iperf3`; F3.5 ssh | `avb-shaper.sh status`, `check-shaper.sh` |
| #6 F4 | F4.1 cold boot; F4.2 `tdm8` label untouched; F4.3 stream multicast reaches a socket; F4.4 stop/start | `S95avb status` |
| #7 F5 | F5.1 bInterval 1; F5.2 bit-exact ramp through the gadget; F5.3 pitch +/-500 ppm; F5.4 <= 500 us; F5.5 TDM8 unchanged | `tdm8-uac2.sh status` |

---

## 3. Path A: the native Milan bridge

Path A runs Milan on the PB2 without PipeWire, with **the same control-plane
implementation as the RISC-V end station**: milan-fpga's Mark II firmware
([milan-fpga#665](https://github.com/kebag-logic/milan-fpga/issues/665)),
compiled unchanged against a software stand-in for the FPGA fabric. The design,
the build and the tests are in [`milan-linux/README.md`](milan-linux/README.md).

| Ticket | State |
|---|---|
| #8 A1, control plane on Linux | `milan-ctrld`: ADP, ACMP, MAAP. Passes milan-fpga's gate at the pin and the host network-namespace test; not yet run on the board |
| #9 A2, entity description | `entity.conf` is generated from the 1x1 TDM8 shape; the PB2's own end-station config needs a non-FPGA target in milan-fpga's builder |
| #10 A4, #11 A3, media clock and talker | `milan-mediad`: the talker on the media clock and the servo on the gadget's feedback. Bit-exact and gap-free in the host test with a +80 ppm host; not yet run on the board |
| #12 A5, listener | `milan-mediad`'s listener: placed and held at its presentation time through the gadget's playback pitch, with Milan's STREAM_INPUT counters. Bit-exact between two bridges in the host test; not yet run on the board |
| #13 A6, saved state | milan-fpga's KLJ2 store on a journal file: a binding survives a power cut and fast-connects at boot; 20 random power cuts never leave a torn journal (host test); not yet run on the board |
| #14 A7, SRP, AECP, interop | waits for milan-fpga SRP (#690) and AECP (#665 lane F5) |

With `AVB_STACK=native`, `S95avb` runs `/usr/sbin/milan-bridge.sh start` after
gPTP. That starts two daemons:

* `milan-ctrld -i eth0 -e /etc/milan/entity.conf -V 2 -s`, at SCHED_FIFO
  `AVB_CTRLD_PRIO` (40, below `flexptpd`). It reads the grandmaster from
  flexptpd's status block `/dev/shm/flexptpd.eth0` and publishes the streams in
  `/dev/shm/milan-datapath`.
* `milan-mediad`, whose talker and listener threads run at SCHED_FIFO 70 and 69
  on CPU 3. It waits for the UAC2 gadget. Then the talker streams whatever the
  host plays, starting once MAAP holds a destination. The listener plays
  whichever stream ACMP settles on, at its presentation time.

On the host:

```sh
aplay   -D hw:<the bridge's card>,0 -f S32_LE -c 8 -r 48000 file.wav   # to Milan
arecord -D hw:<the bridge's card>,0 -f S32_LE -c 8 -r 48000 out.wav    # from Milan
```

`AVB_SRP_DOMAIN=none`, the default until SRP (#14), runs `milan-ctrld -n` for a
direct link: a settled listener takes its talker as registered instead of
re-probing every 10 s. Through an AVB switch, streams need SRP.

```sh
/usr/sbin/milan-bridge.sh status   # state into syslog, then the datapath block:
milan-dp
#   entity_id=<eth0 EUI-64>  gm_id=<grandmaster>  maap_valid=1  maap_base=91e0f000....
#   source0=stream_id:<mac>0000 dest_mac:91e0f000.... vlan:2 dest_mac_valid:1
#   sink0=stream_id:... listening:1        (after a controller's BIND_RX settles)
#   talker_active=1 talker_locked=1 talker_pitch=<1e6 - the host's offset in ppm>
#   talker_frames_tx=... talker_underruns=0 talker_overruns=0 talker_level_avg=24.0
```

Until AECP arrives (#14), a controller cannot enumerate the entity. Bind
it by entity ID with ACMP: BIND_RX names the PB2's entity ID and listener
unique ID 0, or names it as the talker.

---

## 4. Files

| File | What |
|---|---|
| `res/kl-pb2-am62.config` | the real-time block |
| `build-tdm8-uac2-pb2.sh` | `PB2_ETHCAP_APPEND` on the `ethcap` label, the real-time config check |
| `br2-external/board/bb-pocketbeagle2/rootfs-overlay/etc/avb/avb.env` | every setting of this file |
| `.../etc/avb/flexptpd.conf` | flexptpd's configuration (802.1AS) |
| `br2-external/package/flexptp-gptp/` | flexptpd, built from our flexPTP fork at `flexptp.pin` |
| `.../etc/avb/uac2-milan.env` | the bridge's gadget profile |
| `.../etc/init.d/S95avb` | the service |
| `.../usr/sbin/avb-irq.sh`, `avb-gptp.sh`, `avb-shaper.sh` | one layer each |
| `br2-external/board/bb-pocketbeagle2/post-build.sh` | modes, and the removal of `S65ptp4l`/`S66phc2sys` |
| `br2-external/board/common/rootfs-overlay/etc/init.d/S99usb_gadgets` | the AVB gadget branch |
| `br2-external/board/common/rootfs-overlay/usr/sbin/tdm8-uac2.sh` | `TDM8_HS_BINT` and the gadget strings |
| `br2-external/configs/bb_pocketbeagle2_avb_defconfig` | `stress-ng`, `tcpdump` |
| `validation/pb2-tsn/` | the validation kit |
| `milan-linux/` | path A: `milan-ctrld`, `milan-dp`, `milan-bridge.sh`, `config/entity.conf`, `milan-fpga.pin`, the host tests |
| `br2-external/package/milan-fpga-src`, `milan-bridge` | the pinned milan-fpga archive, and path A's daemons, in the image |

---

## 5. Bench log

The rev B board on the bench: `eth0` on the AVB switch, with the grandmaster
`3cc0c6.fffe.fe0210`, a Milan reference device, the FPGA end station and an
I210 host. This machine is the USB host, playing into the bridge's UAC2 gadget.
The image was built from branch `pb2-tsn`.

The first boot, with no manual step:

| Layer | Seen on the board |
|---|---|
| kernel | `7.1.0-tdm8-pb2-rt SMP PREEMPT_RT`, CPU 3 isolated (`/proc/cmdline`) |
| gPTP | `ptp4l` SLAVE to the grandmaster through the switch, about 6 s after link-up; peer delay 202 ns; rms offset 28–36 ns once settled. `phc2sys` holds `CLOCK_REALTIME` within about 110 ns of the PHC |
| shaper | `p0-rx-ptype-rrobin` off (the driver's default is on); 2 TX channels; hardware `mqprio`, class A 17 Mbit/s; the talker's PDUs counted on TC1 |
| control plane | ADP AVAILABLE with the grandmaster; MAAP holds 2 addresses. From the I210 host (`milan-linux/tests/acmp.py`): PROBE_TX answered SUCCESS with the stream, the MAAP address and VLAN 2 (99 us on the board, 0.3 ms round trip); TALKER_UNKNOWN_ID for source 9; GET_TX_STATE and GET_RX_STATE; BIND_RX to the FPGA end station's talker → probe, settled on its stream, UNBIND_RX |
| USB | the host sees "PocketBeagle 2 USB-Milan bridge" at high speed: OUT and IN data endpoints asynchronous, bInterval 1 (125 us), 224-byte packets; feedback endpoint at 1 ms |
| talker | `aplay` of the counting ramp: the servo locks within 20 s. The board's own capture of 160 000 PDUs (20 s): VLAN 2 PCP 3, AVTP step exactly 125 000 ns throughout, AAF fields as Milan's, departure jitter 17 us p99 and 29 us p99.9, 0 bit errors in the ramp. Worst send delay after a slot: 15 us on CPU 3 |

What the bench changed:

* **The buffer level target is 48 frames (1 ms), not 24.** Behind this host's
  USB delivery (a VM with USB passthrough), 24 frames ran dry 52 times a minute.
  48 frames never did over the minutes measured, with 31 frames to spare at the
  worst, and the bridge latency stays inside 2 ms. Under load on the USB host
  itself, it still underruns: a host that drops USB packets loses frames before
  the bridge sees them.
* **The servo's lock band is one USB packet (+-6 frames).** Real delivery holds
  the 50 ms average within +-1 frame, with bursts of +5 after a gap.
* **The talker sleeps on `CLOCK_MONOTONIC`, and restarts its stream on a gPTP
  jump.** At the first lock, `ptp4l` steps the PHC to the grandmaster's time, and
  `phc2sys` then steps the system clock. An absolute sleep on `CLOCK_TAI`
  slept through days of the step.
* **The talker goes IDLE when no frame arrives for 10 ms, not when the buffer is
  empty.** A host that stops leaves a few frames, fewer than a PDU.
* **ADP and ACMP from other entities arrive priority-tagged (VID 0).** The kernel
  moves the tag to the packet's metadata, so AF_PACKET sees them untagged. The
  Mark II filter admits only what concerns the entity: ADP from a bound talker,
  ACMP addressed to it, MAAP that conflicts with its range.
* **Through the AVB switch, streams need SRP.** The switch forwards neither the
  PB2's stream nor any other to a port without an MSRP registration, and a Milan
  talker streams only once a listener registers. The stream paths through the
  switch (A3.3, A3.5, A5) therefore wait for SRP (#14); a direct cable tests
  them without it.

### gPTP on flexptpd (#19)

The same board and grandmaster, with `flexptpd` in place of `ptp4l` and
`phc2sys`: our flexPTP fork's 802.1AS end station, run as the image's
`avb-gptp.sh` does (SCHED_FIFO 53). Judged from the wire by
`milan-gptp-watch`, the link delay at 203 ns.

| Run | Result |
|---|---|
| 10 min, after 60 s to lock | 4800 of 4800 Syncs; offset mean 4 ns, \|offset\| p50 7 ns, p99 31 ns, max 40 ns; 100 % within 100 ns: **pass** |
| eth0 down 2 s, then 120 s watched | link down: DISABLED (no false grandmaster); link up: asCapable, LISTENING, SLAVE to the grandmaster. Syncs again 5 s after link-up; over the 120 s, 927 Syncs, p99 46 ns, max 51 ns, 100 % within 100 ns: **pass**. No clock step: the frequency held through the drop |

For comparison, the same window on the same bench:
- linuxptp: within 54 ns, but each link drop stepped the system clock by 37 s
  through phc2sys.
- Excelfore's gPTP daemon: p99 121 ns.
- upstream flexPTP: p99 24 ns, but it hung on the link drop.

The fork's conformance suite (`tests/conformance/run.sh`, 50 checks of G1 to
G5) passes in a network namespace.
