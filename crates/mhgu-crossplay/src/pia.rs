//! Pia, the network library of both games, between MHXX on the 3DS and MHGU: what has to change in a packet for
//! the other side to take it. The same conversions the AzaharPlus MHGU hub bridge makes (they are known to work
//! there), seen from this side:
//!  - addresses: a 3DS player's Pia address is the CRC-32 of its friend code seed; MHGU's are 169.254.x.n. Each UDS
//!    node n is 169.254.x.n here, and its 3DS address is swapped for that IP on the way in, and back on the way out.
//!    (A gu-native player's seed is made for its IP, so for those the two are the same and nothing changes.)
//!  - three system messages laid out differently: the connection request (MHGU's names the target's constant id),
//!    the connection response (MHGU's always has 4 player infos, UTF-8 names) and the session info (12 slots on the
//!    3DS, with the UDS node id; 8 in MHGU).
//! Game data (protocol 0x2000) passes untouched.

use std::collections::HashMap;

pub const PIA_PORT: u16 = 12345;
const MAGIC: [u8; 4] = [0x32, 0xAB, 0x98, 0x64];
// (MHGU's room data starts with this constant where the 3DS's has zeros)
pub const MHGU_MAGIC: [u8; 8] = [0xEF, 0xCD, 0xAB, 0x90, 0x78, 0x56, 0x34, 0x12];

const P_STATION: u16 = 0x0100;
const P_MESH: u16 = 0x0200;
const P_SESSION: u16 = 0x0300;
const P_GAME: u16 = 0x2000;
const PLAYER_INFO: usize = 187; // name[80] enc account[40] enc language key[64]
const CONN_REQ_3DS: usize = 78;
const CONN_REQ_MHGU: usize = 86;
const CONN_RESP_HEAD: usize = 44;
const SESSION_HEAD: usize = 32;
const SESSION_ENTRY: usize = 9;
const SLOTS_3DS: usize = 12;
const SLOTS_MHGU: usize = 8;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct Message {
    flags: u8,
    src: u8,
    dest: u32,
    cid: [u8; 4], // the sender's constant id: its address
    proto_type: u16,
    proto_port: u16,
    reserved: [u8; 4],
    payload: Vec<u8>,
}

