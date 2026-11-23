// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// rxsock.h - the receive socket of a media input (the AAF listener, the CRF
// receive path), and how its setup ended.
//
// An input's counters are valid only while a program counts them (media.h):
// its thread marks them valid once this setup has succeeded, and a setup that
// fails leaves them invalid, no descriptor open, and the step and errno for
// the main thread to log (rxsock_failed).

#ifndef MILAN_RXSOCK_H
#define MILAN_RXSOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// How an input's setup ended: 0 still running, 1 receiving, -errno failed.
struct rxsock_state {
	atomic_int result;
	const char *step;               // the step that failed, set before result
	atomic_bool logged;
};

// AF_PACKET on ifname, the input's filter closed (filter(fd, 0)), bound to the
// interface, a 5 ms receive timeout. The descriptor, or -1 with the state
// failed and nothing left open.
int rxsock_open(struct rxsock_state *st, const char *ifname, int (*filter)(int fd, uint64_t mac), int *ifindex);

// The step and errno of a failed setup, the first time it is asked: true once.
bool rxsock_failed(struct rxsock_state *st, const char **step, int *err);

#endif // MILAN_RXSOCK_H
