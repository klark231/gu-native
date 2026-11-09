//! The little crypto a 3DS beacon needs: AES-128 (encryption only, for CTR mode), SHA-1, MD5, and zlib's CRC-32.
//! None of it protects anything here: the beacon key the emulators use is all zeros and the hashes are checksums.
//! Small and dependency-free so the crate still builds where cargo can't fetch more.

// ---- AES-128

const SBOX: [u8; 256] = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
    0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
    0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
    0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
    0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
];

fn xtime(b: u8) -> u8 {
    (b << 1) ^ if b & 0x80 != 0 { 0x1b } else { 0 }
}

pub struct Aes128 {
    rk: [[u8; 16]; 11],
}

impl Aes128 {
    pub fn new(key: &[u8; 16]) -> Aes128 {
        let mut rk = [[0u8; 16]; 11];
        rk[0] = *key;
        let mut rcon = 1u8;
        for r in 1..11 {
            let p = rk[r - 1];
            let mut t = [SBOX[p[13] as usize], SBOX[p[14] as usize], SBOX[p[15] as usize], SBOX[p[12] as usize]];
            t[0] ^= rcon;
            rcon = xtime(rcon);
            let mut k = [0u8; 16];
            for i in 0..4 {
                k[i] = p[i] ^ t[i];
            }
            for i in 4..16 {
                k[i] = p[i] ^ k[i - 4];
            }
            rk[r] = k;
        }
        Aes128 { rk }
    }

    pub fn encrypt_block(&self, block: &mut [u8; 16]) {
        let s = block;
        for i in 0..16 {
            s[i] ^= self.rk[0][i];
        }
        for round in 1..11 {
            for b in s.iter_mut() {
                *b = SBOX[*b as usize];
            }
            // shift rows (the state is column-major: byte r + 4c)
            let t = *s;
            for c in 0..4 {
                for r in 0..4 {
                    s[r + 4 * c] = t[r + 4 * ((c + r) % 4)];
                }
            }
            if round != 10 {
                for c in 0..4 {
                    let a = [s[4 * c], s[4 * c + 1], s[4 * c + 2], s[4 * c + 3]];
                    let all = a[0] ^ a[1] ^ a[2] ^ a[3];
                    for r in 0..4 {
                        s[4 * c + r] = a[r] ^ all ^ xtime(a[r] ^ a[(r + 1) % 4]);
                    }
                }
            }
            for i in 0..16 {
                s[i] ^= self.rk[round][i];
            }
        }
    }

    /// AES-CTR (the counter is one big-endian 128-bit number, as Crypto++ counts): encrypts and decrypts alike.
    pub fn ctr(&self, counter: &[u8; 16], data: &mut [u8]) {
        let mut ctr = *counter;
        for chunk in data.chunks_mut(16) {
            let mut ks = ctr;
            self.encrypt_block(&mut ks);
            for (d, k) in chunk.iter_mut().zip(ks.iter()) {
                *d ^= k;
            }
            for i in (0..16).rev() {
                ctr[i] = ctr[i].wrapping_add(1);
                if ctr[i] != 0 {
                    break;
                }
            }
        }
    }
}

// ---- SHA-1

pub fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    for block in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    let mut out = [0u8; 20];
    for i in 0..5 {
        out[4 * i..4 * i + 4].copy_from_slice(&h[i].to_be_bytes());
    }
    out
}

// ---- MD5

pub fn md5(data: &[u8]) -> [u8; 16] {
    const S: [u32; 64] = [
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15,
        21,
    ];
    let k: Vec<u32> = (0..64).map(|i| ((i as f64 + 1.0).sin().abs() * 4294967296.0) as u32).collect();
    let mut h: [u32; 4] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_le_bytes());
    for block in msg.chunks(64) {
        let m: Vec<u32> =
            (0..16).map(|i| u32::from_le_bytes([block[4 * i], block[4 * i + 1], block[4 * i + 2], block[4 * i + 3]])).collect();
        let (mut a, mut b, mut c, mut d) = (h[0], h[1], h[2], h[3]);
        for i in 0..64 {
            let (f, g) = match i {
                0..=15 => ((b & c) | (!b & d), i),
                16..=31 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                32..=47 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let t = d;
            d = c;
            c = b;
            b = b.wrapping_add(a.wrapping_add(f).wrapping_add(k[i]).wrapping_add(m[g]).rotate_left(S[i]));
            a = t;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
    }
    let mut out = [0u8; 16];
    for i in 0..4 {
        out[4 * i..4 * i + 4].copy_from_slice(&h[i].to_le_bytes());
    }
    out
}

// ---- CRC-32 (zlib's)

fn crc_table() -> &'static [u32; 256] {
    static T: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    T.get_or_init(|| {
        let mut t = [0u32; 256];
        for i in 0..256u32 {
            let mut c = i;
            for _ in 0..8 {
                c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
            }
            t[i as usize] = c;
        }
        t
    })
}

