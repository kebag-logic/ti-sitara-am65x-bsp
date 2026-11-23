// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// rxfail.c - an LD_PRELOAD library for tests/rxfail.py: makes one step of a
// media input's receive socket setup (src/rxsock.c) fail, in one thread of
// milan-mediad only, and says on stderr what it did.
//
//   RXFAIL_THREAD  the thread's name (milan-listener, milan-crf-rx)
//   RXFAIL_STEP    interface, socket, filter, bind or timeout
//
// Each failure, and the close of a descriptor that thread opened, is written
// to stderr as "rxfail: <thread> <what>", so a test can tell that a failed
// setup left nothing open.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/filter.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

static __thread int opened = -1;        // the descriptor socket() gave this thread

static bool targeted(const char *step)
{
	const char *t = getenv("RXFAIL_THREAD"), *s = getenv("RXFAIL_STEP");
	char name[16] = {0};
	if (t == NULL || s == NULL || prctl(PR_GET_NAME, name) != 0) {
		return false;
	}
	return strcmp(name, t) == 0 && strcmp(s, step) == 0;
}

static bool in_thread(void)
{
	const char *t = getenv("RXFAIL_THREAD");
	char name[16] = {0};
	return t != NULL && prctl(PR_GET_NAME, name) == 0 && strcmp(name, t) == 0;
}

static void say(const char *fmt, ...)
{
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n > 0) {
		(void)!write(2, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1u);
	}
}

static int injected(const char *step, int err)
{
	say("rxfail: %s %s failed (%s)\n", getenv("RXFAIL_THREAD"), step, strerror(err));
	errno = err;
	return -1;
}

unsigned int if_nametoindex(const char *ifname)
{
	static unsigned int (*real)(const char *);
	if (real == NULL) {
		real = (unsigned int (*)(const char *))dlsym(RTLD_NEXT, "if_nametoindex");
	}
	if (targeted("interface")) {
		(void)injected("interface", ENODEV);
		return 0;
	}
	return real(ifname);
}

int socket(int domain, int type, int protocol)
{
	static int (*real)(int, int, int);
	if (real == NULL) {
		real = (int (*)(int, int, int))dlsym(RTLD_NEXT, "socket");
	}
	if (domain == AF_PACKET && targeted("socket")) {
		return injected("socket", EMFILE);
	}
	int fd = real(domain, type, protocol);
	if (domain == AF_PACKET && in_thread()) {
		opened = fd;
		say("rxfail: %s opened fd %d\n", getenv("RXFAIL_THREAD"), fd);
	}
	return fd;
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len)
{
	static int (*real)(int, int, int, const void *, socklen_t);
	if (real == NULL) {
		real = (int (*)(int, int, int, const void *, socklen_t))dlsym(RTLD_NEXT, "setsockopt");
	}
	if (fd == opened && level == SOL_SOCKET && name == SO_ATTACH_FILTER && targeted("filter")) {
		return injected("filter", ENOMEM);
	}
	if (fd == opened && level == SOL_SOCKET && name == SO_RCVTIMEO && targeted("timeout")) {
		return injected("timeout", EINVAL);
	}
	return real(fd, level, name, value, len);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	static int (*real)(int, const struct sockaddr *, socklen_t);
	if (real == NULL) {
		real = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "bind");
	}
	if (fd == opened && targeted("bind")) {
		return injected("bind", EADDRNOTAVAIL);
	}
	return real(fd, addr, len);
}

int close(int fd)
{
	static int (*real)(int);
	if (real == NULL) {
		real = (int (*)(int))dlsym(RTLD_NEXT, "close");
	}
	if (fd >= 0 && fd == opened) {
		say("rxfail: %s closed fd %d\n", getenv("RXFAIL_THREAD"), fd);
		opened = -1;
	}
	return real(fd);
}
