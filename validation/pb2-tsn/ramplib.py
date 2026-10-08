# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic

"""The counting ramp shared by ramp-gen.py and ramp-check.py.

Sample (n, c) of an interleaved C-channel ramp is

    value(n, c) = ((n mod N) * C + c) << shift        (32 bits, unsigned)

with N = 2^(32 - shift) // C, so the interleaved stream counts up by one
(times 2^shift) from sample to sample, every channel carries its own index
in value mod C, and a frame's counter n is value >> shift // C. shift = 8
keeps the ramp in the top 24 bits, for a path that is 24-bit inside.
"""

import sys
from array import array

assert array("I").itemsize == 4, "array('I') must be 32-bit"


class Ramp:
    def __init__(self, channels, shift=0):
        if not 1 <= channels <= 1023:
            raise ValueError("channels must be 1..1023")
        if not 0 <= shift <= 24:
            raise ValueError("shift must be 0..24")
        self.C = channels
        self.shift = shift
        self.N = (1 << (32 - shift)) // channels
        self.low_mask = (1 << shift) - 1

    def value(self, n, c):
        return (((n % self.N) * self.C + c) << self.shift) & 0xFFFFFFFF

    def chunk(self, n0, count):
        """array('I') of `count` interleaved frames starting at counter n0."""
        out = array("I")
        step = 1 << self.shift
        n = n0 % self.N
        left = count
        while left:
            run = min(left, self.N - n)
            out.extend(range((n * self.C) << self.shift, ((n + run) * self.C) << self.shift, step))
            left -= run
            n = 0
        return out

    def chunk_le_bytes(self, n0, count):
        a = self.chunk(n0, count)
        if sys.byteorder == "big":
            a.byteswap()
        return a.tobytes()

    def decode(self, v):
        """(n, channel) of one sample, or None if it is not a ramp value."""
        if v & self.low_mask:
            return None
        k = v >> self.shift
        if k >= self.N * self.C:
            return None
        return divmod(k, self.C)
