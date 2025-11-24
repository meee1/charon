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

# Add additional buildroot packages to defconfig
echo "Adding additional packages to buildroot config..."
echo "BR2_PACKAGE_BRIDGE_UTILS=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_LIQUID_DSP_FAST=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_FFTW_SINGLE=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_FFTW_USE_NEON=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_STRACE=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_FFTW_FAST=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_BATMAN_ADV=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_BATMAN_ADV_DEBUG=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_BATMAN_ADV_BATMAN_V=y" >> buildroot/configs/zynq_pluto_defconfig

echo "BR2_PACKAGE_VALGRIND=y" >> buildroot/configs/zynq_pluto_defconfig
echo "BR2_PACKAGE_VALGRIND_CALLGRIND=y" >> buildroot/configs/zynq_pluto_defconfig

# Fix liquid-dsp for Cortex-A9 (PlutoSDR has Zynq-7000 with Cortex-A9, not A7)
echo "Patching liquid-dsp makefile for Cortex-A9..."
sed -i 's#LIQUID_DSP_CFLAGS = $(TARGET_CFLAGS)#LIQUID_DSP_CFLAGS = $(TARGET_CFLAGS)\
define LIQUID_DSP_FIX_CORTEX_A9\
    $(SED) '"'"'s/-mcpu=cortex-a7/-mcpu=cortex-a9/g'"'"' $(@D)/makefile\
    $(SED) '"'"'s/-mfpu=neon-vfpv4/-mfpu=neon/g'"'"' $(@D)/makefile\
endef\
LIQUID_DSP_POST_CONFIGURE_HOOKS += LIQUID_DSP_FIX_CORTEX_A9#g' buildroot/package/liquid-dsp/liquid-dsp.mk

# Update libiio to newer commit
echo "Updating libiio version..."
sed -i 's/38483f31be391af66b35542f733e569febe13d3a/a0eca0d/g' buildroot/package/libiio/libiio.mk
echo 'sha256 3b743ead3675af5f7812a0f79da719ad18e4f398ba8a861f0c2aa4ede1d0964b libiio-a0eca0d-br1.tar.gz' >> buildroot/package/libiio/libiio.hash

# Update ad936x_ref_cal hash
sed -i 's/26aedd8021fa939ab2f53e55904d869207265242fef7ad86aa4673e219b7cbef/4814915de63d975807e918df82bb86021d0e78839e8cc4116a36476d0b33180c/g' buildroot/package/ad936x_ref_cal/ad936x_ref_cal.hash

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

echo "Building ad936x_ref_cal..."
make -C buildroot ad936x_ref_cal -j 8

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

# Create git tag for version tracking
cd ..
git tag v0.38 || true
cd plutosdr-fw

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
    sed -i 's\/usr/include/\\g' CMakeLists.txt 2>/dev/null || true
    sed -i 's\/usr/local/include\\g' CMakeLists.txt 2>/dev/null || true
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

# Add Linux kernel config options for batman-adv mesh networking
echo "Adding Linux kernel config options..."
cd plutosdr-fw
echo "CONFIG_TUN=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BRIDGE=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BATMAN_ADV=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BATMAN_ADV_BLA=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BATMAN_ADV_DAT=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BATMAN_ADV_MCAST=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BATMAN_ADV_BATMAN_V=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_BATMAN_ADV_DEBUG=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_MODULES=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_MODULE_UNLOAD=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_MODULE_FORCE_UNLOAD=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_MODVERSIONS=y" >> linux/arch/arm/configs/zynq_pluto_defconfig
echo "CONFIG_SYSCTL_SYSCALL=y" >> linux/arch/arm/configs/zynq_pluto_defconfig

# Add busybox config options
echo "Adding busybox config options..."
echo "CONFIG_TUNCTL=y" >> buildroot/board/pluto/busybox-1.25.0.config
echo "CONFIG_BRCTL=y" >> buildroot/board/pluto/busybox-1.25.0.config
echo "CONFIG_TASKSET=y" >> buildroot/board/pluto/busybox-1.25.0.config

# Build batctl (batman-adv control tool)
echo "Building batctl..."
make -C buildroot batctl -j 8

cd ..

echo ""
echo "=== Build environment setup complete! ==="
echo ""
echo "You can now build charon with: make"
echo ""
echo "Note: The toolchain and libraries are cached in plutosdr-fw/buildroot/output/"
echo "      Subsequent builds will be much faster."
echo ""
echo "To build the full PlutoSDR firmware image:"
echo "  1. Build charon: make"
echo "  2. Copy to buildroot: cp charon plutosdr-fw/buildroot/output/target/usr/bin/"
echo "  3. Build firmware: cd plutosdr-fw && make"
echo "  4. Flash firmware: plutosdr-fw/build/pluto.frm"
