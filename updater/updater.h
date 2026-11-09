// SHA-256 (updater/sha256.c, from MHGURecomp's updater): runtime/net/ldn.c hashes the hub's matchmaking key with it.
// (MHGURecomp's updater itself, which installs releases, is not part of this project.)
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t size);
void sha256_hex(Sha256 *s, char out[65]);
