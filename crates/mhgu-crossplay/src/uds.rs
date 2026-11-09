//! The 3DS's local wireless (UDS) as the Citra-family emulators carry it in a room: the bodies of the 802.11 frames
//! in a room's WifiPacket (citra.rs). Laid out as Azahar's nwm/uds_*.{h,cpp} send and expect them (big-endian
//! where the console is, little-endian where Citra copied its own structs); written from those layouts, not their code.
//!
//! What a UDS network is made of, in the order a join goes:
//!   beacon (host -> all): fixed fields, an SSID tag, Nintendo tags: the network, and its node list AES-CTR encrypted
//!   authentication SEQ1 (station -> host), SEQ2 (host -> station), association response (host -> station)
//!   EAPoL-Start (station -> host, a data frame): who joins; EAPoL-Logoff (host -> station): its node id and the nodes
//!   node map (host -> all): which MAC is which node; then SecureData frames: the game's packets between node ids.

use crate::crypto::{md5, sha1, Aes128};

pub type Mac = [u8; 6];
pub const BROADCAST_MAC: Mac = [0xFF; 6];
pub const BROADCAST_NODE: u16 = 0xFFFF;
pub const HOST_NODE: u16 = 1;
pub const MAX_NODES: usize = 16;

// WifiPacket types (network/room.h)
pub const T_BEACON: u8 = 0;
pub const T_DATA: u8 = 1;
pub const T_AUTH: u8 = 2;
pub const T_ASSOC_RESPONSE: u8 = 3;
pub const T_DEAUTH: u8 = 4;
pub const T_NODE_MAP: u8 = 5;

pub const NINTENDO_OUI: [u8; 3] = [0x00, 0x1F, 0x32];
const TAG_SSID: u8 = 0;
const TAG_VENDOR: u8 = 221;
const NTAG_DUMMY: u8 = 20;
const NTAG_NETWORK: u8 = 21;
const NTAG_DATA0: u8 = 24;
const NTAG_DATA1: u8 = 25;
pub const APP_DATA_MAX: usize = 0xC8;
const BEACON_DATA_HEAD: usize = 0x12; // md5[16], bitmask (be16)
const BEACON_NODE: usize = 0x1E;
const NODE_INFO_SIZE: usize = 40; // Citra's NodeInfo: what it sizes the encrypted tags by
const DATA0_CUTOFF: usize = 0xFA;

pub const ETHER_SECURE_DATA: u16 = 0x876D;
pub const ETHER_EAPOL: u16 = 0x888E;
pub const EAPOL_START: u16 = 0x201;
pub const EAPOL_LOGOFF: u16 = 0x202;
const LLC: usize = 8;
const SECURE_HEADER: usize = 14;
const EAPOL_NODE: usize = 0x28;
const EAPOL_LOGOFF_SIZE: usize = 0x298;
pub const CONN_CLIENT: u8 = 1;
pub const HEARTBEAT_CHANNEL: u8 = 0xF;

/// One player of a UDS network: its friend code seed, name (UTF-16, 10 units) and node id (0: an empty slot).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Node {
    pub seed: u64,
    pub name: [u16; 10],
    pub id: u16,
}

impl Node {
    pub fn name_utf8(&self) -> String {
        let n = self.name.iter().position(|&c| c == 0).unwrap_or(self.name.len());
        String::from_utf16_lossy(&self.name[..n])
    }
    pub fn set_name(&mut self, s: &str) {
        self.name = [0; 10];
        for (d, c) in self.name.iter_mut().zip(s.encode_utf16()) {
            *d = c;
        }
    }
}

/// A UDS network as its beacon describes it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Network {
    pub host: Mac,
    pub channel: u8,
    pub comm_id: u32,
    pub id: u8,
    pub attributes: u16,
    pub network_id: u32,
    pub total: u8,
    pub max: u8,
    pub app: Vec<u8>,
    /// max entries, slot i for node i + 1 (id 0: free)
    pub nodes: Vec<Node>,
}

fn be16(d: &[u8], o: usize) -> u16 {
    u16::from_be_bytes([d[o], d[o + 1]])
}
fn be32(d: &[u8], o: usize) -> u32 {
    u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}
