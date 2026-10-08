// SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
// SPDX-License-Identifier: Apache-2.0
//
// milan_mediad.c - the media plane of the PocketBeagle 2 USB-to-Milan bridge:
// the AAF talker (USB to Milan) and its media clock servo (issues #10, #11).
//
// It takes its streams from the datapath block milan-ctrld publishes, its
// time from gPTP (gptp_time.h), its frames from the UAC2 gadget's capture PCM,
// and reports in the media block (media.h, printed by milan-dp). The talker
// runs in a SCHED_FIFO thread on the core the "ethcap" label isolates; this
// thread recalibrates gPTP time once a second and publishes the reports.

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "audio_src.h"
#include "datapath.h"
#include "gptp_time.h"
#include "media.h"
#include "listener.h"
#include "talker.h"

static struct {
	const char *ifname;
	const char *vlan_ifname;
	const char *pcm;
	const char *ctl;
	const char *dp_name;
	const char *media_name;
	const char *sim;                // simulated host's ppm, NULL for the gadget
	const char *sim_wav;            // the simulated host's recording of the listener's stream
	const char *playback;
	bool talker, listener;
	int64_t in_flight_ns;
	int64_t interrupt_ns;
	unsigned channels;
	uint32_t pto_ns;
	unsigned level_target;
	unsigned level_max;
	int rt_priority;
	int cpu;
	bool use_syslog;
} opt = {
	.ifname = "eth0",
	.vlan_ifname = NULL,
	.pcm = "hw:CARD=UAC2Gadget,DEV=0",
	.playback = "hw:CARD=UAC2Gadget,DEV=0",
	.ctl = "hw:CARD=UAC2Gadget",
	.talker = true,
	.listener = true,
	.in_flight_ns = 500000,
	.interrupt_ns = 10000000,
	.dp_name = MILAN_DP_NAME,
	.media_name = MILAN_MEDIA_NAME,
	.channels = 8,
	.pto_ns = 2000000,
	.level_target = 24,
	.level_max = 96,
	.rt_priority = 70,
	.cpu = -1,
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t dump_requested;

static void say(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (opt.use_syslog) {
		vsyslog(level, fmt, ap);
	} else {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
	va_end(ap);
}

static void on_signal(int sig)
{
	if (sig == SIGUSR1) {
		dump_requested = 1;
	} else {
		stop_requested = 1;
	}
}

static void usage(FILE *to)
{
	fprintf(to,
		"usage: milan-mediad [options]\n"
		"  -i IFACE    AVB interface, for its PTP clock (eth0)\n"
		"  -I IFACE    where the PDUs go, tagged with the stream's VLAN (the -i interface)\n"
		"  -D PCM      the gadget's capture PCM (hw:CARD=UAC2Gadget,DEV=0)\n"
		"  -E PCM      the gadget's playback PCM (hw:CARD=UAC2Gadget,DEV=0)\n"
		"  -C CTL      its control device, for the two pitches (hw:CARD=UAC2Gadget)\n"
		"  -r ROLES    talker, listener or both (talker,listener)\n"
		"  -R NS       the USB IN requests already filled, for the listener (500000: 4 x 125 us)\n"
		"  -T NS       no PDU for this long is STREAM_INTERRUPTED (10000000)\n"
		"  -S PPM      instead of the gadget, a simulated USB host PPM off gPTP (tests)\n"
		"  -W FILE     with -S, the WAV the simulated host records the listener's stream into\n"
		"  -c N        channels (8)\n"
		"  -o NS       presentation time offset (2000000)\n"
		"  -L FRAMES   buffer level the servo holds (24 = 500 us)\n"
		"  -M FRAMES   buffer level past which frames are dropped (96)\n"
		"  -P PRIO     SCHED_FIFO priority of the talker thread (70), 0 for none\n"
		"  -a CPU      CPU of the talker thread (any)\n"
		"  -d NAME     datapath block (" MILAN_DP_NAME ")\n"
		"  -m NAME     media block (" MILAN_MEDIA_NAME ")\n"
		"  -s          log to syslog\n"
		"SIGUSR1 logs the talker's state; SIGTERM or SIGINT stops.\n");
}

static int parse(int argc, char **argv)
{
	int c;
	while ((c = getopt(argc, argv, "i:I:D:E:C:r:R:T:S:W:c:o:L:M:P:a:d:m:sh")) != -1) {
		switch (c) {
		case 'i': opt.ifname = optarg; break;
		case 'I': opt.vlan_ifname = optarg; break;
		case 'D': opt.pcm = optarg; break;
		case 'E': opt.playback = optarg; break;
		case 'C': opt.ctl = optarg; break;
		case 'r':
			opt.talker = strstr(optarg, "talker") != NULL;
			opt.listener = strstr(optarg, "listener") != NULL;
			break;
		case 'R': opt.in_flight_ns = strtoll(optarg, NULL, 0); break;
		case 'T': opt.interrupt_ns = strtoll(optarg, NULL, 0); break;
		case 'S': opt.sim = optarg; break;
		case 'W': opt.sim_wav = optarg; break;
		case 'c': opt.channels = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'o': opt.pto_ns = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'L': opt.level_target = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'M': opt.level_max = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'P': opt.rt_priority = atoi(optarg); break;
		case 'a': opt.cpu = atoi(optarg); break;
		case 'd': opt.dp_name = optarg; break;
		case 'm': opt.media_name = optarg; break;
		case 's': opt.use_syslog = true; break;
		case 'h': usage(stdout); exit(0);
		default: usage(stderr); return -1;
		}
	}
	return optind == argc ? 0 : -1;
}

static void report(const struct milan_media_talker *t, const struct milan_media_listener *l,
		   const struct gptp_time *clk)
{
	if (opt.listener) {
		say(LOG_INFO,
		    "listener %s%s stream %016llx | FRAMES_RX %llu SEQ_NUM_MISMATCH %llu LATE %llu EARLY %llu "
		    "UNSUPPORTED %llu MEDIA_LOCKED %llu MEDIA_UNLOCKED %llu MEDIA_RESET %llu STREAM_INTERRUPTED %llu "
		    "| underruns %llu pitch %u | presentation error %d..%d avg %d ns, least margin %d ns",
		    l->active ? "active" : "idle", l->locked ? ", locked" : "", (unsigned long long)l->stream_id,
		    (unsigned long long)l->frames_rx, (unsigned long long)l->seq_mismatch,
		    (unsigned long long)l->late_timestamp, (unsigned long long)l->early_timestamp,
		    (unsigned long long)l->unsupported_format, (unsigned long long)l->media_locked,
		    (unsigned long long)l->media_unlocked, (unsigned long long)l->media_resets,
		    (unsigned long long)l->stream_interrupted, (unsigned long long)l->underruns, l->pitch,
		    l->align_min_ns == INT32_MAX ? 0 : l->align_min_ns, l->align_max_ns == INT32_MIN ? 0 : l->align_max_ns,
		    l->align_avg_ns, l->margin_min_ns == INT32_MAX ? 0 : l->margin_min_ns);
	}
	if (!opt.talker) {
		return;
	}
	say(LOG_INFO,
	    "talker %s%s stream %016llx -> %012llx | PDUs %llu underruns %llu overruns %llu late %llu send errors %llu "
	    "| level %d..%d avg %.2f (target %d) pitch %u | worst send delay %d ns | gPTP corr %lld ns residual %lld ns",
	    t->active ? "active" : "idle", t->locked ? ", locked" : "", (unsigned long long)t->stream_id,
	    (unsigned long long)t->dest_mac, (unsigned long long)t->frames_tx, (unsigned long long)t->underruns,
	    (unsigned long long)t->overruns, (unsigned long long)t->late, (unsigned long long)t->send_errors,
	    t->level_min == INT32_MAX ? 0 : t->level_min, t->level_max == INT32_MIN ? 0 : t->level_max,
	    t->level_avg_milli / 1000.0, t->level_target, t->pitch, t->late_max_ns,
	    (long long)atomic_load(&clk->corr_ns), (long long)clk->residual_ns);
}

int main(int argc, char **argv)
{
	if (parse(argc, argv) != 0) {
		return 2;
	}
	if (opt.use_syslog) {
		openlog("milan-mediad", LOG_PID, LOG_DAEMON);
	}
	if (opt.vlan_ifname == NULL) {
		opt.vlan_ifname = opt.ifname;
	}
	struct gptp_time clk;
	int rc = gptp_time_open(&clk, opt.ifname);
	if (rc != 0) {
		say(LOG_ERR, "PTP clock of %s: %s", opt.ifname, strerror(-rc));
		return 1;
	}
	gptp_time_calibrate(&clk);
	if (clk.phc_fd < 0) {
		say(LOG_WARNING, "%s has no PTP clock: gPTP time is CLOCK_TAI", opt.ifname);
	}

	struct sigaction sa = {.sa_handler = on_signal};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	// S99usb_gadgets builds the gadget after S95avb has started us: wait for it
	struct audio_src *src = NULL;
	struct audio_sink *snk = NULL;
	for (int i = 0; ((opt.talker && src == NULL) || (opt.listener && snk == NULL)) && !stop_requested; ++i) {
		if (opt.talker && src == NULL) {
			src = opt.sim != NULL ? audio_sim_open(opt.channels, 48000, strtod(opt.sim, NULL))
					      : audio_alsa_open(opt.pcm, opt.ctl, opt.channels, 48000);
		}
		if (opt.listener && snk == NULL) {
			snk = opt.sim != NULL ? audio_sim_sink_open(opt.channels, 48000, strtod(opt.sim, NULL), opt.sim_wav)
					      : audio_alsa_sink_open(opt.playback, opt.ctl, opt.channels, 48000);
		}
		if ((opt.talker && src == NULL) || (opt.listener && snk == NULL)) {
			if (i == 0) {
				say(LOG_INFO, "waiting for the gadget's PCMs");
			}
			sleep(1);
		}
	}
	if (stop_requested) {
		return 0;
	}
	const struct milan_dp *dp = NULL;
	for (int i = 0; dp == NULL && !stop_requested; ++i) {
		dp = milan_dp_open(opt.dp_name);
		if (dp == NULL) {
			if (i == 0) {
				say(LOG_INFO, "waiting for milan-ctrld's datapath block %s", opt.dp_name);
			}
			sleep(1);
		}
	}
	struct milan_media *media = milan_media_create(opt.media_name);
	if (media == NULL) {
		say(LOG_ERR, "media block %s: %s", opt.media_name, strerror(errno));
		return 1;
	}
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		say(LOG_WARNING, "mlockall: %s", strerror(errno));
	}

	static struct talker tk;
	struct talker_cfg tcfg = {
		.ifname = opt.vlan_ifname,
		.priority = 3,
		.source = 0,
		.channels = opt.channels,
		.frames_per_pdu = 6,
		.pto_ns = opt.pto_ns,
		.level_target = opt.level_target,
		.level_max = opt.level_max,
		.late_ns = 50000,
		.rt_priority = opt.rt_priority,
		.cpu = opt.cpu,
		.src = src,
		.clk = &clk,
		.dp = dp,
	};
	if (opt.talker) {
		rc = talker_start(&tk, &tcfg);
		if (rc != 0) {
			say(LOG_ERR, "talker thread: %s", strerror(rc));
			return 1;
		}
		say(LOG_INFO, "talker on %s, %u channels, PTO %u ns, level %u..%u frames, PHC %d", opt.vlan_ifname,
		    opt.channels, opt.pto_ns, opt.level_target, opt.level_max, clk.phc_index);
	}
	static struct listener ls;
	struct listener_cfg lcfg = {
		.ifname = opt.ifname,
		.sink = 0,
		.channels = opt.channels,
		.pto_ns = opt.pto_ns,
		.in_flight_ns = opt.sim != NULL ? 0 : opt.in_flight_ns,
		.interrupt_ns = opt.interrupt_ns,
		.rt_priority = opt.rt_priority > 1 ? opt.rt_priority - 1 : opt.rt_priority,
		.cpu = opt.cpu,
		.snk = snk,
		.clk = &clk,
		.dp = dp,
	};
	if (opt.listener) {
		rc = listener_start(&ls, &lcfg);
		if (rc != 0) {
			say(LOG_ERR, "listener thread: %s", strerror(rc));
			return 1;
		}
		say(LOG_INFO, "listener on %s, %u channels, USB IN pipeline %lld ns", opt.ifname, opt.channels,
		    (long long)lcfg.in_flight_ns);
	}

	struct milan_media_talker t;
	struct milan_media_listener l;
	memset(&t, 0, sizeof t);
	memset(&l, 0, sizeof l);
	for (unsigned tick = 0; !stop_requested; ++tick) {
		struct timespec ts = {0, 100000000};
		nanosleep(&ts, NULL);
		if (tick % 10u == 9u) {
			gptp_time_calibrate(&clk);
		}
		if (opt.talker) {
			talker_report(&tk, &t);
		}
		if (opt.listener) {
			listener_report(&ls, &l);
		}
		milan_media_begin(media);
		media->gptp_corr_ns = atomic_load(&clk.corr_ns);
		media->gptp_residual_ns = clk.residual_ns;
		media->gptp_calibrated = clk.calibrated;
		media->talker = t;
		media->listener = l;
		milan_media_end(media);
		if (dump_requested) {
			dump_requested = 0;
			report(&t, &l, &clk);
		}
	}
	if (opt.talker) {
		talker_stop(&tk);
		talker_report(&tk, &t);
	}
	if (opt.listener) {
		listener_stop(&ls);
		listener_report(&ls, &l);
	}
	report(&t, &l, &clk);
	if (src != NULL) {
		src->close(src);
	}
	if (snk != NULL) {
		snk->close(snk);
	}
	gptp_time_close(&clk);
	return 0;
}
