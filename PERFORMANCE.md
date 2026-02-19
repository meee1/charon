# Performance Improvement Recommendations

Analysis of Charon's hot paths on the Cortex-A9 (Zynq-7000) target, ordered by
expected impact.

---

## 1. Replace `gettimeofday()` with `clock_gettime(CLOCK_MONOTONIC)`

**Files:** `timers.c`, `timers.h`
**Severity:** Critical — estimated 700K–1.4M unnecessary syscalls/sec

Every call to `timer_elapsed_usec()` invokes `gettimeofday()`, which is a
syscall on Linux/ARM. The main loop calls it multiple times per iteration
(ack_timer, symbol_timer, AGC timers). At 1.4 MHz sample rate this dominates
CPU time.

`clock_gettime(CLOCK_MONOTONIC)` is typically vDSO-accelerated (no kernel
entry), immune to NTP adjustments, and provides nanosecond resolution.

```c
// timers.c — before
void timer_reset(timer_obj *o) {
  gettimeofday(&o->start, NULL);
}
long long timer_elapsed_usec(timer_obj *o) {
  gettimeofday(&o->end, NULL);
  return (o->end.tv_sec*1e6+o->end.tv_usec) - (o->start.tv_sec*1e6+o->start.tv_usec);
}

// timers.c — after
void timer_reset(timer_obj *o) {
  clock_gettime(CLOCK_MONOTONIC, &o->start);
}
long long timer_elapsed_usec(timer_obj *o) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - o->start.tv_sec) * 1000000LL
       + (now.tv_nsec - o->start.tv_nsec) / 1000;
}
```

The timer_obj struct would change from two `struct timeval` fields to two
`struct timespec` fields (only `start` is needed; `end` becomes a local).

Also eliminates the floating-point multiply (`*1e6`) in the elapsed calculation.

---

## 2. Cache batman route table — eliminate `popen("batctl o")` per TX

**File:** `util.c:129` (`is_batman_route`), called from `charon.c`
**Severity:** Critical — spawns `/bin/sh` + `batctl` subprocess per unicast TX

Every non-broadcast transmission calls `is_batman_route()`, which runs
`popen("batctl o")`. This forks a shell, execs batctl, reads its stdout, and
parses text. On the Cortex-A9, process creation costs ~1–5 ms.

**Recommendation:** Read `/sys/kernel/debug/batman_adv/bat0/originators` directly
(no subprocess), and cache the result with a staleness timeout (e.g. 1–2
seconds). The batman OGM interval is 10 seconds by default, so route changes
are infrequent.

```c
// Sketch: cached route lookup
static uint8_t cached_route[6];
static char    cached_mac[18];
static timer_obj *route_cache_timer;
#define ROUTE_CACHE_TTL_USEC 2000000  // 2 seconds

int is_batman_route(const char *org_dest_mac_a, uint8_t *dst_route) {
  if (strncmp(org_dest_mac_a, cached_mac, 17) == 0 &&
      timer_elapsed_usec(route_cache_timer) < ROUTE_CACHE_TTL_USEC) {
    memcpy(dst_route, cached_route, 6);
    return 1;
  }

  // Read /sys/kernel/debug/batman_adv/bat0/originators directly
  FILE *fp = fopen("/sys/kernel/debug/batman_adv/bat0/originators", "r");
  // ... parse, update cached_route/cached_mac, timer_reset() ...
}
```

---

## 3. Remove double buffer copy in TAP device read

**File:** `tap_device.c:156–179`
**Severity:** High — extra `memcpy()` of up to 2342 bytes per received frame

`read_tap_dev()` reads into a stack-local `_buffer[2342]`, then immediately
copies into the caller's `buffer`. The intermediate buffer is unnecessary.

```c
// Before (tap_device.c:175-179)
nbytes = read(dev->tun_fd, _buffer, 2342);
if(nbytes<=0) return 0;
memcpy(buffer, _buffer, nbytes);

// After — read directly into output buffer
nbytes = read(dev->tun_fd, buffer, max_len);
if(nbytes<=0) return 0;
```

The original comment ("is this source of memory leak, try copy") suggests this
was a debugging workaround. The caller (`charon.c`) provides a 2342-byte
`tap_buffer`, so direct reads are safe.

---

## 4. Replace `pow()` with integer multiply for exponential backoff