fn be64(d: &[u8], o: usize) -> u64 {
    u64::from_be_bytes(d[o..o + 8].try_into().unwrap())
}

// ---- beacons

/// The counter of the beacon's AES-CTR: host MAC, comm id (le), id, 0, network id (le).
fn beacon_ctr(n: &Network) -> [u8; 16] {
    let mut c = [0u8; 16];
    c[..6].copy_from_slice(&n.host);
    c[6..10].copy_from_slice(&n.comm_id.to_le_bytes());
    c[10] = n.id;
    c[12..16].copy_from_slice(&n.network_id.to_le_bytes());
    c
}
/// The emulators' beacon key: none was ever dumped into them, so it is zeros on every side.
fn beacon_aes() -> Aes128 {
    Aes128::new(&[0u8; 16])
}

/// The network-info tag's 0x1F bytes (from the OUI on).
fn network_fields(n: &Network) -> [u8; 0x1F] {
    let mut f = [0u8; 0x1F];
    f[..3].copy_from_slice(&NINTENDO_OUI);
    f[3] = NTAG_NETWORK;
    f[4..8].copy_from_slice(&n.comm_id.to_be_bytes());
    f[8] = n.id;
    f[10..12].copy_from_slice(&n.attributes.to_be_bytes());
    f[12..16].copy_from_slice(&n.network_id.to_be_bytes());
    f[16] = n.total;
    f[17] = n.max;
    f
}

pub fn build_beacon(n: &Network) -> Vec<u8> {
    let mut b = Vec::with_capacity(400);
    b.extend_from_slice(&900_000_000u64.to_le_bytes()); // uptime (us), fixed as Citra does
    b.extend_from_slice(&100u16.to_le_bytes()); // beacon interval (TU)
    b.extend_from_slice(&0x0431u16.to_le_bytes()); // capabilities
    b.extend_from_slice(&[TAG_SSID, 8, 0, 0, 0, 0, 0, 0, 0, 0]);
    b.extend_from_slice(&[TAG_VENDOR, 7, 0x00, 0x1F, 0x32, NTAG_DUMMY, 0x0A, 0x00, 0x00]);
    // the network: its fields, a SHA-1 over the tag (hash zeroed), the application data
    let app = &n.app[..n.app.len().min(APP_DATA_MAX)];
    let mut t = Vec::with_capacity(54 + app.len());
    t.extend_from_slice(&[TAG_VENDOR, (52 + app.len()) as u8]);
    t.extend_from_slice(&network_fields(n));
    t.extend_from_slice(&[0u8; 20]);
    t.push(app.len() as u8);
    t.extend_from_slice(app);
    let h = sha1(&t[2..]);
    t[2 + 0x1F..2 + 0x1F + 20].copy_from_slice(&h);
    b.extend_from_slice(&t);
    // the nodes, encrypted: [md5, bitmask] + one entry per slot, cut in two tags at 0xFA bytes. Citra sizes the tags
    // by its own NodeInfo (40 bytes a slot, more than the 30 of an entry): the rest is zeros, encrypted too.
    let max = n.max as usize;
    let mut d = vec![0u8; BEACON_DATA_HEAD];
    for i in 0..max {
        let nd = n.nodes.get(i).copied().unwrap_or_default();
        d.extend_from_slice(&nd.seed.to_be_bytes());
        for c in nd.name {
            d.extend_from_slice(&c.to_be_bytes());
        }
        d.extend_from_slice(&nd.id.to_be_bytes());
    }
    let h = md5(&d[16..]);
    d[..16].copy_from_slice(&h);
    let total = (max * NODE_INFO_SIZE).max(d.len());
    d.resize(total, 0);
    beacon_aes().ctr(&beacon_ctr(n), &mut d);
    let first = total.min(DATA0_CUTOFF).min(max * NODE_INFO_SIZE);
    b.extend_from_slice(&[TAG_VENDOR, (4 + first) as u8, 0x00, 0x1F, 0x32, NTAG_DATA0]);
    b.extend_from_slice(&d[..first]);
    if max * NODE_INFO_SIZE > DATA0_CUTOFF {
        let rest = &d[DATA0_CUTOFF..max * NODE_INFO_SIZE];
        b.extend_from_slice(&[TAG_VENDOR, (4 + rest.len()).min(255) as u8, 0x00, 0x1F, 0x32, NTAG_DATA1]);
        b.extend_from_slice(&rest[..rest.len().min(251)]);
    }
    b
}

