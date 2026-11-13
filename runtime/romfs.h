// Virtual RomFS: the game's RomFS image, built in memory from a folder of loose files (the player's extracted
// romfs/) with mod folders layered on top. Only the image's tables live in memory; file data is read from the
// host files when the game reads that part of the image.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Builds the image from base_dir and the given overlay folders (later ones win; files they add are added).
bool romfs_open(const char *base_dir, const char *const *overlays, int noverlays);
uint64_t romfs_size(void);
// The game's image looks cut short (under 12.0 GB, or files of 0 bytes): its size in GB and how many empty files
bool romfs_incomplete(double *gb, int *empty);
// Reads image bytes [off, off+size) into dst. Returns the bytes read (short only past the end).
uint64_t romfs_read(uint8_t *dst, uint64_t off, uint64_t size);
// The RomFS path (e.g. "nativeNX/arc/...") of the file holding image offset off, or NULL.
const char *romfs_file_at(uint64_t off);
// The host file behind a RomFS path ("nativeNX/movie/movie00.mp4", a leading '/' allowed; a mod's copy if one
// replaces it), or NULL.
const char *romfs_host_path(const char *rpath);

// Another image, of a folder alone (a DLC's: services/dlc.c); NULL if it can't be read
typedef struct RomfsImage RomfsImage;
RomfsImage *romfs_image_open(const char *dir);
uint64_t romfs_image_size(const RomfsImage *im);
uint64_t romfs_image_read(const RomfsImage *im, uint8_t *dst, uint64_t off, uint64_t size);
