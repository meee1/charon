# Makefile for Charon unit tests
# All tests are self-contained — each test file #includes the .c under test
# directly so we can exercise static functions without a separate library.

CC = gcc
CFLAGS = -Wall -Wextra -O0 -g -std=gnu99 -I..
LDFLAGS = -lm

TESTS = test_crc test_tcp_subs test_timers test_cw_tone test_cw_tone_channel test_cw_cfo_detection

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

.PHONY: all check clean
