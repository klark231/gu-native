// A .zip read on a desktop (unzip.h). Deflate (RFC 1951) is decoded here: canonical Huffman codes looked up by their
// next 10 bits when that short, the longer ones searched by length, into a 64 KiB ring that holds the 32 KiB a match
// reaches back and the bytes waiting to be written.
#ifndef __ANDROID__
#include "unzip.h"

#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ CRC-32 (the zip's: 0xEDB88320, reflected)
static uint32_t crc_table[256];
static SDL_InitState crc_state;

static void crc_setup(void) {
    if (!SDL_ShouldInit(&crc_state)) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
        crc_table[i] = c;
    }
    SDL_SetInitialized(&crc_state, true);
}

static uint32_t crc_add(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ crc >> 8;
    return ~crc;
}

// ------------------------------------------------------------------ the directory
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static bool read_at(SDL_IOStream *io, uint64_t at, void *buf, size_t n) {
    return at <= (uint64_t)INT64_MAX && SDL_SeekIO(io, (Sint64)at, SDL_IO_SEEK_SET) == (Sint64)at &&
           SDL_ReadIO(io, buf, n) == n;
}

// The DOS code page (437) a name is in without the UTF-8 flag (bit 11): its upper half
static const uint16_t CP437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

// A name in UTF-8 ('\' made '/'): as stored when it is UTF-8 already, else from code page 437. NULL: a zero inside.
static char *utf8_name(const uint8_t *s, size_t n, bool utf8) {
    if (memchr(s, 0, n)) return NULL;
    char *out = malloc(n * 3 + 1);
    size_t k = 0;
    for (size_t i = 0; out && i < n; i++) {
        unsigned c = s[i], u;
        if (utf8 || c < 0x80) {
            out[k++] = (char)(c == '\\' ? '/' : c);
            continue;
        }
        if ((u = CP437[c - 0x80]) < 0x800) out[k++] = (char)(0xC0 | u >> 6), out[k++] = (char)(0x80 | (u & 0x3F));
        else out[k++] = (char)(0xE0 | u >> 12), out[k++] = (char)(0x80 | (u >> 6 & 0x3F)), out[k++] = (char)(0x80 | (u & 0x3F));
    }
    if (out) out[k] = 0;
    return out;
}

