#!/bin/bash
set -e

echo "=== Charon Build Environment Setup ==="
echo "This will build the cross-compilation toolchain and dependencies."
echo "This may take 30-60 minutes on first run."
echo ""

# Clean PATH - remove Windows paths that contain spaces (WSL issue)
# Keep only Linux native paths
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/lib/ccache"
echo "Cleaned PATH to avoid buildroot issues with spaces"

# Check if we're in the right directory
if [ ! -d "plutosdr-fw" ]; then
    echo "Error: plutosdr-fw directory not found!"
    echo "Make sure you're running this from the charon root directory."
    exit 1
fi

# Apply Charon configurations to plutosdr-fw (if not already done)
if [ ! -f "plutosdr-fw/.charon_configs_applied" ]; then
    echo "Applying Charon configurations to plutosdr-fw..."
    #cp -fr changes_to_plutosdr_fw_configs_rel_to_v28/* plutosdr-fw/
    touch plutosdr-fw/.charon_configs_applied
fi

cd plutosdr-fw

# Create dummy LICENSE file if needed
touch buildroot/board/pluto/msd/LICENSE.html

# Apply buildroot patches for static library builds
echo "Applying buildroot patches for static library support..."
cd buildroot
if ! git apply --check --reverse ../../buildroot_static_libs.patch 2>/dev/null; then
    echo "Applying buildroot static library patch..."
    git apply ../../buildroot_static_libs.patch
else
    echo "Buildroot patch already applied"
fi
cd ..

# Configure buildroot for Pluto
echo "Configuring buildroot..."
make -C buildroot ARCH=arm zynq_pluto_defconfig -j 8

# Build the cross-compilation toolchain
echo "Building toolchain (this takes time on first run)..."
make -C buildroot toolchain  -j 8

# Build required libraries
echo "Building libiio..."
make -C buildroot libiio -j 8

echo "Building libad9361-iio..."
make -C buildroot libad9361-iio -j 8

# Copy libad9361 static library to sysroot (buildroot doesn't install it by default)
echo "Installing libad9361.a to sysroot..."
cp buildroot/output/build/libad9361-iio-*/libad9361.a buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/lib/ 2>/dev/null || true

echo "Building libini..."
make -C buildroot libini -j 8

echo "Building zlib..."
make -C buildroot zlib -j 8

echo "Building fftw..."
make -C buildroot fftw-double -j 8
make -C buildroot fftw-single -j 8

# Set up environment variables
export PATH=$(pwd)/buildroot/output/host/bin:$(pwd)/buildroot/output/host/sbin:${PATH}
export CROSS_COMPILE="arm-linux-gnueabihf-"
export SYSROOT=$(pwd)/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/

echo "Verifying toolchain..."
which ${CROSS_COMPILE}gcc || { echo "Toolchain not in PATH!"; exit 1; }

cd ..

# Build libtuntap
echo "Building libtuntap..."
cd third_party/libtuntap
if [ ! -f ".built" ]; then
    sed -i 's|/usr/include/||g' CMakeLists.txt 2>/dev/null || true
    sed -i 's|/usr/local/include||g' CMakeLists.txt 2>/dev/null || true
    mkdir -p build
    cd build
    cmake .. -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc -DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++
    make
    cp lib/*.a ../
    cp tuntap-export.h ../ 2>/dev/null || true
    cd ..
    touch .built
else
    echo "libtuntap already built (remove .built file to rebuild)"
fi
cd ../..

# Build libfec
echo "Building libfec..."
cd third_party/libfec
if [ ! -f ".built" ]; then
    make clean || true
    make
    cp fec.h ../../plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/
    cp *.a ../../plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/lib/
    mkdir -p ../../plutosdr-fw/buildroot/output/target/usr/lib/
    cp *.a ../../plutosdr-fw/buildroot/output/target/usr/lib/
    touch .built
else
    echo "libfec already built (remove .built file to rebuild)"
fi
cd ../..

# Build liquid-dsp
echo "Building liquid-dsp..."
cd plutosdr-fw
make -C buildroot liquid-dsp -j 8
cd ..

echo ""
echo "=== Build environment setup complete! ==="
echo ""
echo "You can now build charon with: make"
echo ""
echo "Note: The toolchain and libraries are cached in plutosdr-fw/buildroot/output/"
echo "      Subsequent builds will be much faster."
