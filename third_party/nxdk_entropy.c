/* Weak entropy for mbed TLS on Xbox (no OS RNG). Demo only — not for production. */
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/entropy.h"

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    static uint32_t state = 0xC0FFEE42u;
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525u + 1013904223u;
        output[i] = (unsigned char)((state >> 16) ^ (state >> 8));
    }
    *olen = len;
    return 0;
}