/// A beacon from the room (tx: who sent it, the network's host).
pub fn parse_beacon(tx: Mac, d: &[u8]) -> Option<Network> {
    if d.len() < 12 {
        return None;
    }
    let mut n = Network { host: tx, ..Default::default() };
    let mut have_info = false;
    let mut enc = Vec::new();
    let mut o = 12;
    while o + 2 <= d.len() {
        let (id, len) = (d[o], d[o + 1] as usize);
        let body = d.get(o + 2..o + 2 + len)?;
        o += 2 + len;
        if id != TAG_VENDOR || len < 4 || body[..3] != NINTENDO_OUI {
            continue;
        }
        match body[3] {
            NTAG_NETWORK if len >= 52 => {
                n.comm_id = be32(body, 4);
                n.id = body[8];
                n.attributes = be16(body, 10);
                n.network_id = be32(body, 12);
                n.total = body[16];
                n.max = body[17];
                let size = body[0x1F + 20] as usize;
                n.app = body.get(52..52 + size)?.to_vec();
                have_info = true;
            }
            NTAG_DATA0 | NTAG_DATA1 => enc.extend_from_slice(&body[4..]),
            _ => {}
        }
    }
    if !have_info || n.max == 0 || n.max as usize > MAX_NODES {
        return None;
    }
    beacon_aes().ctr(&beacon_ctr(&n), &mut enc);
    for i in 0..n.max as usize {
        let at = BEACON_DATA_HEAD + i * BEACON_NODE;
        let mut nd = Node::default();
        if at + BEACON_NODE <= enc.len() {
            nd.seed = be64(&enc, at);
            for k in 0..10 {
                nd.name[k] = be16(&enc, at + 8 + 2 * k);
            }
            nd.id = be16(&enc, at + 28);
        }
        n.nodes.push(nd);
    }
    Some(n)
}

// ---- authentication and association

pub fn auth_frame(seq: u16) -> Vec<u8> {
    let mut b = vec![0u8; 6];
    b[2..4].copy_from_slice(&seq.to_le_bytes());
    b
}
pub fn auth_seq(d: &[u8]) -> Option<u16> {
    (d.len() >= 6).then(|| u16::from_le_bytes([d[2], d[3]]))
}

pub fn assoc_response(assoc_id: u16, network_id: u32) -> Vec<u8> {
    let mut b = Vec::with_capacity(16);
    b.extend_from_slice(&0x0431u16.to_le_bytes());
    b.extend_from_slice(&0u16.to_le_bytes()); // status: successful
    b.extend_from_slice(&(assoc_id | 0xC000).to_le_bytes());
    b.extend_from_slice(&[TAG_SSID, 8]);
    b.extend_from_slice(format!("{network_id:08X}").as_bytes());
    b
}
/// (status, association id)
pub fn parse_assoc(d: &[u8]) -> Option<(u16, u16)> {
    (d.len() >= 6).then(|| (u16::from_le_bytes([d[2], d[3]]), u16::from_le_bytes([d[4], d[5]]) & 0x3FFF))
}

// ---- data frames

fn llc(ether: u16) -> Vec<u8> {
    let mut b = vec![0xAA, 0xAA, 0x03, 0, 0, 0];
    b.extend_from_slice(&ether.to_be_bytes());
    b
}

fn eapol_node(out: &mut Vec<u8>, n: &Node) {
    out.extend_from_slice(&n.seed.to_be_bytes());
    for c in n.name {
        out.extend_from_slice(&c.to_be_bytes());
    }
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&n.id.to_be_bytes());
    out.extend_from_slice(&[0; 6]);
}
fn read_eapol_node(d: &[u8], o: usize) -> Node {
    let mut n = Node { seed: be64(d, o), id: be16(d, o + 0x20), ..Default::default() };
    for k in 0..10 {
        n.name[k] = be16(d, o + 8 + 2 * k);
    }
    n
}

