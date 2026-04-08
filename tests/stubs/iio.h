// Minimal stub for <iio.h> — allows ofdm_tx.c / ofdm_rx.c to compile
// without the real libiio-dev package.  Neither module calls IIO functions
// directly; only pluto.c does.
#ifndef STUB_IIO_H
#define STUB_IIO_H
struct iio_context;
#endif