void unzip_close(Unzip *z) {
    for (int i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
    if (z->io) SDL_CloseIO(z->io);
    memset(z, 0, sizeof *z);
}

UnzipResult unzip_open(Unzip *z, const char *path) {
    memset(z, 0, sizeof *z);
    crc_setup();
    if (!(z->io = SDL_IOFromFile(path, "rb"))) return UNZIP_READ;
    Sint64 size = SDL_GetIOSize(z->io);
    if (size < 22) return size < 0 ? UNZIP_READ : UNZIP_NOT_ZIP;
    // The end of central directory record: 22 bytes and a comment of up to 64 KiB, the last thing in the file
    uint32_t tail = (uint32_t)(size < 22 + 0xFFFF ? size : 22 + 0xFFFF);
    uint8_t *buf = malloc(tail);
    if (!buf || !read_at(z->io, (uint64_t)size - tail, buf, tail)) {
        free(buf);
        return UNZIP_READ;
    }
    int64_t found = -1;
    for (int64_t i = (int64_t)tail - 22; i >= 0; i--)
        if (le32(buf + i) == 0x06054b50) {
            int64_t over = i + 22 + le16(buf + i + 20);
            if (over == (int64_t)tail) {  // (its comment reaches the end: the real one)
                found = i;
                break;
            }
            if (over < (int64_t)tail && found < 0) found = i;  // (bytes after it: kept unless a better one shows)
        }
    if (found < 0) {
        free(buf);
        return UNZIP_NOT_ZIP;
    }
    const uint8_t *e = buf + found;
    uint64_t end = (uint64_t)size - tail + (uint64_t)found;  // (where the directory must end, at the latest)
    uint16_t disk = le16(e + 4), cd_disk = le16(e + 6);
    uint64_t count = le16(e + 10), cd_size = le32(e + 12), cd_at = le32(e + 16);
    free(buf);
    if ((disk && disk != 0xFFFF) || (cd_disk && cd_disk != 0xFFFF)) return UNZIP_NOT_ZIP;  // (split in parts)
    if (count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_at == 0xFFFFFFFF) {  // ZIP64: its locator just before
        uint8_t loc[20], rec[56];
        if (end < 20 || !read_at(z->io, end - 20, loc, 20) || le32(loc) != 0x07064b50 ||
            !read_at(z->io, le64(loc + 8), rec, 56) || le32(rec) != 0x06064b50)
            return UNZIP_NOT_ZIP;
        count = le64(rec + 32), cd_size = le64(rec + 40), cd_at = le64(rec + 48);
        end = le64(loc + 8);
    }
    if (cd_at > end || cd_size > end - cd_at || cd_size > (256u << 20) || count > 1000000 || count * 46 > cd_size)
        return UNZIP_NOT_ZIP;
    uint8_t *cd = malloc(cd_size ? (size_t)cd_size : 1);
    z->entries = calloc(count ? (size_t)count : 1, sizeof *z->entries);
    if (!cd || !z->entries || !read_at(z->io, cd_at, cd, (size_t)cd_size)) {
        free(cd);
        return UNZIP_READ;
    }
    UnzipResult r = UNZIP_OK;
    const uint8_t *p = cd, *stop = cd + cd_size;
    for (uint64_t k = 0; k < count; k++) {
        if (stop - p < 46 || le32(p) != 0x02014b50) {
            r = UNZIP_NOT_ZIP;
            break;
        }
        UnzipEntry *u = &z->entries[z->count];
        u->flags = le16(p + 8), u->method = le16(p + 10), u->crc = le32(p + 16);
        u->packed = le32(p + 20), u->size = le32(p + 24), u->header = le32(p + 42);
        uint32_t nlen = le16(p + 28), xlen = le16(p + 30), clen = le16(p + 32), attr = le32(p + 38);
        const uint8_t *name = p + 46, *extra = name + nlen, *uname = NULL;
        size_t ulen = 0;
        if ((size_t)(stop - name) < (size_t)nlen + xlen + clen) {
            r = UNZIP_NOT_ZIP;
            break;
        }
        for (const uint8_t *x = extra; x + 4 <= extra + xlen;) {
            uint16_t id = le16(x), n = le16(x + 2);
            const uint8_t *d = x + 4, *de = d + n;
            if (de > extra + xlen) break;
            if (id == 0x0001) {  // ZIP64: the 64-bit values of those stored as all ones, in this order
                if (u->size == 0xFFFFFFFF && d + 8 <= de) u->size = le64(d), d += 8;
                if (u->packed == 0xFFFFFFFF && d + 8 <= de) u->packed = le64(d), d += 8;
                if (u->header == 0xFFFFFFFF && d + 8 <= de) u->header = le64(d);
            } else if (id == 0x7075 && n >= 5 && d[0] == 1 && le32(d + 1) == crc_add(0, name, nlen)) {
                uname = d + 5, ulen = n - 5u;  // (Info-ZIP's Unicode path: the UTF-8 name of a DOS-named entry)
            }
            x = de;
        }
        u->name = uname ? utf8_name(uname, ulen, true) : utf8_name(name, nlen, u->flags & 0x800);
        if (!u->name) {
            r = UNZIP_NOT_ZIP;
            break;
        }
        size_t l = strlen(u->name);
        u->dir = (l && u->name[l - 1] == '/') || ((attr & 0x10) && !u->size);  // (or DOS's folder attribute)
        z->count++;
        p = extra + xlen + clen;
    }
    free(cd);
    return r;
}

// ------------------------------------------------------------------ Deflate
#define FAST 10         // code bits looked up at once
#define RING 0x10000u   // the output ring (a power of two)

typedef struct {
    uint16_t fast[1 << FAST];   // by the next FAST bits: length << 9 | symbol of a code that short; 0, a longer one
    uint16_t first[16];         // per length: its first code
    uint16_t first_at[16];      // ... the index of its symbol in sym
    int32_t limit[16];          // ... the code after its last one, left-aligned in 16 bits
    uint8_t len[288];           // by code order: each code's length and symbol
    uint16_t sym[288];
} Huff;

typedef struct {
    SDL_IOStream *in, *out;
    uint64_t left;              // compressed bytes not read yet
    uint8_t buf[0x10000];
    uint32_t at, have;
    int padding;                // zero bytes given past the end (a stream cut short reads them)
    uint64_t bits;              // read and not used yet, the next one at bit 0
    int nbits;
    uint8_t ring[RING];
    uint64_t pos, written, size;  // bytes out, written to the file, and the entry's size (no more than that)
    uint32_t crc;
    bool write_failed;
    void (*progress)(void *, uint64_t);
    void *ud;
    Huff lit, dist;
} Inflate;

static const uint16_t LEN_BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static unsigned next_byte(Inflate *s) {
    if (s->at == s->have) {
        if (!s->left) {
            s->padding++;
            return 0;
        }
        uint32_t n = s->left < sizeof s->buf ? (uint32_t)s->left : (uint32_t)sizeof s->buf;
        if (SDL_ReadIO(s->in, s->buf, n) != n) {  // (the file can't be read further: as a stream cut short)
            s->left = 0, s->padding = 1 << 20;
            return 0;
        }
        s->at = 0, s->have = n, s->left -= n;
    }
    return s->buf[s->at++];
}

static void need(Inflate *s, int n) {
    while (s->nbits < n) s->bits |= (uint64_t)next_byte(s) << s->nbits, s->nbits += 8;
}

static unsigned bits(Inflate *s, int n) {
    need(s, n);
    unsigned v = (unsigned)(s->bits & ((1ull << n) - 1));
    s->bits >>= n, s->nbits -= n;
    return v;
}

static unsigned rev16(unsigned x) {
    x = (x & 0xAAAA) >> 1 | (x & 0x5555) << 1;
    x = (x & 0xCCCC) >> 2 | (x & 0x3333) << 2;
    x = (x & 0xF0F0) >> 4 | (x & 0x0F0F) << 4;
    return (x & 0xFF00) >> 8 | (x & 0x00FF) << 8;
}

// The canonical code for the lengths of n symbols (0: not used); false when the lengths over-subscribe it
static bool build(Huff *h, const uint8_t *lens, int n) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    memset(h->fast, 0, sizeof h->fast);
    memset(h->len, 0, sizeof h->len);
    int code = 0, at = 0;
    for (int l = 1; l < 16; l++) {
        next[l] = code;
        h->first[l] = (uint16_t)code;
        h->first_at[l] = (uint16_t)at;
        code += count[l];
        if (code > 1 << l) return false;
        h->limit[l] = code << (16 - l);
        code <<= 1;
        at += count[l];
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) continue;
        int c = next[l]++, k = c - h->first[l] + h->first_at[l];
        h->len[k] = (uint8_t)l, h->sym[k] = (uint16_t)i;
        if (l <= FAST)  // (the bits come in reversed: the code's first bit is the lowest of the table's index)
            for (unsigned j = rev16((unsigned)c) >> (16 - l); j < 1u << FAST; j += 1u << l) h->fast[j] = (uint16_t)(l << 9 | i);
    }
    return true;
}