pub fn eapol_start(assoc_id: u16, node: &Node) -> Vec<u8> {
    let mut b = llc(ETHER_EAPOL);
    b.extend_from_slice(&EAPOL_START.to_be_bytes());
    b.extend_from_slice(&assoc_id.to_be_bytes());
    b.extend_from_slice(&[CONN_CLIENT, 0, 0, 0]);
    eapol_node(&mut b, &Node { id: 0, ..*node });
    b
}

/// nodes: max entries, slot i for node i + 1
pub fn eapol_logoff(client: Mac, assigned: u16, nodes: &[Node], max: u8, total: u8) -> Vec<u8> {
    let mut b = llc(ETHER_EAPOL);
    b.extend_from_slice(&EAPOL_LOGOFF.to_be_bytes());
    b.extend_from_slice(&[0, 0]);
    b.extend_from_slice(&assigned.to_be_bytes());
    b.extend_from_slice(&client);
    b.extend_from_slice(&[0; 6]);
    b.push(total);
    b.push(max);
    b.extend_from_slice(&[0; 4]);
    for i in 0..MAX_NODES {
        let n = if i < max as usize { nodes.get(i).copied().unwrap_or_default() } else { Node::default() };
        eapol_node(&mut b, &n);
    }
    debug_assert_eq!(b.len(), LLC + EAPOL_LOGOFF_SIZE);
    b
}

pub fn data_frame(payload: &[u8], channel: u8, dest: u16, src: u16, seq: u16) -> Vec<u8> {
    let mut b = llc(ETHER_SECURE_DATA);
    let n = payload.len() as u16;
    b.extend_from_slice(&(n + SECURE_HEADER as u16).to_be_bytes());
    b.extend_from_slice(&[0, 0]);
    b.extend_from_slice(&(n + SECURE_HEADER as u16 - 4).to_be_bytes());
    b.push(0); // not a management frame
    b.push(channel);
    b.extend_from_slice(&seq.to_be_bytes());
    b.extend_from_slice(&dest.to_be_bytes());
    b.extend_from_slice(&src.to_be_bytes());
    b.extend_from_slice(payload);
    b
}

#[derive(Debug, PartialEq, Eq)]
pub enum Data<'a> {
    Start { assoc_id: u16, kind: u8, node: Node },
    Logoff { assigned: u16, total: u8, max: u8, nodes: Vec<Node> },
    Secure { channel: u8, dest: u16, src: u16, payload: &'a [u8] },
}

pub fn parse_data(d: &[u8]) -> Option<Data<'_>> {
    if d.len() < LLC + 2 || d[..6] != [0xAA, 0xAA, 0x03, 0, 0, 0] {
        return None;
    }
    match be16(d, 6) {
        ETHER_EAPOL => match be16(d, LLC) {
            EAPOL_START if d.len() >= LLC + 0x30 => {
                Some(Data::Start { assoc_id: be16(d, LLC + 2), kind: d[LLC + 4], node: read_eapol_node(d, LLC + 8) })
            }
            EAPOL_LOGOFF if d.len() >= LLC + EAPOL_LOGOFF_SIZE => {
                let max = d[LLC + 0x13];
                let nodes = (0..MAX_NODES).map(|i| read_eapol_node(d, LLC + 0x18 + i * EAPOL_NODE)).collect();
                Some(Data::Logoff { assigned: be16(d, LLC + 4), total: d[LLC + 0x12], max, nodes })
            }
            _ => None,
        },
        ETHER_SECURE_DATA if d.len() >= LLC + SECURE_HEADER => {
            let size = (be16(d, LLC) as usize).saturating_sub(SECURE_HEADER);
            let start = LLC + SECURE_HEADER;
            let end = (start + size).min(d.len());
            Some(Data::Secure {
                channel: d[LLC + 7],
                dest: be16(d, LLC + 10),
                src: be16(d, LLC + 12),
                payload: &d[start..end],
            })
        }
        _ => None,
    }
}

// ---- node map (Citra's own: a native size_t count, then MAC + u16 per node, little endian)

