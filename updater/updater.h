// Shared header of the updater folder: only the SHA-256 part is built for Android (updater/sha256.c, used by runtime/net/ldn.c).
#ifndef UPDATER_H
#define UPDATER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint64_t len;  // bytes hashed so far
    uint8_t buf[64];
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t size);
void sha256_hex(Sha256 *s, char out[65]);  // finishes the hash: 64 hex digits + NUL

#ifdef __cplusplus
}
#endif

#endif
