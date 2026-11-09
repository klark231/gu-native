// Reading a .zip on a desktop (runtime/unzip.c, for Add a mod in runtime/pc_actions.c): its entries from the central
// directory (ZIP64 too), each one's data stored or compressed with Deflate, checked against its size and CRC-32. Paths
// are UTF-8 and go through SDL's file functions.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

typedef struct {
    char *name;         // UTF-8 ('\' read as '/'; a name in the old DOS code page converted)
    bool dir;           // a folder: no data
    uint64_t size;      // uncompressed
    uint64_t packed;    // as stored
    uint64_t header;    // its local header's offset
    uint32_t crc;
    uint16_t method;    // 0 stored, 8 Deflate: the two unzip_extract reads
    uint16_t flags;     // bit 0: encrypted
} UnzipEntry;

typedef struct {
    SDL_IOStream *io;
    int count;
    UnzipEntry *entries;
} Unzip;

typedef enum {
    UNZIP_OK,
    UNZIP_READ,       // the file can't be read (SDL_GetError says why)
    UNZIP_NOT_ZIP,    // not a .zip, or cut short
    UNZIP_DAMAGED,    // its data doesn't match its directory or its CRC
    UNZIP_ENCRYPTED,  // an entry has a password
    UNZIP_METHOD,     // an entry is compressed some other way than Deflate
    UNZIP_WRITE,      // the file written failed (SDL_GetError says why)
} UnzipResult;

UnzipResult unzip_open(Unzip *z, const char *path);
void unzip_close(Unzip *z);  // (after any unzip_open, whatever it answered)
// Entry i (not a folder) into the file `to`, created or replaced; on a failure it is removed again. progress(ud, n):
// n more bytes were written (NULL: none).
UnzipResult unzip_extract(Unzip *z, int i, const char *to, void (*progress)(void *ud, uint64_t n), void *ud);
