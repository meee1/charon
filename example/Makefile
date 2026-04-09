# Makefile for Charon OFDM Examples
# Requires liquid-dsp library installed on the host system

CC = gcc
CFLAGS = -Wall -O2 -g
LDFLAGS = -lliquid -lm -lfftw3f

# Targets
TARGETS = ofdm_loopback_example ofdm_file_transfer ofdm_channel_test

all: $(TARGETS)

ofdm_loopback_example: ofdm_loopback_example.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"
	@echo "Run with: ./$@"

ofdm_file_transfer: ofdm_file_transfer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"
	@echo "Run with: ./$@ <input_file> [output_file]"

ofdm_channel_test: ofdm_channel_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $@"
	@echo "Run with: ./$@"

clean:
	rm -f $(TARGETS) *.o

install-deps:
	@echo "Installing liquid-dsp dependencies..."
	@echo "On Ubuntu/Debian: sudo apt-get install liquid-dsp libfftw3-dev"
	@echo "Or build from source: https://github.com/jgaeddert/liquid-dsp"

help:
	@echo "Charon OFDM Example Build System"
	@echo "================================="
	@echo ""
	@echo "Targets:"
	@echo "  all             - Build all examples (default)"
	@echo "  clean           - Remove built binaries"
	@echo "  install-deps    - Show dependency installation instructions"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  ofdm_loopback_example - OFDM TX/RX loopback test"
	@echo "  ofdm_file_transfer    - File transfer over OFDM simulation"

.PHONY: all clean install-deps help
