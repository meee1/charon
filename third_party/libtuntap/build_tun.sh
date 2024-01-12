rm *.o
arm-linux-gnueabihf-gcc  -c -O3 -std=c99 -I../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/ --sysroot=../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/ tuntap.c
arm-linux-gnueabihf-gcc  -c -O3 -std=c99 -I../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/ --sysroot=../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/ tuntap_log.c
arm-linux-gnueabihf-gcc  -c -O3 -std=c99 -I../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/ --sysroot=../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/ tuntap-unix.c
arm-linux-gnueabihf-gcc  -c -O3 -std=c99 -I../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/ --sysroot=../../../buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/ tuntap-unix-linux.c
arm-linux-gnueabihf-ar rcs libtuntap.a *.o
