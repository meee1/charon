#!/bin/bash
# Test script for Charon OFDM examples
# Run this after building the examples to verify they work

set -e

echo "=================================="
echo "Charon OFDM Examples Test Suite"
echo "=================================="
echo ""

# Check if examples are built
if [ ! -f "./ofdm_loopback_example" ]; then
    echo "Error: Examples not built. Run 'make' first."
    exit 1
fi

# Test 1: Loopback example
echo "Test 1: OFDM Loopback"
echo "---------------------"
if ./ofdm_loopback_example; then
    echo "✓ Loopback test PASSED"
else
    echo "✗ Loopback test FAILED"
    exit 1
fi

echo ""
echo "Test 2: File Transfer (small file)"
echo "-----------------------------------"

# Create test file
TEST_MSG="The quick brown fox jumps over the lazy dog. OFDM transmission test with Charon PHY layer!"
echo "$TEST_MSG" > test_small.txt

if ./ofdm_file_transfer test_small.txt received_small.txt > /dev/null 2>&1; then
    if diff test_small.txt received_small.txt > /dev/null 2>&1; then
        echo "✓ Small file transfer PASSED"
    else
        echo "✗ Small file transfer FAILED (file mismatch)"
        exit 1
    fi
else
    echo "✗ Small file transfer FAILED (program error)"
    exit 1
fi

echo ""
echo "Test 3: File Transfer (larger file)"
echo "------------------------------------"

# Create a larger test file (10KB)
dd if=/dev/urandom of=test_large.bin bs=1024 count=10 > /dev/null 2>&1

if ./ofdm_file_transfer test_large.bin received_large.bin > /dev/null 2>&1; then
    if diff test_large.bin received_large.bin > /dev/null 2>&1; then
        echo "✓ Large file transfer PASSED"
    else
        echo "✗ Large file transfer FAILED (file mismatch)"
        exit 1
    fi
else
    echo "✗ Large file transfer FAILED (program error)"
    exit 1
fi

# Cleanup
rm -f test_small.txt received_small.txt
rm -f test_large.bin received_large.bin

echo ""
echo "Test 4: PSS Sync (LTE-style with multiple frequency offset hypotheses)"
echo "-----------------------------------------------------------------------"
if ./pss_sync_example; then
    echo "✓ PSS sync test PASSED"
else
    echo "✗ PSS sync test FAILED"
    exit 1
fi

echo ""
echo "=================================="
echo "✓ All tests PASSED!"
echo "=================================="
echo ""
echo "Examples are working correctly."
echo "Try running them individually:"
echo "  ./ofdm_loopback_example"
echo "  ./ofdm_file_transfer myfile.txt output.txt"
echo "  ./pss_sync_example"
echo ""