pub fn node_map(entries: &[(Mac, u16)]) -> Vec<u8> {
    let mut b = (entries.len() as u64).to_le_bytes().to_vec();
    for (m, id) in entries {
        b.extend_from_slice(m);
        b.extend_from_slice(&id.to_le_bytes());
    }
    b
}
pub fn parse_node_map(d: &[u8]) -> Vec<(Mac, u16)> {
    if d.len() < 8 {
        return Vec::new();
    }
    let n = (u64::from_le_bytes(d[..8].try_into().unwrap()) as usize).min(MAX_NODES * 2);
    (0..n)
        .map_while(|i| {
            let o = 8 + i * 8;
            let e = d.get(o..o + 8)?;
            Some(([e[0], e[1], e[2], e[3], e[4], e[5]], u16::from_le_bytes([e[6], e[7]])))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Network {
        let mut a = Node { seed: 0x1122_3344_5566_7788, id: 1, ..Default::default() };
        a.set_name("Hunter");
        let mut b = Node { seed: 42, id: 3, ..Default::default() };
        b.set_name("Felyne");
        Network {
            host: [2, 1, 2, 3, 4, 5],
            channel: 11,
            comm_id: 0x0019_7110,
            id: 0,
            attributes: 0x8000,
            network_id: 0xDEAD_BEEF,
            total: 2,
            max: 4,
            app: (0..200).map(|i| i as u8).collect(),
            nodes: vec![a, Node::default(), b, Node::default()],
        }
    }

    #[test]
    fn beacon_round_trip() {
        let n = sample();
        let b = build_beacon(&n);
        let mut p = parse_beacon(n.host, &b).unwrap();
        p.channel = n.channel; // (the room packet carries the channel, not the beacon)
        assert_eq!(p, n);
        assert_eq!(p.nodes[0].name_utf8(), "Hunter");
    }

    #[test]
    fn beacon_with_many_slots_uses_the_second_tag() {
        let mut n = sample();
        n.max = 8;
        n.nodes.resize(8, Node::default());
        n.nodes[7] = Node { seed: 7, id: 8, ..Default::default() };
        let p = parse_beacon(n.host, &build_beacon(&n)).unwrap();
        assert_eq!(p.nodes[7].id, 8);
        assert_eq!(p.nodes[7].seed, 7);
    }

    #[test]
    fn beacon_hash_matches_its_tag() {
        // the network tag's SHA-1 covers the tag from its OUI on, with the hash field zero
        let n = sample();
        let b = build_beacon(&n);
        let t = 12 + 10 + 9; // fixed, SSID, dummy
        assert_eq!(b[t], TAG_VENDOR);
        let len = b[t + 1] as usize;
        let mut body = b[t + 2..t + 2 + len].to_vec();
        let h: [u8; 20] = body[0x1F..0x1F + 20].try_into().unwrap();
        body[0x1F..0x1F + 20].fill(0);
        assert_eq!(sha1(&body), h);
    }

    #[test]
    fn eapol_and_data() {
        let mut me = Node { seed: 9, ..Default::default() };
        me.set_name("Me");
        match parse_data(&eapol_start(1, &me)).unwrap() {
            Data::Start { assoc_id, kind, node } => {
                assert_eq!((assoc_id, kind), (1, CONN_CLIENT));
                assert_eq!(node.seed, 9);
                assert_eq!(node.name_utf8(), "Me");
            }
            x => panic!("{x:?}"),
        }
        let n = sample();
        match parse_data(&eapol_logoff([1; 6], 2, &n.nodes, 4, 2)).unwrap() {
            Data::Logoff { assigned, total, max, nodes } => {
                assert_eq!((assigned, total, max), (2, 2, 4));
                assert_eq!(nodes[2], n.nodes[2]);
                assert_eq!(nodes[5], Node::default());
            }
            x => panic!("{x:?}"),
        }
        let f = data_frame(b"hello", 0xF3, BROADCAST_NODE, 2, 7);
        assert_eq!(parse_data(&f).unwrap(), Data::Secure { channel: 0xF3, dest: 0xFFFF, src: 2, payload: b"hello" });
    }

    #[test]
    fn small_frames() {
        assert_eq!(auth_seq(&auth_frame(2)), Some(2));
        assert_eq!(parse_assoc(&assoc_response(1, 0x1234)), Some((0, 1)));
        assert!(assoc_response(1, 0xAB).ends_with(b"000000AB"));
        let m = vec![([1u8; 6], 1u16), ([2u8; 6], 3u16)];
        assert_eq!(parse_node_map(&node_map(&m)), m);
    }
}
