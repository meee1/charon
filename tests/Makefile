# Makefile for Charon unit tests
# All tests are self-contained — each test file #includes the .c under test
# directly so we can exercise static functions without a separate library.

CC = gcc
CFLAGS = -Wall -Wextra -O0 -g -std=gnu99 -I..
LDFLAGS = -lm

# OFDM tests link against real liquid-dsp and fftw3
OFDM_CFLAGS = $(CFLAGS) -Istubs
OFDM_LDFLAGS = -lliquid -lfftw3f -lm

TESTS = test_crc test_tcp_subs test_timers test_cw_tone test_cw_tone_channel test_cw_cfo_detection test_ofdm

all: $(TESTS)

test_crc: test_crc.c test_harness.h ../crc.c ../crc.h
	$(CC) $(CFLAGS) -o $@ test_crc.c $(LDFLAGS)

test_tcp_subs: test_tcp_subs.c test_harness.h ../tcp_subs.c ../ethernet.h
	$(CC) $(CFLAGS) -o $@ test_tcp_subs.c $(LDFLAGS)

test_timers: test_timers.c test_harness.h ../timers.c ../timers.h
	$(CC) $(CFLAGS) -o $@ test_timers.c $(LDFLAGS)

test_cw_tone: test_cw_tone.c test_harness.h ../cw_tone.c ../cw_tone.h ../ofdm_conf.h
	$(CC) $(CFLAGS) -o $@ test_cw_tone.c $(LDFLAGS)

test_cw_tone_channel: test_cw_tone_channel.c test_harness.h ../cw_tone.c ../cw_tone.h ../ofdm_conf.h
	$(CC) $(CFLAGS) -o $@ test_cw_tone_channel.c $(LDFLAGS)

test_cw_cfo_detection: test_cw_cfo_detection.c test_harness.h ../cw_tone.c ../cw_tone.h ../ofdm_conf.h
	$(CC) $(CFLAGS) -o $@ test_cw_cfo_detection.c $(LDFLAGS)

# OFDM test: compile each source as a separate object to avoid
# header re-inclusion conflicts (auto-generated headers lack guards).
OFDM_OBJS = ofdm_build/test_ofdm.o ofdm_build/ofdm_test_shims.o \
            ofdm_build/ofdm_tx.o ofdm_build/ofdm_rx.o \
            ofdm_build/cw_tone.o ofdm_build/timers.o

ofdm_build:
	mkdir -p ofdm_build

ofdm_build/test_ofdm.o: test_ofdm.c test_harness.h ../ofdm_conf.h ../ofdm_tx.h ../ofdm_rx.h ../ethernet.h ../tap_device.h | ofdm_build
	$(CC) $(OFDM_CFLAGS) -c -o $@ test_ofdm.c

ofdm_build/ofdm_test_shims.o: ofdm_test_shims.c | ofdm_build
	$(CC) $(OFDM_CFLAGS) -c -o $@ ofdm_test_shims.c

ofdm_build/ofdm_tx.o: ../ofdm_tx.c ../ofdm_conf.h ../ofdm_tx.h ../ethernet.h | ofdm_build
	$(CC) $(OFDM_CFLAGS) -c -o $@ ../ofdm_tx.c

ofdm_build/ofdm_rx.o: ../ofdm_rx.c ../ofdm_conf.h ../ofdm_rx.h ../ofdm.h ../ethernet.h | ofdm_build
	$(CC) $(OFDM_CFLAGS) -c -o $@ ../ofdm_rx.c

ofdm_build/cw_tone.o: ../cw_tone.c ../cw_tone.h ../ofdm_conf.h | ofdm_build
	$(CC) $(OFDM_CFLAGS) -c -o $@ ../cw_tone.c

ofdm_build/timers.o: ../timers.c ../timers.h | ofdm_build
	$(CC) $(OFDM_CFLAGS) -c -o $@ ../timers.c

test_ofdm: $(OFDM_OBJS)
	$(CC) -o $@ $(OFDM_OBJS) $(OFDM_LDFLAGS)

check: $(TESTS)
	@echo "==================================="
	@echo "  Charon Unit Test Suite"
	@echo "==================================="
	@FAIL=0; \
	for t in $(TESTS); do \
		echo ""; \
		./$$t; \
		if [ $$? -ne 0 ]; then \
			echo "*** $$t FAILED ***"; \
			FAIL=1; \
		fi; \
	done; \
	echo ""; \
	echo "==================================="; \
	if [ $$FAIL -eq 0 ]; then \
		echo "  All test suites PASSED"; \
	else \
		echo "  Some tests FAILED"; \
		exit 1; \
	fi; \
	echo "==================================="

clean:
	rm -f $(TESTS)
	rm -rf ofdm_build

.PHONY: all check clean
