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

> **Status:** built and checked on the host (kernel config, image contents,
> script dry runs). Not yet run on a board. Each ticket closes only on the bench
> evidence its *Validation* section names.

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
| gPTP | #4 | `/etc/avb/gPTP.cfg`, `/usr/sbin/avb-gptp.sh` | `ptp4l` on the CPTS clock of `eth0`, `phc2sys` to `CLOCK_REALTIME`, read-only socket `/var/run/ptp4lro` |
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

`avb-irq.sh` then pins the two interrupt sources the bridge's traffic uses
(`AVB_IRQ_MATCH`: the CPSW at `8000000.ethernet` and the USB controller at
`31000000.usb`) to `AVB_IRQ_CPU`. CPUs 0 and 1 serve everything else. On
`PREEMPT_RT` each handler is an `irq/<n>-<name>` thread that follows its IRQ's
affinity. `avb-irq.sh status` shows where each one landed.

### 1.2 gPTP (#4)

`/etc/avb/gPTP.cfg` holds the 802.1AS attributes the other AM62x boards use
(the pipewire-helper `gPTP.cfg`): L2, peer delay, `transportSpecific 0x1`,
`01:80:C2:00:00:0E`, sync every 125 ms, `priority1 248` so a bench grandmaster
wins. On top of those, `tx_timestamp_timeout 20` covers the CPTS, which delivers
TX timestamps through a work item.

`avb-gptp.sh start` runs `ptp4l -f /etc/avb/gPTP.cfg -i eth0` at SCHED_FIFO 53
and `phc2sys -s eth0 -c CLOCK_REALTIME -w` at 52. Both log to syslog
(`/var/log/messages`). `avb-gptp.sh status` prints the port state, the master
offset, the peer delay and the grandmaster:

```sh
pmc -u -b 0 -t 1 'GET PORT_DATA_SET' 'GET TIME_STATUS_NP' 'GET PARENT_DATA_SET'
```

`-t 1` is required: with `transportSpecific 0x1`, `ptp4l` ignores management
messages that carry 0.

**Buildroot's own linuxptp init scripts are removed.** The linuxptp package
installs `S65ptp4l` and `S66phc2sys`, which run `ptp4l` on `eth0` from
`/etc/linuxptp.cfg`. That configuration is UDPv4, end-to-end, client only: it is
not gPTP. Those scripts would also start before the shaper takes `eth0` down
(§1.3). The PB2 `post-build.sh` deletes both, and `S95avb` starts gPTP after the
shaper.

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
| #3 F1 | F1.1 `/sys/kernel/realtime` = 1; F1.2 `check-rt.sh` for 1 h under load, max <= 100 us; F1.3 README.pb2-ethcap.md §5; F1.4 30 min TDM8 bridge on the RT kernel | `cat /sys/kernel/realtime; uname -v` |
| #4 F2 | F2.1 `ethtool -T`; F2.2 SLAVE within 10 s; F2.3 offset <= 100 ns at 99.9 % over 30 min; F2.4 phc2sys; F2.5 GM change; F2.6 PB2 as GM | `avb-gptp.sh status`, `check-gptp.sh` |
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
  `AVB_CTRLD_PRIO` (40, below `ptp4l` and `phc2sys`). It reads the grandmaster
  from `/var/run/ptp4lro` and publishes the streams in `/dev/shm/milan-datapath`.
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
| `.../etc/avb/gPTP.cfg` | 802.1AS |
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