// The next symbol in code h; -1: no code there
static int decode(Inflate *s, const Huff *h) {
    if (s->padding > 64) return -1;
    need(s, 16);
    unsigned v = h->fast[s->bits & ((1u << FAST) - 1)];
    if (v) {
        s->bits >>= v >> 9, s->nbits -= (int)(v >> 9);
        return (int)(v & 511);
    }
    unsigned k = rev16((unsigned)s->bits & 0xFFFF);
    for (int l = FAST + 1; l < 16; l++)
        if ((int32_t)k < h->limit[l]) {
            unsigned i = (k >> (16 - l)) - h->first[l] + h->first_at[l];
            if (i >= 288 || h->len[i] != l) return -1;
            s->bits >>= l, s->nbits -= l;
            return h->sym[i];
        }
    return -1;
}

static bool flush(Inflate *s) {
    while (s->written < s->pos) {
        uint32_t at = (uint32_t)(s->written & (RING - 1));
        uint64_t n = s->pos - s->written;
        if (n > RING - at) n = RING - at;
        s->crc = crc_add(s->crc, s->ring + at, (size_t)n);
        if (SDL_WriteIO(s->out, s->ring + at, (size_t)n) != n) {
            s->write_failed = true;
            return false;
        }
        s->written += n;
        if (s->progress) s->progress(s->ud, n);
    }
    return true;
}

static bool put(Inflate *s, unsigned byte) {
    if (s->pos == s->size) return false;  // (more than its size)
    s->ring[s->pos++ & (RING - 1)] = (uint8_t)byte;
    return s->pos - s->written < RING / 2 || flush(s);
}

static bool copy(Inflate *s, unsigned dist, unsigned len) {
    if (dist > s->pos || len > s->size - s->pos) return false;
    for (uint64_t from = s->pos - dist; len--; from++) s->ring[s->pos++ & (RING - 1)] = s->ring[from & (RING - 1)];
    return s->pos - s->written < RING / 2 || flush(s);
}

static bool fixed(Inflate *s) {
    uint8_t l[288];
    for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    if (!build(&s->lit, l, 288)) return false;
    for (int i = 0; i < 30; i++) l[i] = 5;
    return build(&s->dist, l, 30);
}

