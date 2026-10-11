# Host (x86/ARM Linux) build of the Charon RF channel simulator.
#
#   charon_sim  - charon with pluto.c linked against a fake libiio/libad9361
#                 (iio_zmq.c) that talks to rfchan over ZMQ
#   rfchan      - the real-time RF channel / impairment simulator
#
# Needs: libliquid-dev libfftw3-dev libzmq3-dev libiio-dev libad9361-dev
# (libiio/libad9361 for headers only; nothing links against them).

CC      ?= gcc
OUT     := build
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu99 -DCHARON_SIM -I.. -I../third_party/libtuntap -Iinclude -MMD -MP
WARN    := -Wall -Wno-unused-result -Wno-pointer-sign -Wno-unused-variable \
           -Wno-unused-but-set-variable -Wno-format-overflow -Wno-stringop-truncation \
           -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-discarded-qualifiers \
           -Wno-implicit-function-declaration -Wno-format-truncation -Wno-unused-function \
           -Wno-unused-label -Wno-sizeof-pointer-memaccess -Wno-maybe-uninitialized
LDLIBS  := -lliquid -lfftw3f -lzmq -lm

CHARON_SRC := ../charon.c ../config.c ../crc.c ../cw_tone.c ../ofdm_rx.c ../ofdm_tx.c \
              ../tap_device.c ../tcp_subs.c ../timers.c ../util.c ../pluto.c
SIM_SRC    := iio_zmq.c sim_traffic.c
TUNTAP_SRC := ../third_party/libtuntap/tuntap.c ../third_party/libtuntap/tuntap-unix.c \
              ../third_party/libtuntap/tuntap-unix-linux.c ../third_party/libtuntap/tuntap_log.c

CHARON_OBJ := $(addprefix $(OUT)/charon/,$(notdir $(CHARON_SRC:.c=.o))) \
              $(addprefix $(OUT)/sim/,$(SIM_SRC:.c=.o)) \
              $(addprefix $(OUT)/tuntap/,$(notdir $(TUNTAP_SRC:.c=.o)))
RFCHAN_OBJ := $(OUT)/sim/rfchan.o $(OUT)/sim/channel_model.o

all: $(OUT)/charon_sim $(OUT)/rfchan

$(OUT)/charon_sim: $(CHARON_OBJ)
	$(CC) -o $@ $^ $(LDLIBS)

$(OUT)/rfchan: $(RFCHAN_OBJ)
	$(CC) -o $@ $^ -lzmq -lm

$(OUT)/charon/%.o: ../%.c | $(OUT)/charon
	$(CC) $(CFLAGS) $(WARN) -c -o $@ $<

$(OUT)/tuntap/%.o: ../third_party/libtuntap/%.c | $(OUT)/tuntap
	$(CC) $(CFLAGS) -DLinux -D_GNU_SOURCE -w -c -o $@ $<

$(OUT)/sim/%.o: %.c | $(OUT)/sim
	$(CC) $(CFLAGS) -Wall -Wextra -c -o $@ $<

$(OUT)/charon $(OUT)/sim $(OUT)/tuntap:
	mkdir -p $@

clean:
	rm -rf $(OUT)

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)

.PHONY: all clean