**File:** `charon.c:307`
**Severity:** Medium — `pow()` is a heavy libm call for squaring an integer

```c
// Before
ack_timeout += (int) pow( (max_retrans-tx_retry), 2) * ...

// After
int backoff_exp = max_retrans - tx_retry;
ack_timeout += (backoff_exp * backoff_exp) * ...
```

`pow()` on Cortex-A9 without hardware double-precision takes ~100+ cycles for
what is a single integer multiply.

---

## 5. Batch IQ sample conversion with NEON intrinsics

**File:** `charon.c:362–372` (`do_process_iq16`)
**Severity:** High — called 1.4M times/sec, no vectorization

Currently processes one IQ sample at a time with scalar float division:

```c
void do_process_iq16(const int16_t i, const int16_t q) {
  IF = ((float) i)/32768.0f;
  QF = ((float) q)/32768.0f;
  sample = (float complex) (IF + _Complex_I * QF);
  bump_nco();
  do_ofdm_mix_down(sample, &sample);
  do_ofdm_rx(sample);
}
```

This function and `pluto_receive()` could be refactored to process samples in
blocks (e.g. 4 or 8 at a time) using NEON:

```c
#include <arm_neon.h>

// Convert 4 IQ pairs: int16 -> float, scale by 1/32768
void convert_iq_block(const int16_t *src, float *i_out, float *q_out) {
  int16x4_t vi = {src[0], src[2], src[4], src[6]};  // I samples
  int16x4_t vq = {src[1], src[3], src[5], src[7]};  // Q samples
  float32x4_t fi = vcvtq_f32_s32(vmovl_s16(vi));
  float32x4_t fq = vcvtq_f32_s32(vmovl_s16(vq));
  float32x4_t scale = vdupq_n_f32(1.0f/32768.0f);
  vst1q_f32(i_out, vmulq_f32(fi, scale));
  vst1q_f32(q_out, vmulq_f32(fq, scale));
}
```

This requires refactoring the per-sample callback chain (`bump_nco`,
`do_ofdm_mix_down`, `do_ofdm_rx`) to accept sample blocks, which is a
significant but high-value change. The NCO and mix-down stages in particular
can be vectorized.

Similarly, `pluto_transmit()` (`pluto.c:417–420`) does per-sample float-to-int16
conversion that could be NEON-vectorized:

```c
// Current: per-sample
((int16_t*)tx_p_dat)[0] = (int16_t)(creal(buffer[ii]) * 8192.0);
((int16_t*)tx_p_dat)[1] = (int16_t)(cimag(buffer[ii]) * 8192.0);
```

---

## 6. Use `FFTW_MEASURE` instead of `FFTW_ESTIMATE` for FFT planning

**File:** `ofdm.h:41`
**Severity:** Medium — suboptimal FFT execution for the fixed 128-point transform

```c
// Current
#define FFT_METHOD  FFTW_ESTIMATE

// Recommended
#define FFT_METHOD  FFTW_MEASURE
```

`FFTW_ESTIMATE` uses heuristics to pick an FFT algorithm. `FFTW_MEASURE`
benchmarks multiple algorithms at plan creation and picks the fastest. For a
fixed 128-point transform that runs millions of times, the one-time 100–200 ms
startup cost pays for itself immediately.

For even better results, generate a wisdom file once on-device and load it at
startup with `fftwf_import_wisdom_from_filename()`. This avoids the measurement
cost on subsequent boots.

---

## 7. Cache IIO channel pointers for AGC control

**File:** `pluto.c` (AGC functions)
**Severity:** Medium — repeated `iio_device_find_channel()` lookups

Functions like `pluto_get_in_gain()`, `pluto_set_in_gain()`,
`pluto_bump_agc_down()` call `iio_device_find_channel()` to locate the
hardware gain channel every time they're invoked. The AGC runs every 256 RX
iterations (fast path) plus every 1 second (slow path).

Cache the channel pointer once during initialization:

```c
static struct iio_channel *phy_voltage0_in = NULL;

void pluto_init(...) {
  // ... existing init ...
  phy_voltage0_in = iio_device_find_channel(phy, "voltage0", false);
}

long long pluto_get_in_gain(void) {
  long long val = 72;
  iio_channel_attr_read_longlong(phy_voltage0_in, "hardwaregain", &val);
  return val;
}
```