static bool dynamic(Inflate *s) {
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned nlit = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (nlit > 286 || ndist > 30) return false;
    uint8_t l[286 + 30] = {0};
    for (unsigned i = 0; i < ncode; i++) l[ORDER[i]] = (uint8_t)bits(s, 3);
    Huff lengths;
    if (!build(&lengths, l, 19)) return false;
    memset(l, 0, sizeof l);
    for (unsigned n = 0; n < nlit + ndist;) {
        int c = decode(s, &lengths);
        if (c < 0) return false;
        if (c < 16) {
            l[n++] = (uint8_t)c;
            continue;
        }
        unsigned repeat, value = 0;
        if (c == 16) {  // the previous length again
            if (!n) return false;
            value = l[n - 1], repeat = 3 + bits(s, 2);
        } else {
            repeat = c == 17 ? 3 + bits(s, 3) : 11 + bits(s, 7);
        }
        if (n + repeat > nlit + ndist) return false;
        while (repeat--) l[n++] = (uint8_t)value;
    }
    return l[256] && build(&s->lit, l, (int)nlit) && build(&s->dist, l + nlit, (int)ndist);  // (256 ends a block)
}

static bool inflate_all(Inflate *s) {
    unsigned last;
    do {
        last = bits(s, 1);
        unsigned type = bits(s, 2);
        if (type == 0) {  // stored: from the next byte, its length and that inverted, then the bytes
            s->bits >>= s->nbits & 7, s->nbits -= s->nbits & 7;
            unsigned len = bits(s, 16);
            if (bits(s, 16) != (~len & 0xFFFF) || s->padding > 8) return false;
            while (len--)
                if (s->padding > 8 || !put(s, bits(s, 8))) return false;
            continue;
        }
        if (type == 3 || !(type == 1 ? fixed(s) : dynamic(s))) return false;
        for (;;) {
            int c = decode(s, &s->lit);
            if (c < 256) {
                if (c < 0 || !put(s, (unsigned)c)) return false;
                continue;
            }
            if (c == 256) break;
            if ((c -= 257) >= 29) return false;
            unsigned len = LEN_BASE[c] + bits(s, LEN_EXTRA[c]);
            int d = decode(s, &s->dist);
            if (d < 0 || d >= 30 || !copy(s, DIST_BASE[d] + bits(s, DIST_EXTRA[d]), len)) return false;
        }
    } while (!last);
    return s->padding <= 8 && flush(s);
}

UnzipResult unzip_extract(Unzip *z, int i, const char *to, void (*progress)(void *ud, uint64_t n), void *ud) {
    const UnzipEntry *e = &z->entries[i];
    if (e->flags & 1) return UNZIP_ENCRYPTED;
    if (e->method != 0 && e->method != 8) return UNZIP_METHOD;
    uint8_t h[30];
    if (!read_at(z->io, e->header, h, sizeof h) || le32(h) != 0x04034b50 || (e->method == 0 && e->packed != e->size))
        return UNZIP_DAMAGED;
    uint64_t data = e->header + 30 + le16(h + 26) + le16(h + 28);  // (the local header's own name and extra lengths)
    if (data > (uint64_t)INT64_MAX || SDL_SeekIO(z->io, (Sint64)data, SDL_IO_SEEK_SET) != (Sint64)data) return UNZIP_DAMAGED;
    Inflate *s = calloc(1, sizeof *s);
    if (!s) return SDL_OutOfMemory(), UNZIP_WRITE;
    if (!(s->out = SDL_IOFromFile(to, "wb"))) {
        free(s);
        return UNZIP_WRITE;
    }
    s->in = z->io, s->left = e->packed, s->size = e->size, s->progress = progress, s->ud = ud;
    UnzipResult r = UNZIP_OK;
    if (e->method == 8) {
        if (!inflate_all(s)) r = s->write_failed ? UNZIP_WRITE : UNZIP_DAMAGED;
    } else {  // stored as it is
        while (r == UNZIP_OK && s->left) {
            size_t n = s->left < sizeof s->buf ? (size_t)s->left : sizeof s->buf;
            if (SDL_ReadIO(s->in, s->buf, n) != n) r = UNZIP_DAMAGED;
            else if (SDL_WriteIO(s->out, s->buf, n) != n) r = UNZIP_WRITE;
            else {
                s->crc = crc_add(s->crc, s->buf, n);
                s->left -= n, s->pos += n;
                if (progress) progress(ud, n);
            }
        }
    }
    if (r == UNZIP_OK && (s->pos != e->size || s->crc != e->crc)) r = UNZIP_DAMAGED;
    if (!SDL_CloseIO(s->out) && r == UNZIP_OK) r = UNZIP_WRITE;
    free(s);
    if (r != UNZIP_OK) SDL_RemovePath(to);
    return r;
}
#endif
