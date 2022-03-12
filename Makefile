NAME ?= charon
SUFFIX ?= .c
DIRS ?= .

ARCH=arm

FLAGS ?= -O3 -std=c99 -mfloat-abi=hard -I./plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/usr/include/\
         --sysroot=./plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/\
        -I./third_party/libtuntap/

SYSROOT ?= ./plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/

LDFLAGS ?= --sysroot=./plutosdr-fw/gcc-arm-8.2-2018.08-x86_64-arm-linux-gnueabihf/arm-linux-gnueabihf/libc\
           -L ./plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/\
           -L ./plutosdr-fw/gcc-arm-8.2-2018.08-x86_64-arm-linux-gnueabihf/arm-linux-gnueabihf/libc\
           -L ./plutosdr-fw/gcc-arm-8.2-2018.08-x86_64-arm-linux-gnueabihf/arm-linux-gnueabihf/libc/lib\
           -L ./plutosdr-fw/gcc-arm-8.2-2018.08-x86_64-arm-linux-gnueabihf/arm-linux-gnueabihf/libc/usr/lib \
           -L ./third_party/libfec\
           -L ./third_party/libtuntap\
           -L$(SYSROOT) -L$(SYSROOT)lib -L$(SYSROOT)usr -L$(SYSROOT)usr/lib \
           -lliquid -liio -lad9361 -lc -lm -lfftw3 -lini -lusb-1.0 -lserialport -lavahi-client -lavahi-common -lxml2 -lz -ldbus-1 -lfec -ltuntap

PLATFORM := $(shell uname -s)

-include Make.config


OUT_DIR := .build
SRC := $(foreach dir, $(DIRS), $(wildcard $(dir)/*$(SUFFIX)))
OBJ_ := $(SRC:$(SUFFIX)=.o)
OBJ := $(addprefix $(OUT_DIR)/,$(OBJ_))
DEPS := $(OBJ:.o=.d)
SHARED_SUFFIX := dll
STATIC_SUFFIX := lib

ifeq "$(PLATFORM)" "Linux"
    SHARED_SUFFIX := so
    STATIC_SUFFIX := a
endif

ifeq "$(LIBRARY)" "shared"
    OUT=lib$(NAME).$(SHARED_SUFFIX)
    LDFLAGS += -shared
else ifeq "$(LIBRARY)" "static"
    OUT=lib$(NAME).$(STATIC_SUFFIX)
else
    OUT=$(NAME)
endif

ifeq "$(SUFFIX)" ".cpp"
    COMPILER := $(CXX)
else ifeq "$(SUFFIX)" ".c"
    COMPILER := $(CROSS_COMPILE)gcc
endif

.SUFFIXES:
.PHONY: clean

$(OUT): $(OBJ)
ifeq "$(LIBRARY)" "static"
	@$(AR) rcs $@ $^
else
	@$(COMPILER) $^ $(LDFLAGS) -o $@
endif

$(OUT_DIR)/%.o: %$(SUFFIX)
	@mkdir -p $(dir $@)
	@echo $(COMPILER) $(CXXFLAGS) $(FLAGS)
	@$(COMPILER) $(CXXFLAGS) $(FLAGS) -MMD -MP -fPIC -c $< -o $@

clean:
	@$(RM) -r $(OUT) $(OUT_DIR)

-include: $(DEPS)
