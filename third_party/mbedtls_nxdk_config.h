/* mbed TLS overrides for NXDK (Original Xbox): no /dev/urandom — use weak hardware entropy hook. */
#ifndef MBEDTLS_NXDK_CONFIG_H
#define MBEDTLS_NXDK_CONFIG_H

/* nxdk-cc sets MSVC compatibility; avoid Windows <sal.h> in platform_util.h */
#ifndef MBEDTLS_CHECK_RETURN
#define MBEDTLS_CHECK_RETURN
#endif

#include "mbedtls/mbedtls_config.h"

#undef MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#undef MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* Use lwIP BSD sockets in https_client.c; mbed TLS net_sockets (POSIX/Win32) is not used. */
#undef MBEDTLS_NET_C

/* No POSIX <sys/types.h> / filesystem in nxdk PDCLib. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

#define MBEDTLS_PLATFORM_MS_TIME_ALT

#endif /* MBEDTLS_NXDK_CONFIG_H */