pub fn crc32(data: &[u8]) -> u32 {
    let t = crc_table();
    let mut r = 0xFFFF_FFFFu32;
    for &b in data {
        r = (r >> 8) ^ t[((r ^ b as u32) & 0xFF) as usize];
    }
    r ^ 0xFFFF_FFFF
}

/// On the 3DS, Pia's address of a player is the CRC-32 of its friend code seed (8 bytes, little endian). This makes
/// a seed whose CRC-32 is `address`: "MHGU" and four bytes worked back from the result (each table entry's top byte
/// is unique). A gu-native player's seed is the one for its IP, so the 3DS sees it at the address MHGU uses.
pub fn seed_for_address(address: u32) -> u64 {
    let t = crc_table();
    let mut seed = [b'M', b'H', b'G', b'U', 0, 0, 0, 0];
    let mut state = crc32(&seed[..4]) ^ 0xFFFF_FFFF;
    let mut f = address ^ 0xFFFF_FFFF;
    let mut index = [0u8; 4];
    for k in (0..4).rev() {
        let j = (0..256).find(|&i| t[i] >> 24 == f >> 24).unwrap_or(0);
        index[k] = j as u8;
        f = (f ^ t[j]) << 8;
    }
    for k in 0..4 {
        let b = ((state ^ index[k] as u32) & 0xFF) as u8;
        seed[4 + k] = b;
        state = (state >> 8) ^ t[((state ^ b as u32) & 0xFF) as usize];
    }
    u64::from_le_bytes(seed)
}

/// The Pia address of a player with this friend code seed.
pub fn address_of_seed(seed: u64) -> u32 {
    crc32(&seed.to_le_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    #[test]
    fn aes_vectors() {
        let k: [u8; 16] = core::array::from_fn(|i| i as u8);
        let mut b: [u8; 16] = core::array::from_fn(|i| (i as u8) * 0x11);
        Aes128::new(&k).encrypt_block(&mut b);
        assert_eq!(hex(&b), "69c4e0d86a7b0430d8cdb78070b4c55a"); // FIPS-197 C.1
        let mut z = [0u8; 16];
        Aes128::new(&[0; 16]).encrypt_block(&mut z);
        assert_eq!(hex(&z), "66e94bd4ef8a2c3b884cfa59ca342b2e");
    }

    #[test]
    fn ctr_round_trip_and_carry() {
        let aes = Aes128::new(&[0; 16]);
        let mut ctr = [0u8; 16];
        ctr[15] = 0xFF;
        let mut d = vec![7u8; 40];
        aes.ctr(&ctr, &mut d);
        assert_ne!(d, vec![7u8; 40]);
        aes.ctr(&ctr, &mut d);
        assert_eq!(d, vec![7u8; 40]);
        // the second block's counter is ...0100 (the carry went into byte 14)
        let mut one = [0u8; 32];
        aes.ctr(&ctr, &mut one);
        let mut c2 = [0u8; 16];
        c2[14] = 1;
        aes.encrypt_block(&mut c2);
        assert_eq!(&one[16..], &c2[..]);
    }

    #[test]
    fn hashes() {
        assert_eq!(hex(&sha1(b"abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
        assert_eq!(hex(&sha1(&[0u8; 100])), "ed4a77d1b56a118938788fc53037759b6c501e3d");
        assert_eq!(hex(&md5(b"")), "d41d8cd98f00b204e9800998ecf8427e");
        assert_eq!(hex(&md5(b"The quick brown fox jumps over the lazy dog")), "9e107d9d372bb6826bd81d3542a419d6");
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn seeds_hit_their_address() {
        for a in [0xA9FE_0102u32, 0xA9FE_7F05, 0, 0xFFFF_FFFF, 0x1234_5678] {
            assert_eq!(address_of_seed(seed_for_address(a)), a);
        }
    }
}