fn be16(d: &[u8], o: usize) -> u16 {
    u16::from_be_bytes([d[o], d[o + 1]])
}
fn be32(d: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

fn parse(d: &[u8]) -> Option<([u8; 12], Vec<Message>)> {
    if d.len() < 12 || d[..4] != MAGIC || d[4] != 1 {
        return None; // (1: not encrypted)
    }
    let head: [u8; 12] = d[..12].try_into().unwrap();
    let mut out = Vec::new();
    let mut o = 12;
    while o < d.len() {
        if o + 20 > d.len() {
            return None;
        }
        let size = be16(d, o + 2) as usize;
        if o + 20 + size > d.len() {
            return None;
        }
        out.push(Message {
            flags: d[o],
            src: d[o + 1],
            dest: be32(d, o + 4),
            cid: d[o + 8..o + 12].try_into().unwrap(),
            proto_type: be16(d, o + 12),
            proto_port: be16(d, o + 14),
            reserved: d[o + 16..o + 20].try_into().unwrap(),
            payload: d[o + 20..o + 20 + size].to_vec(),
        });
        o += 20 + ((size + 3) & !3);
    }
    (o == d.len()).then_some((head, out))
}

fn build(head: &[u8; 12], msgs: &[Message]) -> Vec<u8> {
    let mut out = head.to_vec();
    for m in msgs {
        let o = out.len();
        out.resize(o + 20 + ((m.payload.len() + 3) & !3), 0);
        out[o] = m.flags;
        out[o + 1] = m.src;
        out[o + 2..o + 4].copy_from_slice(&(m.payload.len() as u16).to_be_bytes());
        out[o + 4..o + 8].copy_from_slice(&m.dest.to_be_bytes());
        out[o + 8..o + 12].copy_from_slice(&m.cid);
        out[o + 12..o + 14].copy_from_slice(&m.proto_type.to_be_bytes());
        out[o + 14..o + 16].copy_from_slice(&m.proto_port.to_be_bytes());
        out[o + 16..o + 20].copy_from_slice(&m.reserved);
        out[o + 20..o + 20 + m.payload.len()].copy_from_slice(&m.payload);
    }
    out
}

/// Every 4-byte run equal to one of the `from`s becomes its `to` (all at once: one swap never feeds another).
fn replace_all(p: &mut [u8], pairs: &[([u8; 4], [u8; 4])]) {
    let mut i = 0;
    while i + 4 <= p.len() {
        if let Some((_, to)) = pairs.iter().find(|(f, _)| p[i..i + 4] == *f) {
            p[i..i + 4].copy_from_slice(to);
            i += 4;
        } else {
            i += 1;
        }
    }
}

// a player name field between UTF-8 (encoding 1) and big-endian UTF-16 (encoding 2)
fn name_to_utf16(p: &mut [u8], at: usize, size: usize, enc_at: usize) {
    if p[enc_at] != 1 {
        return;
    }
    let f = &p[at..at + size];
    let n = f.iter().position(|&b| b == 0).unwrap_or(size);
    let w: Vec<u16> = String::from_utf8_lossy(&f[..n]).encode_utf16().take(size / 2 - 1).collect();
    p[at..at + size].fill(0);
    for (i, c) in w.iter().enumerate() {
        p[at + 2 * i..at + 2 * i + 2].copy_from_slice(&c.to_be_bytes());
    }
    p[enc_at] = 2;
}
fn name_to_utf8(p: &mut [u8], at: usize, size: usize, enc_at: usize) {
    if p[enc_at] != 2 {
        return;
    }
    let w: Vec<u16> =
        (0..size / 2).map(|i| be16(p, at + 2 * i)).take_while(|&c| c != 0).collect();
    let mut s = String::from_utf16_lossy(&w).into_bytes();
    while s.len() > size - 1 {
        s.pop(); // (may cut a character: as the bridge does, the game ends the string at the NUL)
    }
    p[at..at + size].fill(0);
    p[at..at + s.len()].copy_from_slice(&s);
    p[enc_at] = 1;
}

fn is_link_local(b: &[u8]) -> bool {
    b[0] == 0xA9 && b[1] == 0xFE
}

/// One network's translation state. `subnet` is 169.254.x.0; nodes are 169.254.x.n.
#[derive(Default)]
pub struct Translator {
    subnet: u32,
    addrs: HashMap<u16, [u8; 4]>,  // UDS node -> its 3DS Pia address, where it differs from its IP
    pids: HashMap<u32, [u8; 8]>,   // station IP -> its constant id (PID) as MHGU messages show it
    index_ips: HashMap<u8, u32>,   // Pia station index -> IP, from MHGU's session info
    pub translated: u64,
}

impl Translator {
    pub fn new(subnet: u32) -> Translator {
        Translator { subnet, ..Default::default() }
    }
    pub fn subnet(&self) -> u32 {
        self.subnet
    }
    fn ip_of(&self, node: u16) -> [u8; 4] {
        (self.subnet | node as u32).to_be_bytes()
    }
    /// The 3DS address of UDS node `node` (from its friend code seed).
    pub fn set_node(&mut self, node: u16, addr: u32) {
        let a = addr.to_be_bytes();
        if a == self.ip_of(node) {
            self.addrs.remove(&node);
        } else {
            self.addrs.insert(node, a);
        }
    }
    pub fn clear_nodes(&mut self) {
        self.addrs.clear();
    }
    fn to_ip_pairs(&self) -> Vec<([u8; 4], [u8; 4])> {
        self.addrs.iter().map(|(&n, &a)| (a, self.ip_of(n))).collect()
    }
    fn to_addr_pairs(&self) -> Vec<([u8; 4], [u8; 4])> {
        self.addrs.iter().map(|(&n, &a)| (self.ip_of(n), a)).collect()
    }
    fn swap(m: &mut Message, pairs: &[([u8; 4], [u8; 4])]) {
        if let Some((_, to)) = pairs.iter().find(|(f, _)| m.cid == *f) {
            m.cid = *to;
        }
        if m.proto_type != P_GAME {
            replace_all(&mut m.payload, pairs);
        }
    }
    fn pid_of(&self, ip: u32) -> [u8; 8] {
        if let Some(p) = self.pids.get(&ip) {
            return *p;
        }
        let mut p = [0u8; 8];
        p[..4].copy_from_slice(&ip.to_be_bytes());
        p[4..6].copy_from_slice(&PIA_PORT.to_be_bytes());
        p
    }

    // what MHGU's own messages say about its stations (the local game's, on their way out)
    fn learn(&mut self, m: &Message) {
        let p = &m.payload;
        // session info from the host: its variable id (le at 20); a PID is address, port, the id's low 16 bits
        if m.proto_type == P_SESSION && m.proto_port == PIA_PORT && p.len() >= 24 && p[0] == 1 && p[1] == 0x11
            && is_link_local(&m.cid)
        {
            let ip = u32::from_be_bytes(m.cid);
            let var = u32::from_le_bytes(p[20..24].try_into().unwrap());
            let mut pid = [0u8; 8];
            pid[..4].copy_from_slice(&m.cid);
            pid[4..6].copy_from_slice(&PIA_PORT.to_be_bytes());
            pid[6..8].copy_from_slice(&(var as u16).to_be_bytes());
            self.pids.insert(ip, pid);
            let mut o = SESSION_HEAD;
            while o + SESSION_ENTRY <= p.len() {
                if p[o + 8] != 0xFF && is_link_local(&p[o..]) {
                    self.index_ips.insert(p[o + 8], be32(p, o));
                }
                o += SESSION_ENTRY;
            }
            return;
        }
        if m.proto_type != P_STATION && m.proto_type != P_MESH {
            return;
        }
        // station locations: address, port, PID (= address, port, 2 bytes)
        let mut i = 0;
        while i + 14 <= p.len() {
            if is_link_local(&p[i..]) && p[i..i + 6] == p[i + 6..i + 12] {
                self.pids.insert(be32(p, i), p[i + 6..i + 14].try_into().unwrap());
                i += 14;
            } else {
                i += 1;
            }
        }
    }

    /// A packet from the room (3DS format) for the local MHGU. `dest_node`: its UDS destination (ours), or
    /// BROADCAST for everyone. Not a Pia packet: returned as it is.
    pub fn to_mhgu(&mut self, d: &[u8], dest_node: u16) -> Vec<u8> {
        let Some((head, mut msgs)) = parse(d) else { return d.to_vec() };
        let pairs = self.to_ip_pairs();
        for m in msgs.iter_mut() {
            Self::swap(m, &pairs);
            let p = &mut m.payload;
            if m.proto_type == P_STATION && m.proto_port == 0 && p.len() == CONN_REQ_3DS && p[0] == 1 {
                // connection request: MHGU's names the target's PID after its first 4 bytes. The target is the UDS
                // node it went to; failing that, the station index it names; failing that, the host.
                let mut target = if dest_node != crate::uds::BROADCAST_NODE { self.subnet | dest_node as u32 } else { 0 };
                for i in 0..32u8 {
                    if target == 0 && m.dest == 1u32 << i {
                        target = self.index_ips.get(&i).copied().unwrap_or(0);
                    }
                }
                let pid = self.pid_of(if target != 0 { target } else { self.subnet | 1 });
                p.splice(4..4, pid);
                self.translated += 1;
            } else if m.proto_type == P_STATION && m.proto_port == 0 && p.len() >= CONN_RESP_HEAD + 4 && p[0] == 2
                && p[1] == 0 && (p.len() - CONN_RESP_HEAD - 4) % PLAYER_INFO == 0
            {
                // connection response: MHGU's platform magic, always 4 player infos, UTF-8 names
                let infos = (p.len() - CONN_RESP_HEAD - 4) / PLAYER_INFO;
                if infos >= 4 {
                    continue;
                }
                p[3] = 4;
                if p[5..13].iter().all(|&b| b == 0) {
                    p[5..13].copy_from_slice(&MHGU_MAGIC);
                }
                let ack: Vec<u8> = p[p.len() - 4..].to_vec();
                p.truncate(p.len() - 4);
                for i in 0..infos {
                    let at = CONN_RESP_HEAD + i * PLAYER_INFO;
                    name_to_utf8(p, at, 80, at + 0x50);
                    name_to_utf8(p, at + 0x51, 40, at + 0x79);
                }
                p.resize(CONN_RESP_HEAD + 4 * PLAYER_INFO, 0);
                p.extend_from_slice(&ack);
                self.translated += 1;
            } else if m.proto_type == P_SESSION && m.proto_port == PIA_PORT
                && p.len() == SESSION_HEAD + SLOTS_3DS * SESSION_ENTRY + 1 && p[0] == 1 && p[1] == 0x11
            {
                // session info: 12 slots (with the UDS node id) -> 8 (node id 0)
                let mut q = p[..SESSION_HEAD].to_vec();
                for i in 0..SLOTS_MHGU {
                    let e = &p[SESSION_HEAD + i * SESSION_ENTRY..SESSION_HEAD + (i + 1) * SESSION_ENTRY];
                    q.extend_from_slice(e);
                    let n = q.len();
                    q[n - 3] = 0;
                    q[n - 2] = 0;
                }
                q.push(*p.last().unwrap());
                q[2] = (SLOTS_MHGU * SESSION_ENTRY + 1) as u8;
                *p = q;
                self.translated += 1;
            }
        }
        build(&head, &msgs)
    }

    /// A packet from the local MHGU (MHGU format) for the room.
    pub fn to_3ds(&mut self, d: &[u8]) -> Vec<u8> {
        let Some((head, mut msgs)) = parse(d) else { return d.to_vec() };
        let pairs = self.to_addr_pairs();
        for m in msgs.iter_mut() {
            self.learn(m);
            Self::swap(m, &pairs);
            let p = &mut m.payload;
            if m.proto_type == P_STATION && m.proto_port == 0 && p.len() == CONN_REQ_MHGU && p[0] == 1 {
                p.drain(4..12);
                self.translated += 1;
            } else if m.proto_type == P_STATION && m.proto_port == 0 && p.len() == CONN_RESP_HEAD + 4 * PLAYER_INFO + 4
                && p[0] == 2 && p[1] == 0
            {
                let infos = (p[43] as usize).clamp(1, 4);
                p[3] = 2;
                p[5..37].fill(0);
                let ack: Vec<u8> = p[p.len() - 4..].to_vec();
                p.truncate(CONN_RESP_HEAD + infos * PLAYER_INFO);
                for i in 0..infos {
                    let at = CONN_RESP_HEAD + i * PLAYER_INFO;
                    name_to_utf16(p, at, 80, at + 0x50);
                    name_to_utf16(p, at + 0x51, 40, at + 0x79);
                }
                p.extend_from_slice(&ack);
                self.translated += 1;
            } else if m.proto_type == P_SESSION && m.proto_port == PIA_PORT
                && p.len() == SESSION_HEAD + SLOTS_MHGU * SESSION_ENTRY + 1 && p[0] == 1 && p[1] == 0x11
            {
                // 8 slots -> 12, each with the UDS node the 3DS sends that station's packets to
                let mut q = p[..SESSION_HEAD].to_vec();
                for i in 0..SLOTS_3DS {
                    let mut e = [0u8, 0, 0, 0, 0, 0, 0, 0, 0xFF];
                    if i < SLOTS_MHGU {
                        e.copy_from_slice(&p[SESSION_HEAD + i * SESSION_ENTRY..SESSION_HEAD + (i + 1) * SESSION_ENTRY]);
                        if e[8] != 0xFF {
                            let node = self
                                .addrs
                                .iter()
                                .find(|(_, a)| e[..4] == a[..])
                                .map(|(&n, _)| n)
                                .unwrap_or(if is_link_local(&e) { e[3] as u16 } else { 0 });
                            e[6..8].copy_from_slice(&node.to_be_bytes());
                        }
                    }
                    q.extend_from_slice(&e);
                }
                q.push(*p.last().unwrap());
                q[2] = (SLOTS_3DS * SESSION_ENTRY + 1) as u8;
                *p = q;
                self.translated += 1;
            }
        }
        build(&head, &msgs)
    }
}

/// The room description: Pia's local-play application data on both sides (200 bytes), a few fields apart. The 3DS's
/// header has a session value at 12; MHGU's game data starts with MHGU_MAGIC and has 8 more bytes after the room's
/// text, so everything after it sits 8 bytes later.
pub fn uds_app_to_adv(app: &[u8]) -> Vec<u8> {
    if app.len() != 200 {
        return app.to_vec();
    }
    let mut out = vec![0u8; 200];
    out[..12].copy_from_slice(&app[..12]);
    if app[20..28].iter().all(|&b| b == 0) {
        out[20..28].copy_from_slice(&MHGU_MAGIC);
    } else {
        out[20..28].copy_from_slice(&app[20..28]);
    }
    out[28..76].copy_from_slice(&app[28..76]);
    out[84..200].copy_from_slice(&app[76..192]);
    out
}

pub fn adv_to_uds_app(adv: &[u8], session_param: u32) -> Vec<u8> {
    if adv.len() != 200 {
        return adv.to_vec();
    }
    let mut out = vec![0u8; 200];
    out[..12].copy_from_slice(&adv[..12]);
    out[12..16].copy_from_slice(&(if session_param != 0 { session_param } else { 1 }).to_le_bytes());
    if adv[20..28] != MHGU_MAGIC {
        out[20..28].copy_from_slice(&adv[20..28]);
    }
    out[28..76].copy_from_slice(&adv[28..76]);
    out[76..192].copy_from_slice(&adv[84..200]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn msg(proto_type: u16, proto_port: u16, cid: [u8; 4], payload: Vec<u8>) -> Message {
        Message { flags: 0, src: 1, dest: 0, cid, proto_type, proto_port, reserved: [0; 4], payload }
    }
    fn packet(msgs: &[Message]) -> Vec<u8> {
        let mut h = [0u8; 12];
        h[..4].copy_from_slice(&MAGIC);
        h[4] = 1;
        build(&h, msgs)
    }

    const SUB: u32 = 0xA9FE_2A00;

    #[test]
    fn parse_build_round_trip() {
        let p = packet(&[msg(P_GAME, 1, [1, 2, 3, 4], vec![9; 5]), msg(P_MESH, 0, [0; 4], vec![1; 8])]);
        let (h, m) = parse(&p).unwrap();
        assert_eq!(build(&h, &m), p);
        assert!(parse(&p[..p.len() - 1]).is_none());
    }

    #[test]
    fn addresses_swap_both_ways_but_not_in_game_data() {
        let mut t = Translator::new(SUB);
        t.set_node(1, 0xDEAD_BEEF); // a real 3DS host
        t.set_node(2, SUB | 2); // a gu-native player: same as its IP
        let host = 0xDEAD_BEEFu32.to_be_bytes();
        let host_ip = (SUB | 1).to_be_bytes();
        let mut pl = vec![0u8; 4];
        pl.extend_from_slice(&host);
        let p = packet(&[msg(P_MESH, 0, host, pl.clone()), msg(P_GAME, 0, host, pl.clone())]);
        let out = t.to_mhgu(&p, 2);
        let (_, m) = parse(&out).unwrap();
        assert_eq!(m[0].cid, host_ip);
        assert_eq!(&m[0].payload[4..8], &host_ip);
        assert_eq!(m[1].cid, host_ip);
        assert_eq!(m[1].payload, pl); // game data untouched
        let back = t.to_3ds(&out);
        let (_, m) = parse(&back).unwrap();
        assert_eq!(m[0].cid, host);
        assert_eq!(&m[0].payload[4..8], &host);
    }

    #[test]
    fn connection_request_gains_and_loses_the_target_pid() {
        let mut t = Translator::new(SUB);
        let mut req = vec![0u8; CONN_REQ_3DS];
        req[0] = 1;
        let p = packet(&[msg(P_STATION, 0, (SUB | 2).to_be_bytes(), req.clone())]);
        let out = t.to_mhgu(&p, 1);
        let (_, m) = parse(&out).unwrap();
        assert_eq!(m[0].payload.len(), CONN_REQ_MHGU);
        assert_eq!(&m[0].payload[4..8], &(SUB | 1).to_be_bytes());
        assert_eq!(&m[0].payload[8..10], &PIA_PORT.to_be_bytes());
        let back = t.to_3ds(&out);
        assert_eq!(parse(&back).unwrap().1[0].payload, req);
    }

    #[test]
    fn session_info_slots() {
        let mut t = Translator::new(SUB);
        t.set_node(1, 0x1111_1111);
        let mut s = vec![0u8; SESSION_HEAD + SLOTS_MHGU * SESSION_ENTRY + 1];
        s[0] = 1;
        s[1] = 0x11;
        for i in 0..SLOTS_MHGU {
            s[SESSION_HEAD + i * SESSION_ENTRY + 8] = 0xFF;
        }
        let e = SESSION_HEAD;
        s[e..e + 4].copy_from_slice(&(SUB | 1).to_be_bytes());
        s[e + 8] = 0;
        let e = SESSION_HEAD + SESSION_ENTRY;
        s[e..e + 4].copy_from_slice(&(SUB | 3).to_be_bytes());
        s[e + 8] = 1;
        let p = packet(&[msg(P_SESSION, PIA_PORT, (SUB | 3).to_be_bytes(), s)]);
        let out = t.to_3ds(&p);
        let (_, m) = parse(&out).unwrap();
        let q = &m[0].payload;
        assert_eq!(q.len(), SESSION_HEAD + SLOTS_3DS * SESSION_ENTRY + 1);
        assert_eq!(&q[SESSION_HEAD..SESSION_HEAD + 4], &0x1111_1111u32.to_be_bytes()); // the host's 3DS address
        assert_eq!(be16(q, SESSION_HEAD + 6), 1); // its node
        assert_eq!(be16(q, SESSION_HEAD + SESSION_ENTRY + 6), 3);
        assert_eq!(q[SESSION_HEAD + 11 * SESSION_ENTRY + 8], 0xFF);
        // and back to 8
        let again = t.to_mhgu(&out, 3);
        assert_eq!(parse(&again).unwrap().1[0].payload.len(), SESSION_HEAD + SLOTS_MHGU * SESSION_ENTRY + 1);
    }

    #[test]
    fn connection_response_names() {
        let mut t = Translator::new(SUB);
        // MHGU's: 4 infos, UTF-8 names, then 4 bytes
        let mut r = vec![0u8; CONN_RESP_HEAD + 4 * PLAYER_INFO + 4];
        r[0] = 2;
        r[43] = 1;
        r[CONN_RESP_HEAD..CONN_RESP_HEAD + 5].copy_from_slice(b"Kirin");
        r[CONN_RESP_HEAD + 0x50] = 1;
        let n = r.len();
        r[n - 4..].copy_from_slice(&[9, 8, 7, 6]);
        let p = packet(&[msg(P_STATION, 0, (SUB | 1).to_be_bytes(), r)]);
        let out = t.to_3ds(&p);
        let q = parse(&out).unwrap().1[0].payload.clone();
        assert_eq!(q.len(), CONN_RESP_HEAD + PLAYER_INFO + 4);
        assert_eq!(q[CONN_RESP_HEAD + 0x50], 2);
        assert_eq!(be16(&q, CONN_RESP_HEAD), 'K' as u16);
        assert_eq!(&q[q.len() - 4..], &[9, 8, 7, 6]);
        let back = parse(&t.to_mhgu(&out, 2)).unwrap().1[0].payload.clone();
        assert_eq!(back.len(), CONN_RESP_HEAD + 4 * PLAYER_INFO + 4);
        assert_eq!(&back[CONN_RESP_HEAD..CONN_RESP_HEAD + 5], b"Kirin");
        assert_eq!(&back[5..13], &MHGU_MAGIC);
    }

    #[test]
    fn app_data_round_trip() {
        let mut adv: Vec<u8> = (0..200).map(|i| i as u8).collect();
        adv[20..28].copy_from_slice(&MHGU_MAGIC);
        adv[76..84].fill(0);
        let app = adv_to_uds_app(&adv, 7);
        assert_eq!(&app[12..16], &7u32.to_le_bytes());
        assert_eq!(&app[20..28], &[0; 8]);
        let mut back = uds_app_to_adv(&app);
        back[12..20].copy_from_slice(&adv[12..20]); // (the 3DS's session value is there instead)
        back[192..].copy_from_slice(&adv[192..]); // (no room for the last 8 on the 3DS)
        assert_eq!(back, adv);
    }
}
