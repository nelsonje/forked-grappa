#include <stdlib.h>
#include "xmt-ops.h"

// prototype
void xmtcompat_initialize_rand(void);

int64_t xmtcompat_rand_initialized = 0;

void xmtcompat_initialize_rand(void) {
  srand48(894893uL); /* constant picked by hitting the keyboard */
  xmtcompat_rand_initialized = 1;
}

void prand(int64_t n, double * v) {
  int64_t i;
  extern int64_t xmtcompat_rand_initialized;
  extern void xmtcompat_initialize_rand(void);
  if (!xmtcompat_rand_initialized) xmtcompat_initialize_rand();
  for (i = 0; i < n; ++i) {
    v[i] = drand48();
  }
}

uint64_t MTA_BIT_LEFT_ZEROS(uint64_t y) {
  uint64_t x = (uint64_t) y;
  uint64_t out = 0;
#if INT_MAX == 9223372036854775807L
  if (!x) return 64;
  if (!(x & 0xFFFFFFFF00000000)) { out += 32; x <<= 32; }
  if (!(x & 0xFFFF0000FFFF0000)) { out += 16; x <<= 16; }
  if (!(x & 0xFF00FF00FF00FF00)) { out += 8; x <<= 8; }
  if (!(x & 0xF0F0F0F0F0F0F0F0)) { out += 4; x <<= 4; }
  if (!(x & 0xCCCCCCCCCCCCCCCC)) { out += 2; x <<= 2; }
  if (!(x & 0xAAAAAAAAAAAAAAAA)) out += 1;
#elif INT_MAX == 2147483647
  if (!x) return 32;
  if (!(x & 0xFFFF0000)) { out += 16; x <<= 16; }
  if (!(x & 0xFF00FF00)) { out += 8; x <<= 8; }
  if (!(x & 0xF0F0F0F0)) { out += 4; x <<= 4; }
  if (!(x & 0xCCCCCCCC)) { out += 2; x <<= 2; }
  if (!(x & 0xAAAAAAAA)) out += 1;
#else
#error "Unsupported int64_t size."
#endif
  return out;
}
