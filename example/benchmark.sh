#!/bin/bash
# Performance benchmark for Charon OFDM examples
# Tests various parameters and measures throughput

echo "=========================================="
echo "Charon OFDM Performance Benchmark"
echo "=========================================="
echo ""

if [ ! -f "./ofdm_file_transfer" ]; then
    echo "Error: Build examples first with 'make'"
    exit 1
fi

# Create test files of various sizes
echo "Creating test files..."
dd if=/dev/urandom of=test_1k.bin bs=1024 count=1 2>/dev/null
dd if=/dev/urandom of=test_10k.bin bs=1024 count=10 2>/dev/null
dd if=/dev/urandom of=test_100k.bin bs=1024 count=100 2>/dev/null

echo ""
echo "File Size | Frames | Time (s) | Throughput | Success"
echo "----------|--------|----------|------------|--------"

# Benchmark function
run_benchmark() {
    local file=$1
    local size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file" 2>/dev/null)
    
    # Run transfer and capture output
    output=$(./ofdm_file_transfer "$file" output.bin 2>&1)
    
    # Extract statistics
    frames=$(echo "$output" | grep "Frames sent:" | awk '{print $3}')
    time=$(echo "$output" | grep "TX time:" | awk '{print $3}')
    rate=$(echo "$output" | grep "Data rate:" | awk '{print $3}')
    success=$(echo "$output" | grep "Success rate:" | awk '{print $3}')
    
    # Format size
    if [ $size -lt 1024 ]; then
        size_str="${size}B"
    else
        size_kb=$((size / 1024))
        size_str="${size_kb}KB"
    fi
    
    printf "%-9s | %-6s | %-8s | %-10s | %s\n" \
           "$size_str" "$frames" "$time" "${rate}kbps" "$success"
    
    rm -f output.bin
}

# Run benchmarks
run_benchmark test_1k.bin
run_benchmark test_10k.bin
run_benchmark test_100k.bin

# Cleanup
rm -f test_1k.bin test_10k.bin test_100k.bin output.bin

echo ""
echo "=========================================="
echo "Benchmark complete!"
echo ""
echo "Notes:"
echo "  - These are simulated transfers (no real RF)"
echo "  - Actual PlutoSDR performance may vary"
echo "  - Real-world throughput affected by:"
echo "    * Distance between nodes"
echo "    * RF interference"
echo "    * Multi-hop routing overhead"
echo "=========================================="
