#ifndef JIPC_MBEDTLS_USER_CONFIG_H
#define JIPC_MBEDTLS_USER_CONFIG_H

/* The OEM Linux 3.10 kernel has no getrandom(). MbedTLS's /dev/random
 * fallback blocks during seeding, including the separate PSA RNG used by TLS.
 * Use the kernel CSPRNG also used for this program's generated credentials.
 * On newer kernels MbedTLS still prefers getrandom(). */
#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/urandom"

/* HTTPS has multiple workers; PSA/global library state needs its own locks in
 * addition to the application's mutex around its per-server CTR_DRBG. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

#endif
