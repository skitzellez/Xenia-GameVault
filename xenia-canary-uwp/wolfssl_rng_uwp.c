#include "wolfssl_rng_uwp.h"

#include <windows.h>

#include <bcrypt.h>

int xe_uwp_random_seed(unsigned char* output, unsigned int size) {
  NTSTATUS status =
      BCryptGenRandom(NULL, output, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return status < 0 ? -1 : 0;
}
