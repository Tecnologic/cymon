// canard_pio.c — PlatformIO/ESP-IDF wrapper for canard.c
//
// Problem: ESP-IDF's platform_include/assert.h (included by canard.c via
//          <assert.h>) unconditionally includes <stdlib.h>, which in
//          Espressif's picolibc declares `long int random(void)`.
//          This conflicts with libcanard's internal `static uint64_t random(...)`.
//
// Fix: include <stdlib.h> here first, with _GNU_SOURCE and _BSD_SOURCE
//      temporarily undefined so that random() is NOT declared.  The stdlib.h
//      header guard then prevents the double-inclusion that would re-declare
//      random() when assert.h later pulls it in.

#undef _GNU_SOURCE
#undef _BSD_SOURCE
#include <stdlib.h>

#include "canard.c"
