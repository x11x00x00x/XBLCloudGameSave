/* Monotonic-ish ms clock for mbed TLS when MBEDTLS_PLATFORM_MS_TIME_ALT is set. */
#include <windows.h>

#include "mbedtls/platform_time.h"

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)GetTickCount();
}
