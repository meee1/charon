NAME = charon
SUFFIX ?= .c
DIRS ?= .

ARCH=arm

CROSS_COMPILE=arm-linux-gnueabihf-

SYSROOT ?=  ./plutosdr-fw/buildroot/output/host/arm-buildroot-linux-gnueabihf/sysroot/


FLAGS ?= -O3 -std=gnu99 -mfloat-abi=hard -ggdb -I$(SYSROOT)usr/include/\
         --sysroot=$(SYSROOT)\
        -I./third_party/libtuntap/\
        -D_TIME_BITS=32 -fno-builtin-strtol

# Static library paths
LIBLIQUID_STATIC = $(SYSROOT)usr/lib/libliquid.a
LIBFFTW3F_STATIC = $(SYSROOT)usr/lib/libfftw3f.a

LDFLAGS ?= -ggdb --sysroot=$(SYSROOT)\
           -L ./plutosdr-fw/buildroot/output/target/usr/lib\
           -L ./third_party/libfec\
           -L ./third_party/libtuntap\
           -L$(SYSROOT) -L$(SYSROOT)lib -L$(SYSROOT)usr -L$(SYSROOT)usr/lib \
           -Wl,--wrap=__isoc23_strtol \
           -Wl,--wrap=__isoc23_strtoll \
           -Wl,--wrap=__isoc23_strtoul \
           -Wl,--wrap=__isoc23_strtoull \
           -Wl,-Bstatic $(LIBLIQUID_STATIC) $(LIBFFTW3F_STATIC) \
           -Wl,-Bdynamic \
           -lc -lm -lfftw3 -liio -lad9361 -lini -lusb-1.0 -lserialport -lavahi-client -lavahi-common -lxml2 -lz -ldbus-1 \
           -Wl,-Bstatic -lfec -ltuntap \
           -Wl,-Bdynamic

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
	@echo $(COMPILER) $^ $(LDFLAGS) -o $@
	@$(COMPILER) $^ $(LDFLAGS) -o $@
endif

$(OUT_DIR)/%.o: %$(SUFFIX)
	@mkdir -p $(dir $@)
	@echo $(COMPILER) $(CXXFLAGS) $(FLAGS) -MMD -MP -fPIC -c $< -o $@
	@$(COMPILER) $(CXXFLAGS) $(FLAGS) -MMD -MP -fPIC -c $< -o $@

clean:
	@$(RM) -r $(OUT) $(OUT_DIR)

-include: $(DEPS)
