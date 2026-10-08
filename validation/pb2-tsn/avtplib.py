# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""Shared helpers for the PB2 TSN validation kit: pcap/pcapng reading,
Ethernet/VLAN and AVTP AAF parsing, WAV I/O and small statistics.

Python 3 standard library only, so it runs on any tap or peer host.
"""

import math
import struct
import sys

ETH_P_8021Q = 0x8100
ETH_P_8021AD = 0x88A8
ETH_P_AVTP = 0x22F0

LINKTYPE_ETHERNET = 1

AVTP_SUBTYPE_AAF = 0x02
AVTP_SUBTYPE_CRF = 0x04

# IEEE 1722-2016 Table 7-2 (nsr) and Table 7-3 (format)
AAF_NSR_HZ = {
    0x0: 0, 0x1: 8000, 0x2: 16000, 0x3: 32000, 0x4: 44100, 0x5: 48000,
    0x6: 88200, 0x7: 96000, 0x8: 176400, 0x9: 192000, 0xA: 24000,
}
AAF_FORMAT_NAME = {
    0x00: "USER", 0x01: "FLOAT_32BIT", 0x02: "INT_32BIT",
    0x03: "INT_24BIT", 0x04: "INT_16BIT", 0x05: "AES3_32BIT",
}
AAF_FORMAT_BYTES = {0x01: 4, 0x02: 4, 0x03: 3, 0x04: 2, 0x05: 4}

AAF_HDR_LEN = 24


class PcapError(Exception):
    pass


def _pcap_classic(f, magic_bytes):
    magic_le = struct.unpack("<I", magic_bytes)[0]
    magic_be = struct.unpack(">I", magic_bytes)[0]
    if magic_le in (0xA1B2C3D4, 0xA1B23C4D):
        endian, magic = "<", magic_le
    elif magic_be in (0xA1B2C3D4, 0xA1B23C4D):
        endian, magic = ">", magic_be
    else:
        raise PcapError("not a pcap file")
    scale = 1 if magic == 0xA1B23C4D else 1000
    hdr = f.read(20)
    if len(hdr) < 20:
        raise PcapError("truncated pcap header")
    linktype = struct.unpack(endian + "HHiIII", hdr)[5] & 0x0FFFFFFF
    if linktype != LINKTYPE_ETHERNET:
        raise PcapError(f"linktype {linktype} is not Ethernet (capture on the interface, not 'any')")
    rec = struct.Struct(endian + "IIII")
    while True:
        h = f.read(16)
        if len(h) < 16:
            return
        sec, frac, incl, _orig = rec.unpack(h)
        data = f.read(incl)
        if len(data) < incl:
            return
        yield sec * 1_000_000_000 + frac * scale, data


def _tsresol_to_ns(code):
    # pcapng if_tsresol: high bit clear = 10^-n s, set = 2^-n s
    if code & 0x80:
        return 1e9 / (1 << (code & 0x7F))
    return 1e9 / (10 ** code)


def _pcapng(f, first4):
    endian = "<"
    ifaces = []  # (linktype, ns_per_tick)
    data = first4 + f.read(8)
    while True:
        if len(data) < 12:
            return
        btype_raw = data[0:4]
        if btype_raw == b"\x0a\x0d\x0d\x0a":
            bom = data[8:12]
            if bom == b"\x4d\x3c\x2b\x1a":
                endian = "<"
            elif bom == b"\x1a\x2b\x3c\x4d":
                endian = ">"
            else:
                raise PcapError("bad pcapng byte-order magic")
            ifaces = []
        btype, blen = struct.unpack(endian + "II", data[0:8])
        if blen < 12:
            raise PcapError("bad pcapng block length")
        rest = f.read(blen - 12)
        if len(rest) < blen - 12:
            return
        # body sits between the 8-byte head and the 4-byte trailing length
        body = (data[8:12] + rest)[: blen - 12]
        if btype == 0x00000001:  # IDB
            linktype = struct.unpack(endian + "H", body[0:2])[0]
            ns_per_tick = 1000.0
            opts = body[8:]
            i = 0
            while i + 4 <= len(opts):
                code, olen = struct.unpack(endian + "HH", opts[i:i + 4])
                if code == 0:
                    break
                if code == 9 and olen >= 1:
                    ns_per_tick = _tsresol_to_ns(opts[i + 4])
                i += 4 + ((olen + 3) & ~3)
            ifaces.append((linktype, ns_per_tick))
        elif btype == 0x00000006:  # EPB
            ifid, tsh, tsl, cap, _orig = struct.unpack(endian + "IIIII", body[0:20])
            if ifid >= len(ifaces):
                raise PcapError("EPB references an unknown interface")
            linktype, ns_per_tick = ifaces[ifid]
            if linktype != LINKTYPE_ETHERNET:
                raise PcapError(f"linktype {linktype} is not Ethernet")
            ticks = (tsh << 32) | tsl
            if ns_per_tick == 1.0:
                ts = ticks
            elif ns_per_tick >= 1.0 and float(ns_per_tick).is_integer():
                ts = ticks * int(ns_per_tick)
            else:
                ts = int(round(ticks * ns_per_tick))
            yield ts, body[20:20 + cap]
        elif btype == 0x00000003:  # SPB: no timestamp
            raise PcapError("simple packet blocks carry no timestamp")
        data = f.read(12)


def read_pcap(path):
    """Yield (timestamp_ns, frame_bytes) from a pcap or pcapng file."""
    with open(path, "rb") as f:
        first4 = f.read(4)
        if len(first4) < 4:
            raise PcapError("empty file")
        if first4 == b"\x0a\x0d\x0d\x0a":
            yield from _pcapng(f, first4)
        else:
            yield from _pcap_classic(f, first4)


def pcap_resolution_ns(path):
    """Best-effort timestamp resolution of a capture, in ns (1 or 1000 typically)."""
    with open(path, "rb") as f:
        head = f.read(4)
    if head == b"\x0a\x0d\x0d\x0a":
        return None
    le = struct.unpack("<I", head)[0]
    be = struct.unpack(">I", head)[0]
    return 1 if 0xA1B23C4D in (le, be) else 1000


def write_pcap(path, records, nanosecond=True):
    """Write classic little-endian pcap from (timestamp_ns, frame) pairs."""
    magic = 0xA1B23C4D if nanosecond else 0xA1B2C3D4
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", magic, 2, 4, 0, 0, 65535, LINKTYPE_ETHERNET))
        for ts, data in records:
            sec, rem = divmod(ts, 1_000_000_000)
            frac = rem if nanosecond else rem // 1000
            f.write(struct.pack("<IIII", sec, frac, len(data), len(data)))
            f.write(data)


def mac_str(b):
    return ":".join(f"{x:02x}" for x in b)


class Frame:
    __slots__ = ("ts", "dst", "src", "vid", "pcp", "ethertype", "payload")


def parse_ethernet(data):
    """Return a Frame with VLAN tags stripped, or None if too short."""
    if len(data) < 14:
        return None
    fr = Frame()
    fr.dst = data[0:6]
    fr.src = data[6:12]
    fr.vid = None
    fr.pcp = None
    et = struct.unpack("!H", data[12:14])[0]
    off = 14
    while et in (ETH_P_8021Q, ETH_P_8021AD) and len(data) >= off + 4:
        tci, et = struct.unpack("!HH", data[off:off + 4])
        if fr.vid is None:
            fr.vid = tci & 0x0FFF
            fr.pcp = tci >> 13
        off += 4
    fr.ethertype = et
    fr.payload = data[off:]
    return fr


class AafPdu:
    __slots__ = ("subtype", "sv", "version", "mr", "tv", "seq", "tu",
                 "stream_id", "avtp_ts", "format", "nsr", "channels",
                 "bit_depth", "sdl", "sp", "evt", "data")


def parse_avtp_stream(payload):
    """Parse an AVTP stream PDU common header; return AafPdu with AAF fields
    filled when the subtype is AAF. None if it is not a stream PDU."""
    if len(payload) < 12:
        return None
    p = AafPdu()
    p.subtype = payload[0]
    b1 = payload[1]
    p.sv = b1 >> 7
    p.version = (b1 >> 4) & 0x7
    p.mr = (b1 >> 3) & 1
    p.tv = b1 & 1
    p.seq = payload[2]
    p.tu = payload[3] & 1
    p.stream_id = struct.unpack("!Q", payload[4:12])[0]
    p.avtp_ts = None
    p.format = p.nsr = p.channels = p.bit_depth = p.sdl = p.sp = p.evt = None
    p.data = b""
    if len(payload) >= 16:
        p.avtp_ts = struct.unpack("!I", payload[12:16])[0]
    if p.subtype == AVTP_SUBTYPE_AAF and len(payload) >= AAF_HDR_LEN:
        p.format = payload[16]
        p.nsr = payload[17] >> 4
        p.channels = ((payload[17] & 0x03) << 8) | payload[18]
        p.bit_depth = payload[19]
        p.sdl = struct.unpack("!H", payload[20:22])[0]
        p.sp = (payload[22] >> 4) & 1
        p.evt = payload[22] & 0x0F
        p.data = payload[AAF_HDR_LEN:AAF_HDR_LEN + p.sdl]
    return p


def build_aaf_frame(dst, src, stream_id, seq, avtp_ts, samples_be, channels,
                    fmt=0x02, nsr=0x5, bit_depth=32, vid=2, pcp=3, tv=1):
    """Build one tagged Ethernet frame carrying an AAF PDU (for fixtures)."""
    sdl = len(samples_be)
    hdr = struct.pack(
        "!BBBBQIBBBBHBB",
        AVTP_SUBTYPE_AAF,
        0x80 | (tv & 1),  # sv=1, version 0, mr 0, tv
        seq & 0xFF,
        0,
        stream_id,
        avtp_ts & 0xFFFFFFFF,
        fmt,
        (nsr << 4) | ((channels >> 8) & 0x03),
        channels & 0xFF,
        bit_depth,
        sdl,
        0,
        0,
    )
    eth = dst + src
    if vid is not None:
        eth += struct.pack("!HH", ETH_P_8021Q, (pcp << 13) | vid)
    eth += struct.pack("!H", ETH_P_AVTP)
    return eth + hdr + samples_be


def iter_avtp(path):
    """Yield (ts_ns, Frame, AafPdu) for every AVTP stream PDU in a capture."""
    for ts, data in read_pcap(path):
        fr = parse_ethernet(data)
        if fr is None or fr.ethertype != ETH_P_AVTP:
            continue
        p = parse_avtp_stream(fr.payload)
        if p is None or not p.sv:
            continue
        fr.ts = ts
        yield ts, fr, p


def s32(x):
    """Interpret a 32-bit value modulo 2^32 as signed."""
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def percentile(sorted_vals, q):
    """Nearest-rank percentile (q in 0..100) of an already sorted list."""
    if not sorted_vals:
        return None
    k = max(0, min(len(sorted_vals) - 1, int(math.ceil(q / 100.0 * len(sorted_vals))) - 1))
    return sorted_vals[k]


class Stats:
    """Streaming min/max/mean/stdev."""

    def __init__(self):
        self.n = 0
        self.mean = 0.0
        self.m2 = 0.0
        self.min = None
        self.max = None

    def add(self, x):
        self.n += 1
        d = x - self.mean
        self.mean += d / self.n
        self.m2 += d * (x - self.mean)
        self.min = x if self.min is None or x < self.min else self.min
        self.max = x if self.max is None or x > self.max else self.max

    @property
    def stdev(self):
        return math.sqrt(self.m2 / (self.n - 1)) if self.n > 1 else 0.0


# ------------------------------------------------------------------ WAV ----

def write_wav_header(f, rate, channels, bits, nframes):
    block = channels * (bits // 8)
    data_len = nframes * block
    f.write(b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * block, block, bits))
    f.write(b"data" + struct.pack("<I", data_len))


def patch_wav_sizes(f, data_len):
    f.seek(4)
    f.write(struct.pack("<I", 36 + data_len))
    f.seek(40)
    f.write(struct.pack("<I", data_len))


class WavInfo:
    __slots__ = ("rate", "channels", "bits", "data_offset", "data_len")


def read_wav_info(path):
    """Locate the data chunk of a PCM WAV (plain or WAVE_FORMAT_EXTENSIBLE).
    A data size larger than the file (a streamed arecord) is clamped."""
    import os
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        riff = f.read(12)
        if len(riff) < 12 or riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
            raise ValueError("not a RIFF/WAVE file")
        info = WavInfo()
        info.rate = info.channels = info.bits = None
        while True:
            ch = f.read(8)
            if len(ch) < 8:
                raise ValueError("no data chunk")
            cid, clen = ch[0:4], struct.unpack("<I", ch[4:8])[0]
            if cid == b"fmt ":
                body = f.read(clen + (clen & 1))
                tag, info.channels, info.rate, _br, _ba, info.bits = struct.unpack("<HHIIHH", body[0:16])
                if tag == 0xFFFE and len(body) >= 40:
                    tag = struct.unpack("<H", body[24:26])[0]
                if tag != 1:
                    raise ValueError(f"WAV format tag {tag} is not integer PCM")
            elif cid == b"data":
                if info.channels is None:
                    raise ValueError("data chunk before fmt chunk")
                info.data_offset = f.tell()
                avail = size - info.data_offset
                info.data_len = min(clen, avail) if clen else avail
                block = info.channels * (info.bits // 8)
                info.data_len -= info.data_len % block
                return info
            else:
                f.seek(clen + (clen & 1), 1)


def die_usage(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(2)
