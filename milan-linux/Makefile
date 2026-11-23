# SPDX-FileCopyrightText: Copyright (c) 2026 Kebag-Logic
# SPDX-License-Identifier: Apache-2.0
#
# The PocketBeagle 2 USB-to-Milan bridge daemons.
#
#   make MILAN_FPGA=<milan-fpga tree at the pinned revision>     # host build
#   make MILAN_FPGA=... CC=aarch64-linux-gnu-gcc                  # cross build
#   make MILAN_FPGA=... check                                     # host tests
#
# The Mark II control-plane firmware is compiled from the milan-fpga tree,
# unmodified, with the flags its own gate uses (sw/firmware/ctrl/test/
# ctrl_build.py: C11, -Wall -Wextra -Werror -pedantic), and the units the split
# platform's image links: the portable set (ctrl_build.py PORTABLE, with the
# TSN stack's ADP, ACMP and MAAP), AECP with its adapter, face and saved-state
# owner and its bridge (ctrl_srp_image.py aecp_inputs), and the store. Every
# unit, the firmware's and this directory's, is compiled with the board's shape
# header force-included, as srp_arms.prepared does for an image
# (config/srp_entity_gen.h), and with CTRL_HOST_PLATFORM, the define that
# selects the host platform's row of THE SHAPES (ctrl_loop.h; milan-fpga#983).
# The Linux side is GNU C11, since it needs the system's socket, epoll and
# netlink headers. tools/fetch-milan-fpga.sh fetches the pinned tree
# (milan-fpga.pin); tools/gen-config.sh writes config/ from the board's
# end-station config.

MILAN_FPGA ?= .milan-fpga
CC ?= cc
O ?= build
PREFIX ?= /usr
DESTDIR ?=

CTRL := $(MILAN_FPGA)/sw/firmware/ctrl
NVM := $(MILAN_FPGA)/sw/firmware/ctrl_nvm
STACK := $(MILAN_FPGA)/third_party/tsn-c-stack
LWSRP := $(MILAN_FPGA)/third_party/lwSRP
FW_DIRS := mbx host port loop adp maap acmp app aecp srp
# config/ holds the generated headers: the AEM image, the shape, the store's shape
FW_INC := $(addprefix -I$(CTRL)/,$(FW_DIRS)) -I$(STACK)/include -I$(LWSRP)/src/include -I$(NVM) -Iconfig
SHAPE := -include config/srp_entity_gen.h -DCTRL_HOST_PLATFORM
FW_SRCS := $(addprefix $(CTRL)/,mbx/mbx.c loop/ctrl_loop.c port/ctrl_pool.c port/ctrl_debug.c port/shlan_port.c) \
	$(STACK)/src/adp.c $(CTRL)/adp/adp_mbx.c $(STACK)/src/maap.c $(CTRL)/maap/maap_mbx.c \
	$(CTRL)/maap/maap_csr.c $(STACK)/src/acmp.c $(addprefix $(CTRL)/,acmp/acmp_mbx.c acmp/acmp_nvm.c \
	app/ctrl_app.c host/mbx_model.c) \
	$(addprefix $(CTRL)/aecp/,aecp.c aecp_commands.c aecp_maps.c aecp_image.c aecp_state.c aecp_mbx.c \
	aecp_nvm.c aecp_face.c) $(CTRL)/app/ctrl_app_aecp.c \
	$(addprefix $(NVM)/,nvm_klj2.c nvm_store.c)
FW_OBJS := $(addprefix $(O)/fw/,$(notdir $(FW_SRCS:.c=.o)))
GEN := config/srp_entity_gen.h config/aecp_entity_gen.h config/nvm_shape_gen.h

FW_CFLAGS := -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror -pedantic $(FW_INC) $(SHAPE)
OWN_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc $(FW_INC) $(SHAPE) $(CFLAGS)

CTRLD_SRCS := src/milan_ctrld.c src/softfab.c src/gptp_shm.c src/datapath.c src/shmblk.c src/entity_conf.c \
	src/nvm_file.c src/aecp_far.c src/media.c