---

## 8. Optimize duplicate frame detection

**File:** `crc.c:75–83`
**Severity:** Low-Medium — O(256) linear scan per non-broadcast frame

```c
int is_dup(uint32_t pid) {
  for(i=0; i<MAX_DUPES; i++) {
    if(duplicates[i]==pid) return 1;
  }
  return 0;
}
```

Options, from simplest to most effective:

**A) Search backwards from insertion point (most recent first):**
Duplicates from retransmissions arrive close together in time, so searching
from `dup_idx` backwards gives O(1) average case:

```c
int is_dup(uint32_t pid) {
  int idx = dup_idx;
  for (int i = 0; i < MAX_DUPES; i++) {
    idx = (idx - 1) & (MAX_DUPES - 1);
    if (duplicates[idx] == pid) return 1;
    if (duplicates[idx] == 0) return 0;  // early termination
  }
  return 0;
}
```

**B) Small hash set:** Use a 256-slot hash table keyed on `pid & 0xFF` for
O(1) average lookup, with a parallel circular buffer for eviction.

---

## 9. Enable Link-Time Optimization (LTO)

**File:** `Makefile`
**Severity:** Medium — 5–15% throughput improvement potential

Add `-flto` to both `FLAGS` and `LDFLAGS`:

```makefile
FLAGS ?= -O2 -flto -std=gnu99 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard ...
LDFLAGS ?= -flto -O2 -ggdb --sysroot=$(SYSROOT) ...
```

LTO enables the compiler to optimize across translation unit boundaries —
inlining hot functions like `do_process_iq16()`, `timer_elapsed_usec()`, and
`bump_nco()` across files that currently prevent it.

Consider also adding `-ffast-math` for the signal processing code. OFDM
modulation/demodulation does not require strict IEEE 754 compliance, and this
flag enables additional floating-point optimizations (associativity,
reciprocal approximations, no NaN/Inf checks).

---

## 10. Reduce `timer_elapsed_usec()` calls in the main loop

**File:** `charon.c:289–333`
**Severity:** Medium — redundant time reads

The main loop calls `timer_elapsed_usec()` on multiple timers independently,
each fetching the current time separately. Read the time once per loop
iteration and compare against stored deadlines:

```c
// Before: multiple independent syscalls
while (ofdm_rx_state() != SEEKPLCP ||
       (tx_retry > 0 && timer_elapsed_usec(ack_timer) < ack_timeout)) { ... }
// ... later ...
if (timer_elapsed_usec(ack_timer) > ack_timeout &&
    timer_elapsed_usec(symbol_timer) > symbol_delay_timeout) { ... }
// ... later ...
if (n > 0 && tx_retry == 0 &&
    timer_elapsed_usec(symbol_timer) > (symbol_delay_timeout + ...)) { ... }

// After: single time read, reuse
struct timespec now;
clock_gettime(CLOCK_MONOTONIC, &now);
long long now_usec = now.tv_sec * 1000000LL + now.tv_nsec / 1000;

long long ack_elapsed  = now_usec - ack_timer->start_usec;
long long sym_elapsed  = now_usec - symbol_timer->start_usec;
// ... use ack_elapsed, sym_elapsed in all comparisons ...
```

---

## Summary — priority and effort matrix

| # | Change | Impact | Effort | Risk |
|---|--------|--------|--------|------|
| 1 | `clock_gettime` timers | High | Low | Low |
| 2 | Cache batman routes | High | Medium | Low |
| 3 | Remove TAP double copy | Medium | Trivial | Low |
| 4 | `pow()` → integer multiply | Low-Med | Trivial | None |
| 5 | NEON IQ conversion | High | High | Medium |
| 6 | FFTW_MEASURE | Medium | Trivial | Low |
| 7 | Cache IIO channels | Medium | Low | Low |
| 8 | Optimize dup detection | Low-Med | Low | Low |
| 9 | Enable LTO | Medium | Trivial | Low |
| 10 | Single time read per loop | Medium | Low | Low |

Items 1, 3, 4, 6, and 9 are low-effort changes that can be applied
independently with minimal risk. Items 2 and 7 require modest refactoring.
Item 5 (NEON vectorization) yields the highest per-sample improvement but
requires restructuring the sample processing pipeline from per-sample callbacks
to block processing.
