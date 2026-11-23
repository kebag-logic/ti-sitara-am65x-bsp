// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// rxsock.c - see rxsock.h.

#define _GNU_SOURCE
#include "rxsock.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int fail(struct rxsock_state *st, int fd, const char *step, int err)
{
	if (fd >= 0) {
		close(fd);
	}
	st->step = step;
	atomic_store(&st->result, -(err != 0 ? err : EIO));
	return -1;
}

int rxsock_open(struct rxsock_state *st, const char *ifname, int (*filter)(int fd, uint64_t mac), int *ifindex)
{
	*ifindex = (int)if_nametoindex(ifname);
	if (*ifindex == 0) {
		return fail(st, -1, "interface", errno);
	}
	int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (fd < 0) {
		return fail(st, -1, "socket", errno);
	}
	if (filter(fd, 0) != 0) {
		return fail(st, fd, "filter", errno);
	}
	struct sockaddr_ll sll = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = *ifindex};
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) != 0) {
		return fail(st, fd, "bind", errno);
	}
	struct timeval tv = {0, 5000};
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
		return fail(st, fd, "receive timeout", errno);
	}
	atomic_store(&st->result, 1);
	return fd;
}

bool rxsock_failed(struct rxsock_state *st, const char **step, int *err)
{
	int r = atomic_load(&st->result);
	if (r >= 0 || atomic_exchange(&st->logged, true)) {
		return false;
	}
	*step = st->step;
	*err = -r;
	return true;
}