CTRLD_OBJS := $(addprefix $(O)/,$(notdir $(CTRLD_SRCS:.c=.o)))
MEDIAD_SRCS := src/milan_mediad.c src/talker.c src/listener.c src/aaf.c src/servo.c src/gptp_time.c src/audio_alsa.c \
	src/audio_sim.c src/datapath.c src/media.c src/shmblk.c
MEDIAD_OBJS := $(addprefix $(O)/,$(notdir $(MEDIAD_SRCS:.c=.o)))
DP_OBJS := $(addprefix $(O)/,milan_dp.o datapath.o media.o shmblk.o gptp_shm.o)

all: $(O)/milan-ctrld $(O)/milan-mediad $(O)/milan-dp $(O)/milan-gptp-watch

$(CTRL)/mbx/mbx.c:
	@echo "no milan-fpga tree at MILAN_FPGA=$(MILAN_FPGA): run tools/fetch-milan-fpga.sh" >&2; exit 1

$(O)/fw/%.o: $(GEN) | $(CTRL)/mbx/mbx.c
	@mkdir -p $(dir $@)
	$(CC) $(FW_CFLAGS) -c $(filter %/$(notdir $(@:.o=.c)),$(FW_SRCS)) -o $@

$(O)/%.o: src/%.c src/*.h $(GEN) | $(CTRL)/mbx/mbx.c
	@mkdir -p $(dir $@)
	$(CC) $(OWN_CFLAGS) -c $< -o $@

$(O)/milan-ctrld: $(CTRLD_OBJS) $(FW_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread -lrt

$(O)/milan-mediad: $(MEDIAD_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lasound -lpthread -lm -lrt

$(O)/milan-dp: $(DP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lrt

# the gPTP judge stands alone: it needs neither the firmware nor the daemons
$(O)/milan-gptp-watch: src/gptp_watch.c
	@mkdir -p $(O)
	$(CC) $(OWN_CFLAGS) $(LDFLAGS) -o $@ $<

# config/ is what the board's end-station config generates at the pinned
# milan-fpga (tools/gen-config.sh); the generators need the tree's
# protocol-processor and gptp-processor (tools/fetch-milan-fpga.sh --with-submodules)
check-config:
	tools/gen-config.sh --check $(MILAN_FPGA)

check: all check-config $(O)/test_media $(O)/test_gptp
	$(O)/test_media
	$(O)/test_gptp
	tests/run-netns.sh $(O)/milan-ctrld $(O)/milan-dp config/entity.conf
	tests/run-netns-aecp.sh $(O)/milan-ctrld $(O)/milan-dp config/entity.conf
	tests/run-netns-talker.sh $(O)
	tests/run-netns-listener.sh $(O)
	tests/run-netns-nvm.sh $(O)

$(O)/test_gptp: tests/test_gptp.c src/gptp_time.c src/gptp_shm.c src/gptp_time.h src/gptp_shm.h
	@mkdir -p $(O)
	$(CC) $(OWN_CFLAGS) -o $@ tests/test_gptp.c src/gptp_time.c src/gptp_shm.c -lrt

$(O)/test_media: tests/test_media.c src/aaf.c src/servo.c src/aaf.h src/servo.h
	@mkdir -p $(O)
	$(CC) $(OWN_CFLAGS) -o $@ tests/test_media.c src/aaf.c src/servo.c -lm

install: all
	install -D -m 0755 $(O)/milan-ctrld $(DESTDIR)$(PREFIX)/sbin/milan-ctrld
	install -D -m 0755 $(O)/milan-mediad $(DESTDIR)$(PREFIX)/sbin/milan-mediad
	install -D -m 0755 $(O)/milan-dp $(DESTDIR)$(PREFIX)/sbin/milan-dp
	install -D -m 0755 $(O)/milan-gptp-watch $(DESTDIR)$(PREFIX)/bin/milan-gptp-watch

clean:
	rm -rf $(O)

.PHONY: all check check-config install clean
