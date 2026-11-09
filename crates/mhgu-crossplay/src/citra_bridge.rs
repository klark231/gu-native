//! MHGU in a Citra-family room (Azahar, Citra) with Monster Hunter XX players: this game plays one 3DS there.
//!
//! The room carries the 3DS's local wireless (UDS) between its members, one MAC each (citra.rs); this file is that
//! console's UDS side, and gives runtime/net/ldn.c an nn::ldn network to show the game:
//!  - an MHXX player hosts: its beacons are listed in the game's search; joining one is a UDS join (authentication,
//!    association, EAPoL), and the network's nodes become the LDN network's.
//!  - the game hosts: this console sends beacons for it, and each MHXX player who joins is a station of the LDN
//!    network.
//!
//! The game's packets (Pia) are converted on the way (pia.rs). Another phone running this is just one more 3DS.
//!
//! UDS node n is 169.254.x.n on the LDN side (x from the host's MAC, so every phone in a network agrees). Ours has
//! a friend code seed made for its IP: to the 3DS, our Pia address is the IP MHGU uses.
//!
//! `Core` is the protocol and nothing else (frames in, actions out), so the tests can drive it; the driver below runs
//! it on the room's thread and a ticker, and the C API (runtime/net/citra_room.h) is at the end.

use crate::citra;
use crate::crypto::{address_of_seed, crc32, md5, seed_for_address};
use crate::pia::{self, Translator, PIA_PORT};
use crate::uds::{self, Data, Mac, Network, Node, BROADCAST_MAC, BROADCAST_NODE, HOST_NODE};
use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

pub const MHXX_COMM_ID: u32 = 0x0019_7110;
const MHXX_ATTRIBUTES: u16 = 0x8000; // what MHXX sets on the networks it hosts
const MHGU_LOCAL_COMM_ID: u64 = 0x0100_7700_08DD_8000;
const MHGU_LCV: u16 = 1;
const WIFI_CHANNEL: u8 = 11;
const DEFAULT_DATA_CHANNEL: u8 = 0xF3; // MHXX's
const LDN_MAX_NODES: usize = 8;

const BEACON_EVERY: Duration = Duration::from_millis(102);
const HEARTBEAT_EVERY: Duration = Duration::from_millis(250);
const NODE_MAP_EVERY: Duration = Duration::from_secs(5);
const SEEN_FOR: Duration = Duration::from_secs(3);
const JOIN_TIMEOUT: Duration = Duration::from_secs(5);
const HOST_SILENT: Duration = Duration::from_secs(15);
const MEMBER_SILENT: Duration = Duration::from_secs(30);

// ---- nn::ldn NetworkInfo (0x480 bytes, little endian): ldn.c's NI_* / NODE_*
pub const NETINFO: usize = 0x480;
mod ni {
    pub const LOCAL_COMM_ID: usize = 0x00;
    pub const SESSION_ID: usize = 0x10;
    pub const BSSID: usize = 0x20;
    pub const SSID: usize = 0x26;
    pub const CHANNEL: usize = 0x48;
    pub const LINK_LEVEL: usize = 0x4A;
    pub const NETWORK_TYPE: usize = 0x4B;
    pub const SEC_PARAM: usize = 0x50;
    pub const SEC_MODE: usize = 0x60;
    pub const ACCEPT_POLICY: usize = 0x62;
    pub const NODE_MAX: usize = 0x66;
    pub const NODE_COUNT: usize = 0x67;
    pub const NODES: usize = 0x68;
    pub const ADV_SIZE: usize = 0x26A;
    pub const ADV: usize = 0x26C;
    pub const AUTH_ID: usize = 0x478;
    pub const NODE: usize = 0x40;
    pub const N_IP: usize = 0x00;
    pub const N_MAC: usize = 0x04;
    pub const N_ID: usize = 0x0A;
    pub const N_CONNECTED: usize = 0x0B;
    pub const N_NAME: usize = 0x0C;
    pub const N_LCV: usize = 0x2E;
    pub const ADV_MAX: usize = 0x180;
}

/// What the core wants done.
#[derive(Debug, PartialEq, Eq)]
pub enum Out {
    /// a WifiPacket into the room
    Frame { ty: u8, dst: Mac, data: Vec<u8> },
    /// the LDN network we're in changed (None: it's gone)
    Info(Option<Vec<u8>>),
    /// a game packet for the local game (UDP, Pia's port both ends)
    Packet { src: u32, dst: u32, data: Vec<u8> },
}

/// 169.254.x.0 for a network hosted by `host`
pub fn subnet_for(host: &Mac) -> u32 {
    0xA9FE_0000 | (1 + crc32(host) % 254) << 8
}

fn node_slot(info: &mut [u8], i: usize) -> &mut [u8] {
    &mut info[ni::NODES + i * ni::NODE..ni::NODES + (i + 1) * ni::NODE]
}
fn ldn_name(info: &[u8], i: usize) -> String {
    let o = ni::NODES + i * ni::NODE + ni::N_NAME;
    let f = &info[o..o + 0x20];
    let n = f.iter().position(|&b| b == 0).unwrap_or(0x20);
    String::from_utf8_lossy(&f[..n]).into_owned()
}
fn put_name(slot: &mut [u8], name: &str) {
    let mut b = name.as_bytes();
    while b.len() > 0x20 {
        // (whole characters only)
        let mut n = 0x20;
        while n > 0 && (name.as_bytes()[n] & 0xC0) == 0x80 {
            n -= 1;
        }
        b = &name.as_bytes()[..n];
    }
    slot[ni::N_NAME..ni::N_NAME + 0x21].fill(0);
    slot[ni::N_NAME..ni::N_NAME + b.len()].copy_from_slice(b);
}
fn fake_mac(subnet: u32, id: u16) -> Mac {
    let s = subnet.to_be_bytes();
    [0x02, 0x00, s[0], s[1], s[2], id as u8]
}

/// The LDN network for a UDS network: MHGU's ids, node n at index n - 1 (as far as the LDN side has room).
fn ldn_info(net: &Network, nodes: &[Node], macs: &HashMap<u16, Mac>, total: u8) -> Vec<u8> {
    let mut info = vec![0u8; NETINFO];
    let subnet = subnet_for(&net.host);
    info[ni::LOCAL_COMM_ID..ni::LOCAL_COMM_ID + 8].copy_from_slice(&MHGU_LOCAL_COMM_ID.to_le_bytes());
    // (session, security and auth ids only have to be the same for everyone who sees this network)
    let mut seed = b"mhxx-uds".to_vec();
    seed.extend_from_slice(&net.host);
    seed.extend_from_slice(&net.network_id.to_be_bytes());
    let h = md5(&seed);
    info[ni::SESSION_ID..ni::SESSION_ID + 16].copy_from_slice(&h);
    seed.push(1);
    info[ni::SEC_PARAM..ni::SEC_PARAM + 16].copy_from_slice(&md5(&seed));
    seed.push(2);
    info[ni::AUTH_ID..ni::AUTH_ID + 8].copy_from_slice(&md5(&seed)[..8]);
    info[ni::BSSID..ni::BSSID + 6].copy_from_slice(&net.host);
    let ssid = format!("MHXXRoom{:08X}", net.network_id);
    info[ni::SSID] = ssid.len() as u8;
    info[ni::SSID + 1..ni::SSID + 1 + ssid.len()].copy_from_slice(ssid.as_bytes());
    info[ni::CHANNEL..ni::CHANNEL + 2].copy_from_slice(&6u16.to_le_bytes());
    info[ni::LINK_LEVEL] = 3;
    info[ni::NETWORK_TYPE] = 2;
    info[ni::SEC_MODE..ni::SEC_MODE + 2].copy_from_slice(&1u16.to_le_bytes());
    let max = (net.max as usize).clamp(1, LDN_MAX_NODES);
    info[ni::NODE_MAX] = max as u8;
    let mut count = 0u8;
    for (i, n) in nodes.iter().enumerate().take(max) {
        if n.id == 0 {
            continue;
        }
        let id = (i + 1) as u16;
        let mac = if id == HOST_NODE { net.host } else { macs.get(&id).copied().unwrap_or(fake_mac(subnet, id)) };
        let slot = node_slot(&mut info, i);
        slot[ni::N_IP..ni::N_IP + 4].copy_from_slice(&(subnet | id as u32).to_le_bytes());
        slot[ni::N_MAC..ni::N_MAC + 6].copy_from_slice(&mac);
        slot[ni::N_ID] = i as u8;
        slot[ni::N_CONNECTED] = 1;
        put_name(slot, &n.name_utf8());
        slot[ni::N_LCV..ni::N_LCV + 2].copy_from_slice(&MHGU_LCV.to_le_bytes());
        count += 1;
    }
    info[ni::NODE_COUNT] = count.max(1).max(total.min(max as u8)).min(max as u8);
    let adv = pia::uds_app_to_adv(&net.app);
    let n = adv.len().min(ni::ADV_MAX);
    info[ni::ADV_SIZE..ni::ADV_SIZE + 2].copy_from_slice(&(n as u16).to_le_bytes());
    info[ni::ADV..ni::ADV + n].copy_from_slice(&adv[..n]);
    info
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Phase {
    Auth,
    Eapol,
    Joined,
}

struct Station {
    host: Mac,
    net: Network,
    phase: Phase,
    started: Instant,
    me: Node,
    my_id: u16,
    nodes: Vec<Node>,
    macs: HashMap<u16, Mac>,
    total: u8,
    last_host: Instant,
    last_heartbeat: Instant,
    pia: Translator,
}

struct Member {
    mac: Mac,
    node: Node,
    last_rx: Instant,
}

struct Host {
    ldn: Vec<u8>,
    network_id: u32,
    session_param: u32,
    subnet: u32,
    me: Node,
    members: Vec<Option<Member>>, // index id - 1; [0] is us
    pending: HashMap<Mac, Instant>,
    last_beacon: Option<Instant>,
    last_heartbeat: Instant,
    last_map: Instant,
    pia: Translator,
}

enum Role {
    Idle,
    Station(Box<Station>),
    Host(Box<Host>),
}

pub struct Core {
    pub mac: Mac,
    seen: Vec<(Network, Instant)>,
    id8: u8,      // the id MHXX's networks carry (learned from their beacons; 0 until then, which MHXX takes)
    channel: u8,  // the game's UDS data channel
    seq: u16,
    rng: u64,
    role: Role,
    pub log: Vec<String>,
}

fn present(n: &Node) -> bool {
    n.id != 0
}

impl Core {
    pub fn new(mac: Mac, entropy: u64) -> Core {
        Core {
            mac,
            seen: Vec::new(),
            id8: 0,
            channel: DEFAULT_DATA_CHANNEL,
            seq: 0,
            rng: entropy | 1,
            role: Role::Idle,
            log: Vec::new(),
        }
    }
    fn rand32(&mut self) -> u32 {
        self.rng ^= self.rng << 13;
        self.rng ^= self.rng >> 7;
        self.rng ^= self.rng << 17;
        (self.rng >> 16) as u32
    }
    fn note(&mut self, s: String) {
        if self.log.len() < 64 {
            self.log.push(s);
        }
    }
    fn data(&mut self, dst: Mac, payload: &[u8], channel: u8, dest: u16, src: u16) -> Out {
        self.seq = self.seq.wrapping_add(1);
        Out::Frame { ty: uds::T_DATA, dst, data: uds::data_frame(payload, channel, dest, src, self.seq) }
    }

    pub fn idle(&self) -> bool {
        matches!(self.role, Role::Idle)
    }
    pub fn station_phase(&self) -> Option<Phase> {
        match &self.role {
            Role::Station(s) => Some(s.phase),
            _ => None,
        }
    }

    // ---- scanning

    /// The MHXX networks seen in the room lately, as LDN networks (not full ones, not our own).
    pub fn scan(&mut self, now: Instant) -> Vec<Vec<u8>> {
        self.seen.retain(|(_, at)| now.duration_since(*at) < SEEN_FOR);
        self.seen
            .iter()
            .filter(|(n, _)| n.comm_id == MHXX_COMM_ID && n.host != self.mac && n.total < n.max)
            .map(|(n, _)| ldn_info(n, &n.nodes, &HashMap::new(), n.total))
            .collect()
    }

    // ---- joining an MHXX player's network

    pub fn connect(&mut self, now: Instant, bssid: &Mac, name: &str) -> Result<Vec<Out>, &'static str> {
        if !self.idle() {
            return Err("busy");
        }
        let Some((net, _)) = self.seen.iter().find(|(n, _)| n.host == *bssid).cloned() else {
            return Err("network not found");
        };
        // the host gives a joiner the lowest free node: our seed is made for that one's IP (if someone else
        // takes it first, the translator still knows our real address)
        let guess = net.nodes.iter().position(|n| !present(n)).map(|i| i as u16 + 1).unwrap_or(2).max(2);
        let subnet = subnet_for(bssid);
        let mut me = Node { seed: seed_for_address(subnet | guess as u32), ..Default::default() };
        me.set_name(if name.is_empty() { "Hunter" } else { name });
        self.note(format!("[citra] joining the network of {:02x?} (expecting node {guess})", bssid));
        self.role = Role::Station(Box::new(Station {
            host: *bssid,
            net,
            phase: Phase::Auth,
            started: now,
            me,
            my_id: 0,
            nodes: Vec::new(),
            macs: HashMap::new(),
            total: 0,
            last_host: now,
            last_heartbeat: now,
            pia: Translator::new(subnet),
        }));
        // (a deauthentication first: a host that still has us from an earlier try refuses the authentication)
        Ok(vec![
            Out::Frame { ty: uds::T_DEAUTH, dst: *bssid, data: Vec::new() },
            Out::Frame { ty: uds::T_AUTH, dst: *bssid, data: uds::auth_frame(1) },
        ])
    }

    /// The joined network: its LDN info and our IP.
    pub fn station_info(&self) -> Option<(Vec<u8>, u32)> {
        let Role::Station(s) = &self.role else { return None };
        (s.phase == Phase::Joined).then(|| {
            (ldn_info(&s.net, &s.nodes, &s.macs, s.total), subnet_for(&s.host) | s.my_id as u32)
        })
    }

    pub fn leave(&mut self) -> Vec<Out> {
        let mut out = Vec::new();
        if let Role::Station(s) = &self.role {
            out.push(Out::Frame { ty: uds::T_DEAUTH, dst: s.host, data: Vec::new() });
            self.role = Role::Idle;
        }
        out
    }

    fn station_update(s: &mut Station, nodes: &[Node], max: u8, total: u8) {
        let max = (max as usize).clamp(1, uds::MAX_NODES);
        s.nodes = nodes.iter().take(max).copied().collect();
        s.nodes.resize(max, Node::default());
        // (an entry's id is its node; a stray one in the wrong slot is dropped)
        for (i, n) in s.nodes.iter_mut().enumerate() {
            if n.id != 0 && n.id as usize != i + 1 {
                *n = Node::default();
            }
        }
        s.net.max = max as u8;
        s.total = total;
        s.pia.clear_nodes();
        for n in s.nodes.iter().filter(|n| present(n)) {
            s.pia.set_node(n.id, address_of_seed(n.seed));
        }
    }

    // ---- hosting for the local game

    /// Start hosting: `info` is the LDN network ldn.c made (node 0 the local player, its IP subnet_for(our MAC) | 1).
    pub fn host(&mut self, now: Instant, info: &[u8]) -> Vec<Out> {
        let mut out = self.leave();
        let subnet = subnet_for(&self.mac);
        let mut me = Node { seed: seed_for_address(subnet | 1), id: HOST_NODE, ..Default::default() };
        me.set_name(&ldn_name(info, 0));
        let mut members: Vec<Option<Member>> = Vec::new();
        members.resize_with(uds::MAX_NODES, || None);
        let network_id = self.rand32() | 1;
        let session_param = self.rand32() | 1;
        self.role = Role::Host(Box::new(Host {
            ldn: info[..NETINFO].to_vec(),
            network_id,
            session_param,
            subnet,
            me,
            members,
            pending: HashMap::new(),
            last_beacon: None,
            last_heartbeat: now,
            last_map: now,
            pia: Translator::new(subnet),
        }));
        self.note(format!("[citra] hosting for MHXX players: network {network_id:08x}"));
        out.extend(self.host_beacon(now));
        out
    }

    /// ldn.c's network changed: `info` holds the local player and the players who came through ldn.c's own host
    /// (LAN, hub servers), not ours. Those are UDS nodes too, so the MHXX players learn of a change in them; nothing
    /// is handed back (ldn.c calls this holding its lock).
    pub fn host_update(&mut self, info: &[u8]) -> Vec<Out> {
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        let before = Self::link_ids(h);
        h.ldn = info[..NETINFO].to_vec();
        h.me.set_name(&ldn_name(info, 0));
        // (a member's slot that ldn.c now also uses stays the member's here: ldn.c sees the clash and sends one away)
        if Self::link_ids(h) != before {
            return self.host_membership(None, false);
        }
        Vec::new()
    }

    /// The UDS node ids of ldn.c's own players (its connected slots past the local player, not ours).
    fn link_ids(h: &Host) -> Vec<u16> {
        (1..Self::max_nodes(h))
            .filter(|&i| h.members[i].is_none() && h.ldn[ni::NODES + i * ni::NODE + ni::N_CONNECTED] != 0)
            .map(|i| i as u16 + 1)
            .collect()
    }
    fn is_link(h: &Host, id: u16) -> bool {
        let i = id as usize;
        i >= 2 && i <= Self::max_nodes(h) && h.members[i - 1].is_none()
            && h.ldn[ni::NODES + (i - 1) * ni::NODE + ni::N_CONNECTED] != 0
    }

    pub fn host_stop(&mut self) -> Vec<Out> {
        if !matches!(self.role, Role::Host(_)) {
            return Vec::new();
        }
        self.role = Role::Idle;
        // (as a 3DS closing its network: a deauthentication to everyone, three times)
        (0..3).map(|_| Out::Frame { ty: uds::T_DEAUTH, dst: BROADCAST_MAC, data: Vec::new() }).collect()
    }

    fn max_nodes(h: &Host) -> usize {
        (h.ldn[ni::NODE_MAX] as usize).clamp(2, LDN_MAX_NODES)
    }

    fn uds_nodes(h: &Host) -> Vec<Node> {
        let max = Self::max_nodes(h);
        (0..max)
            .map(|i| {
                if i == 0 {
                    h.me
                } else if let Some(m) = &h.members[i] {
                    m.node
                } else if Self::is_link(h, i as u16 + 1) {
                    // a gu-native player of ldn.c's: its seed is made for its IP, as ours is
                    let mut n = Node { seed: seed_for_address(h.subnet | (i as u32 + 1)), id: i as u16 + 1, ..Default::default() };
                    n.set_name(&ldn_name(&h.ldn, i));
                    n
                } else {
                    Node::default()
                }
            })
            .collect()
    }
    /// everyone but us: our MHXX players and ldn.c's
    fn member_count(h: &Host) -> usize {
        h.members.iter().flatten().count() + Self::link_ids(h).len()
    }

    /// ldn.c's network with the MHXX players added
    fn host_ldn(&self) -> Vec<u8> {
        let Role::Host(h) = &self.role else { return Vec::new() };
        let mut info = h.ldn.clone();
        let max = Self::max_nodes(h);
        let lcv = u16::from_le_bytes([info[ni::NODES + ni::N_LCV], info[ni::NODES + ni::N_LCV + 1]]);
        let links = Self::link_ids(h);
        for i in 1..LDN_MAX_NODES {
            if !links.contains(&(i as u16 + 1)) {
                node_slot(&mut info, i).fill(0);
            }
        }
        let mut count = 1 + links.len() as u8;
        for (i, m) in h.members.iter().enumerate().take(max).skip(1) {
            let Some(m) = m else { continue };
            let slot = node_slot(&mut info, i);
            slot[ni::N_IP..ni::N_IP + 4].copy_from_slice(&(h.subnet | (i as u32 + 1)).to_le_bytes());
            slot[ni::N_MAC..ni::N_MAC + 6].copy_from_slice(&m.mac);
            slot[ni::N_ID] = i as u8;
            slot[ni::N_CONNECTED] = 1;
            put_name(slot, &m.node.name_utf8());
            slot[ni::N_LCV..ni::N_LCV + 2].copy_from_slice(&(if lcv != 0 { lcv } else { MHGU_LCV }).to_le_bytes());
            count += 1;
        }
        info[ni::NODE_COUNT] = count;
        info
    }

    /// What ldn.c is handed: the local player and our MHXX players only (it keeps its own and the other door's; a
    /// slot left out is free, so a stale view of theirs can't bring a player back).
    fn host_members_ldn(&self) -> Vec<u8> {
        let Role::Host(h) = &self.role else { return Vec::new() };
        let mut info = self.host_ldn();
        let mut count = 1u8;
        for i in 1..LDN_MAX_NODES {
            if h.members[i].is_some() {
                count += 1;
            } else {
                node_slot(&mut info, i).fill(0);
            }
        }
        info[ni::NODE_COUNT] = count;
        info
    }

    fn host_beacon(&mut self, now: Instant) -> Vec<Out> {
        let id8 = self.id8;
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        h.last_beacon = Some(now);
        let max = Self::max_nodes(h);
        let adv_size = (u16::from_le_bytes([h.ldn[ni::ADV_SIZE], h.ldn[ni::ADV_SIZE + 1]]) as usize).min(ni::ADV_MAX);
        let adv = &h.ldn[ni::ADV..ni::ADV + adv_size];
        let net = Network {
            host: self.mac,
            channel: WIFI_CHANNEL,
            comm_id: MHXX_COMM_ID,
            id: id8,
            attributes: MHXX_ATTRIBUTES,
            network_id: h.network_id,
            total: 1 + Self::member_count(h) as u8,
            max: max as u8,
            app: pia::adv_to_uds_app(adv, h.session_param),
            nodes: Self::uds_nodes(h),
        };
        vec![Out::Frame { ty: uds::T_BEACON, dst: BROADCAST_MAC, data: uds::build_beacon(&net) }]
    }

    /// The node list changed: the update to each member (an EAPoL-Logoff, as Azahar does it), a node map.
    fn host_membership(&mut self, joined: Option<(Mac, u16)>, with_info: bool) -> Vec<Out> {
        let my_mac = self.mac;
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        let nodes = Self::uds_nodes(h);
        let max = Self::max_nodes(h) as u8;
        let total = 1 + Self::member_count(h) as u8;
        let mut out = Vec::new();
        let (client, assigned) = joined.unwrap_or((BROADCAST_MAC, 0));
        let logoff = uds::eapol_logoff(client, assigned, &nodes, max, total);
        // (the joiner first: a console still connecting takes the first logoff it gets)
        if let Some((mac, _)) = joined {
            out.push((mac, logoff.clone()));
        }
        for m in h.members.iter().flatten() {
            if Some(m.mac) != joined.map(|j| j.0) {
                out.push((m.mac, logoff.clone()));
            }
        }
        let map = Self::map_entries(h, my_mac);
        h.last_map = Instant::now();
        h.pia.clear_nodes();
        for m in h.members.iter().flatten() {
            h.pia.set_node(m.node.id, address_of_seed(m.node.seed));
        }
        let mut res: Vec<Out> = out.into_iter().map(|(dst, data)| Out::Frame { ty: uds::T_DATA, dst, data }).collect();
        res.push(Out::Frame { ty: uds::T_NODE_MAP, dst: BROADCAST_MAC, data: uds::node_map(&map) });
        if with_info {
            res.push(Out::Info(Some(self.host_members_ldn())));
        }
        res
    }

    /// Which MAC is which node: ldn.c's players are reached through us.
    fn map_entries(h: &Host, my_mac: Mac) -> Vec<(Mac, u16)> {
        let mut map = vec![(my_mac, HOST_NODE)];
        map.extend(h.members.iter().flatten().map(|m| (m.mac, m.node.id)));
        map.extend(Self::link_ids(h).into_iter().map(|id| (my_mac, id)));
        map
    }

    fn host_remove(&mut self, mac: &Mac, why: &str) -> Vec<Out> {
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        let Some(i) = h.members.iter().position(|m| m.as_ref().map(|m| &m.mac) == Some(mac)) else { return Vec::new() };
        let m = h.members[i].take().unwrap();
        let name = m.node.name_utf8();
        self.note(format!("[citra] {name} (node {}) left the network: {why}", i + 1));
        self.host_membership(None, true)
    }

    pub fn reject(&mut self, ip: u32) -> Vec<Out> {
        let Role::Host(h) = &self.role else { return Vec::new() };
        let id = (ip & 0xFF) as usize;
        if ip & 0xFFFF_FF00 != h.subnet || !(2..=uds::MAX_NODES).contains(&id) {
            return Vec::new();
        }
        let Some(mac) = h.members[id - 1].as_ref().map(|m| m.mac) else { return Vec::new() };
        let mut out = vec![Out::Frame { ty: uds::T_DEAUTH, dst: mac, data: Vec::new() }];
        out.extend(self.host_remove(&mac, "sent away"));
        out
    }

    // ---- the room

    /// The room's members changed: anyone of ours who is gone from it is gone from the network.
    pub fn room_members(&mut self, macs: &[Mac]) -> Vec<Out> {
        if macs.is_empty() {
            return Vec::new();
        }
        match &self.role {
            Role::Host(h) => {
                let gone: Vec<Mac> =
                    h.members.iter().flatten().filter(|m| !macs.contains(&m.mac)).map(|m| m.mac).collect();
                gone.iter().flat_map(|m| self.host_remove(m, "left the room")).collect::<Vec<_>>()
            }
            Role::Station(s) if s.phase == Phase::Joined && !macs.contains(&s.host) => {
                self.note("[citra] the network's host left the room".into());
                self.role = Role::Idle;
                vec![Out::Info(None)]
            }
            _ => Vec::new(),
        }
    }

    pub fn room_lost(&mut self) -> Vec<Out> {
        let was = matches!(&self.role, Role::Host(_)) || matches!(&self.role, Role::Station(s) if s.phase == Phase::Joined);
        self.role = Role::Idle;
        self.seen.clear();
        if was { vec![Out::Info(None)] } else { Vec::new() }
    }

    /// A WifiPacket from the room.
    pub fn on_frame(&mut self, now: Instant, ty: u8, tx: Mac, dst: Mac, d: &[u8]) -> Vec<Out> {
        if tx == self.mac || (dst != self.mac && dst != BROADCAST_MAC) {
            return Vec::new();
        }
        if let Role::Station(s) = &mut self.role {
            if tx == s.host {
                s.last_host = now;
            }
        }
        match ty {
            uds::T_BEACON => self.on_beacon(now, tx, d),
            uds::T_AUTH => self.on_auth(now, tx, d),
            uds::T_ASSOC_RESPONSE => self.on_assoc(tx, d),
            uds::T_DEAUTH => self.on_deauth(tx),
            uds::T_NODE_MAP => self.on_node_map(tx, d),
            uds::T_DATA => self.on_data(now, tx, dst, d),
            _ => Vec::new(),
        }
    }

    fn on_beacon(&mut self, now: Instant, tx: Mac, d: &[u8]) -> Vec<Out> {
        let Some(net) = uds::parse_beacon(tx, d) else { return Vec::new() };
        if net.comm_id == MHXX_COMM_ID && net.attributes == MHXX_ATTRIBUTES {
            self.id8 = net.id;
        }
        if let Role::Station(s) = &mut self.role {
            if s.host == tx {
                // (the app data and the slot count; the nodes come from the EAPoL updates once joined)
                s.net.app = net.app.clone();
                s.net.network_id = net.network_id;
                if s.phase != Phase::Joined {
                    s.net.nodes = net.nodes.clone();
                }
            }
        }
        match self.seen.iter_mut().find(|(n, _)| n.host == tx) {
            Some(e) => *e = (net, now),
            None => self.seen.push((net, now)),
        }
        Vec::new()
    }

    fn on_auth(&mut self, now: Instant, tx: Mac, d: &[u8]) -> Vec<Out> {
        if uds::auth_seq(d) != Some(1) {
            return Vec::new(); // (SEQ2 needs nothing: the association response follows)
        }
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        if h.members.iter().flatten().any(|m| m.mac == tx) {
            // the same console again (its game reconnects): what it had is gone
            let _ = self.host_remove(&tx, "reconnecting");
        }
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        let full = 1 + Self::member_count(h) + h.pending.len() >= Self::max_nodes(h);
        if h.ldn[ni::ACCEPT_POLICY] == 1 || full {
            return Vec::new(); // (Azahar's own host says nothing either)
        }
        h.pending.insert(tx, now);
        let network_id = h.network_id;
        vec![
            Out::Frame { ty: uds::T_AUTH, dst: tx, data: uds::auth_frame(2) },
            Out::Frame { ty: uds::T_ASSOC_RESPONSE, dst: tx, data: uds::assoc_response(1, network_id) },
        ]
    }

    fn on_assoc(&mut self, tx: Mac, d: &[u8]) -> Vec<Out> {
        let Role::Station(s) = &mut self.role else { return Vec::new() };
        if tx != s.host || s.phase != Phase::Auth {
            return Vec::new();
        }
        let Some((status, assoc_id)) = uds::parse_assoc(d) else { return Vec::new() };
        if status != 0 {
            self.note("[citra] the host refused the association".into());
            self.role = Role::Idle;
            return Vec::new();
        }
        s.phase = Phase::Eapol;
        let start = uds::eapol_start(assoc_id, &s.me);
        vec![Out::Frame { ty: uds::T_DATA, dst: s.host, data: start }]
    }

    fn on_deauth(&mut self, tx: Mac) -> Vec<Out> {
        match &self.role {
            Role::Station(s) if s.host == tx => {
                let joined = s.phase == Phase::Joined;
                self.note("[citra] the host closed the network".into());
                self.role = Role::Idle;
                if joined { vec![Out::Info(None)] } else { Vec::new() }
            }
            Role::Host(_) => self.host_remove(&tx, "it disconnected"),
            _ => Vec::new(),
        }
    }

    fn on_node_map(&mut self, tx: Mac, d: &[u8]) -> Vec<Out> {
        let Role::Station(s) = &mut self.role else { return Vec::new() };
        if tx != s.host {
            return Vec::new();
        }
        s.macs = uds::parse_node_map(d).into_iter().map(|(m, id)| (id, m)).collect();
        if s.phase == Phase::Joined {
            let (i, _) = self.station_info().unwrap();
            return vec![Out::Info(Some(i))];
        }
        Vec::new()
    }

    fn on_data(&mut self, now: Instant, tx: Mac, dst: Mac, d: &[u8]) -> Vec<Out> {
        let Some(frame) = uds::parse_data(d) else { return Vec::new() };
        match frame {
            Data::Start { node, .. } => self.on_eapol_start(now, tx, node),
            Data::Logoff { assigned, total, max, nodes } => {
                let Role::Station(s) = &mut self.role else { return Vec::new() };
                if tx != s.host {
                    return Vec::new();
                }
                let was = s.phase;
                if was == Phase::Eapol {
                    if assigned == 0 || assigned as usize > uds::MAX_NODES {
                        return Vec::new();
                    }
                    s.my_id = assigned;
                    s.phase = Phase::Joined;
                } else if was != Phase::Joined {
                    return Vec::new();
                }
                Self::station_update(s, &nodes, max, total);
                if was == Phase::Eapol {
                    let (id, n) = (s.my_id, s.nodes.iter().filter(|n| present(n)).count());
                    self.note(format!("[citra] joined the MHXX network as node {id} ({n} players)"));
                }
                let (i, _) = self.station_info().unwrap();
                vec![Out::Info(Some(i))]
            }
            Data::Secure { channel, dest, src, payload } => self.on_secure(now, tx, dst, d, channel, dest, src, payload),
        }
    }

    fn on_eapol_start(&mut self, now: Instant, tx: Mac, node: Node) -> Vec<Out> {
        let Role::Host(h) = &mut self.role else { return Vec::new() };
        if h.pending.remove(&tx).is_none() {
            return Vec::new();
        }
        let max = Self::max_nodes(h);
        // (the lowest free node, as Azahar's host gives it and as a joiner expects; not one of ldn.c's players)
        let Some(i) = (1..max).find(|&i| h.members[i].is_none() && !Self::is_link(h, i as u16 + 1)) else { return Vec::new() };
        let id = i as u16 + 1;
        let node = Node { id, ..node };
        h.members[i] = Some(Member { mac: tx, node, last_rx: now });
        self.note(format!("[citra] {} joined from the room as node {id}", node.name_utf8()));
        self.host_membership(Some((tx, id)), true)
    }

    #[allow(clippy::too_many_arguments)]
    fn on_secure(&mut self, now: Instant, tx: Mac, dst: Mac, raw: &[u8], channel: u8, dest: u16, src: u16, payload: &[u8]) -> Vec<Out> {
        if channel != uds::HEARTBEAT_CHANNEL {
            self.channel = channel;
        }
        let bc = dest == BROADCAST_NODE;
        match &mut self.role {
            Role::Station(s) => {
                if s.phase != Phase::Joined || src == s.my_id || channel == uds::HEARTBEAT_CHANNEL {
                    return Vec::new();
                }
                // only this network's consoles (another network in the same room broadcasts too)
                if tx != s.host && !s.macs.values().any(|m| *m == tx) {
                    return Vec::new();
                }
                if !bc && dest != s.my_id {
                    return Vec::new();
                }
                let subnet = s.pia.subnet();
                let data = s.pia.to_mhgu(payload, dest);
                let dst_ip = if bc { subnet | 0xFF } else { subnet | s.my_id as u32 };
                vec![Out::Packet { src: subnet | src as u32, dst: dst_ip, data }]
            }
            Role::Host(h) => {
                let Some(m) = h.members.iter_mut().flatten().find(|m| m.mac == tx) else { return Vec::new() };
                m.last_rx = now;
                if channel == uds::HEARTBEAT_CHANNEL || src == HOST_NODE {
                    return Vec::new();
                }
                if !bc && dest != HOST_NODE && !Self::is_link(h, dest) {
                    // for another member, sent to us: pass it on (as Azahar's host does)
                    if dst == self.mac {
                        let to = h.members.iter().flatten().find(|m| m.node.id == dest).map(|m| m.mac);
                        if let Some(to) = to {
                            return vec![Out::Frame { ty: uds::T_DATA, dst: to, data: raw.to_vec() }];
                        }
                    }
                    return Vec::new();
                }
                // (to us, to one of ldn.c's players, or to everyone: ldn.c takes it from here)
                let subnet = h.subnet;
                let data = h.pia.to_mhgu(payload, dest);
                let dst_ip = if bc { subnet | 0xFF } else { subnet | dest as u32 };
                vec![Out::Packet { src: subnet | src as u32, dst: dst_ip, data }]
            }
            Role::Idle => Vec::new(),
        }
    }

    /// A game packet into the network: from the local game, or (hosting) from one of ldn.c's players (`src_ip`).
    pub fn send(&mut self, src_ip: u32, dst_ip: u32, dst_port: u16, data: &[u8]) -> Vec<Out> {
        if dst_port != PIA_PORT {
            return Vec::new(); // (the 3DS game has nothing else listening)
        }
        let bc = dst_ip & 0xFF == 0xFF || dst_ip == 0xFFFF_FFFF;
        let dest = if bc { BROADCAST_NODE } else { (dst_ip & 0xFF) as u16 };
        let channel = self.channel;
        let (mac, src, payload) = match &mut self.role {
            Role::Station(s) if s.phase == Phase::Joined => {
                if !bc && dst_ip & 0xFFFF_FF00 != s.pia.subnet() {
                    return Vec::new();
                }
                let mac = if bc {
                    BROADCAST_MAC
                } else if dest == HOST_NODE {
                    s.host
                } else {
                    // (unknown: to the host, which passes it on)
                    s.macs.get(&dest).copied().unwrap_or(s.host)
                };
                (mac, s.my_id, s.pia.to_3ds(data))
            }
            Role::Host(h) => {
                if !bc && dst_ip & 0xFFFF_FF00 != h.subnet {
                    return Vec::new();
                }
                let mac = if bc {
                    BROADCAST_MAC
                } else {
                    match h.members.iter().flatten().find(|m| m.node.id == dest) {
                        Some(m) => m.mac,
                        None => return Vec::new(), // (ldn.c's players are ldn.c's to reach)
                    }
                };
                let from = (src_ip & 0xFF) as u16;
                let src = if src_ip & 0xFFFF_FF00 == h.subnet && Self::is_link(h, from) { from } else { HOST_NODE };
                (mac, src, h.pia.to_3ds(data))
            }
            _ => return Vec::new(),
        };
        vec![self.data(mac, &payload, channel, dest, src)]
    }

    /// Every few tens of milliseconds: beacons, heartbeats, timeouts.
    pub fn tick(&mut self, now: Instant) -> Vec<Out> {
        let mut out = Vec::new();
        match &mut self.role {
            Role::Station(s) => {
                if s.phase != Phase::Joined && now.duration_since(s.started) > JOIN_TIMEOUT {
                    let (phase, host) = (s.phase, s.host);
                    self.role = Role::Idle;
                    self.note(format!("[citra] the host did not answer the join ({phase:?})"));
                    out.push(Out::Frame { ty: uds::T_DEAUTH, dst: host, data: Vec::new() });
                } else if s.phase == Phase::Joined && now.duration_since(s.last_host) > HOST_SILENT {
                    self.note("[citra] nothing from the network's host for a while: the network is gone".into());
                    self.role = Role::Idle;
                    out.push(Out::Info(None));
                } else if s.phase == Phase::Joined && now.duration_since(s.last_heartbeat) >= HEARTBEAT_EVERY {
                    s.last_heartbeat = now;
                    let (host, id) = (s.host, s.my_id);
                    out.push(self.data(host, &[0], uds::HEARTBEAT_CHANNEL, HOST_NODE, id));
                }
            }
            Role::Host(h) => {
                h.pending.retain(|_, at| now.duration_since(*at) < JOIN_TIMEOUT);
                let silent: Vec<Mac> = h
                    .members
                    .iter()
                    .flatten()
                    .filter(|m| now.duration_since(m.last_rx) > MEMBER_SILENT)
                    .map(|m| m.mac)
                    .collect();
                let beacon = h.last_beacon.map(|t| now.duration_since(t) >= BEACON_EVERY).unwrap_or(true);
                let heartbeat = now.duration_since(h.last_heartbeat) >= HEARTBEAT_EVERY;
                if heartbeat {
                    h.last_heartbeat = now;
                }
                let map = now.duration_since(h.last_map) >= NODE_MAP_EVERY;
                for m in silent {
                    out.extend(self.host_remove(&m, "nothing from it for 30 s"));
                }
                if beacon {
                    out.extend(self.host_beacon(now));
                }
                if heartbeat {
                    out.push(self.data(BROADCAST_MAC, &[0], uds::HEARTBEAT_CHANNEL, BROADCAST_NODE, HOST_NODE));
                }
                if map {
                    if let Role::Host(h) = &mut self.role {
                        h.last_map = now;
                        let m = Self::map_entries(h, self.mac);
                        out.push(Out::Frame { ty: uds::T_NODE_MAP, dst: BROADCAST_MAC, data: uds::node_map(&m) });
                    }
                }
            }
            Role::Idle => {}
        }
        out
    }
}

// ================= the driver: the room's thread and a ticker run the core; ldn.c calls in

pub type InfoCb = Option<extern "C" fn(*const u8)>;
pub type PacketCb = Option<extern "C" fn(u32, u32, *const u8, u32)>;

struct Driver {
    core: Option<Core>,
    info: InfoCb,
    packet: PacketCb,
    wifi_channel: u8,
    ticker: bool,
}

static D: Mutex<Driver> = Mutex::new(Driver { core: None, info: None, packet: None, wifi_channel: WIFI_CHANNEL, ticker: false });

macro_rules! netlog {
    ($($a:tt)*) => { eprintln!($($a)*) };
}

fn run(outs: Vec<Out>) {
    if outs.is_empty() {
        return;
    }
    let (info, packet, channel, mac, log) = {
        let mut d = D.lock().unwrap();
        let log = d.core.as_mut().map(|c| std::mem::take(&mut c.log)).unwrap_or_default();
        (d.info, d.packet, d.wifi_channel, d.core.as_ref().map(|c| c.mac), log)
    };
    for l in log {
        netlog!("{l}");
    }
    for o in outs {
        match o {
            Out::Frame { ty, dst, data } => {
                if let Some(mac) = mac {
                    citra::send_frame(ty, channel, mac, dst, &data);
                }
            }
            Out::Info(i) => {
                if let Some(f) = info {
                    f(i.as_ref().map(|v| v.as_ptr()).unwrap_or(std::ptr::null()));
                }
            }
            Out::Packet { src, dst, data } => {
                if let Some(f) = packet {
                    f(src, dst, data.as_ptr(), data.len() as u32);
                }
            }
        }
    }
}

fn with_core<R>(f: impl FnOnce(&mut Core) -> R) -> Option<R> {
    let mut d = D.lock().unwrap();
    d.core.as_mut().map(f)
}

fn flush_log() {
    let log = with_core(|c| std::mem::take(&mut c.log)).unwrap_or_default();
    for l in log {
        netlog!("{l}");
    }
}

extern "C" fn room_frame(ty: u8, _channel: u8, tx: *const u8, dst: *const u8, data: *const u8, len: u32) {
    if tx.is_null() || dst.is_null() {
        return;
    }
    let tx: Mac = unsafe { std::slice::from_raw_parts(tx, 6) }.try_into().unwrap();
    let dst: Mac = unsafe { std::slice::from_raw_parts(dst, 6) }.try_into().unwrap();
    let d = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    let outs = with_core(|c| c.on_frame(Instant::now(), ty, tx, dst, d)).unwrap_or_default();
    run(outs);
}
extern "C" fn room_lost() {
    netlog!("[citra] the room connection ended");
    let outs = with_core(|c| c.room_lost()).unwrap_or_default();
    run(outs);
}
extern "C" fn room_members(macs: *const [u8; 6], n: i32) {
    if macs.is_null() || n <= 0 {
        return;
    }
    let list = unsafe { std::slice::from_raw_parts(macs, n as usize) }.to_vec();
    let outs = with_core(|c| c.room_members(&list)).unwrap_or_default();
    run(outs);
}

fn ticker() {
    loop {
        std::thread::sleep(Duration::from_millis(25));
        let alive = {
            let d = D.lock().unwrap();
            d.core.is_some()
        };
        if !alive {
            D.lock().unwrap().ticker = false;
            return;
        }
        let outs = with_core(|c| c.tick(Instant::now())).unwrap_or_default();
        run(outs);
    }
}

use crate::room::room_nick;

pub fn start(server: &str, nick: &str, password: &str, token: &str) -> Result<(), String> {
    if citra::connected() && with_core(|_| ()).is_some() {
        return Ok(());
    }
    citra::set_hooks(citra::CitraHooks { frame: Some(room_frame), lost: Some(room_lost), members: Some(room_members) });
    let base = room_nick(nick);
    let mut last = String::new();
    for k in 1..=5 {
        let name = if k == 1 { base.clone() } else { room_nick(&format!("{}-{k}", &base[..base.len().min(17)])) };
        match citra::join(server, &name, password, token) {
            Ok(mac) => {
                let entropy = std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .map(|d| d.as_nanos() as u64)
                    .unwrap_or(1)
                    ^ u64::from_le_bytes([mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], 0, 0]);
                let spawn = {
                    let mut d = D.lock().unwrap();
                    d.core = Some(Core::new(mac, entropy));
                    let spawn = !d.ticker;
                    d.ticker = true;
                    spawn
                };
                if spawn {
                    std::thread::spawn(ticker);
                }
                netlog!("[citra] in the room {server} as \"{name}\" ({mac:02x?})");
                return Ok(());
            }
            Err(e) if e.contains("name is taken") => last = e,
            Err(e) => return Err(e),
        }
    }
    Err(last)
}

pub fn stop() {
    let outs = with_core(|c| {
        let mut o = c.leave();
        o.extend(c.host_stop());
        o
    })
    .unwrap_or_default();
    run(outs);
    D.lock().unwrap().core = None;
    citra::leave();
}

// ---- the C API (runtime/net/citra_room.h)

use std::ffi::{c_char, CStr};

fn cstr(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}
fn netinfo<'a>(p: *const u8) -> Option<&'a [u8]> {
    (!p.is_null()).then(|| unsafe { std::slice::from_raw_parts(p, NETINFO) })
}

#[no_mangle]
pub extern "C" fn mhgu_uds_set_hooks(info: InfoCb, packet: PacketCb) {
    let mut d = D.lock().unwrap();
    d.info = info;
    d.packet = packet;
}

#[no_mangle]
pub extern "C" fn mhgu_uds_start(
    server: *const c_char, nick: *const c_char, password: *const c_char, token: *const c_char, err: *mut c_char,
    err_size: i32,
) -> bool {
    match start(&cstr(server), &cstr(nick), &cstr(password), &cstr(token)) {
        Ok(()) => true,
        Err(e) => {
            if !err.is_null() && err_size > 0 {
                let b = e.as_bytes();
                let n = b.len().min(err_size as usize - 1);
                unsafe {
                    std::ptr::copy_nonoverlapping(b.as_ptr(), err as *mut u8, n);
                    *err.add(n) = 0;
                }
            }
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn mhgu_uds_stop() {
    stop();
}

#[no_mangle]
pub extern "C" fn mhgu_uds_ready() -> bool {
    citra::connected() && with_core(|_| ()).is_some()
}

#[no_mangle]
pub extern "C" fn mhgu_uds_mac(mac: *mut u8) {
    let m = with_core(|c| c.mac).unwrap_or([0; 6]);
    if !mac.is_null() {
        unsafe { std::ptr::copy_nonoverlapping(m.as_ptr(), mac, 6) };
    }
}

/// The subnet a network this console hosts uses (its host is .1).
#[no_mangle]
pub extern "C" fn mhgu_uds_subnet() -> u32 {
    with_core(|c| subnet_for(&c.mac)).unwrap_or(0)
}

/// The MHXX networks in the room, as nn::ldn NetworkInfos; returns how many were written.
#[no_mangle]
pub extern "C" fn mhgu_uds_scan(infos: *mut u8, max: i32) -> i32 {
    if infos.is_null() || max <= 0 {
        return 0;
    }
    let list = with_core(|c| c.scan(Instant::now())).unwrap_or_default();
    let n = list.len().min(max as usize);
    for (i, info) in list.iter().take(n).enumerate() {
        unsafe { std::ptr::copy_nonoverlapping(info.as_ptr(), infos.add(i * NETINFO), NETINFO) };
    }
    n as i32
}

/// Joins the MHXX network hosted by `bssid` (blocks up to 5 s). 0: joined, `info` and `ip` filled; 1: not in the
/// room's list; 2: no answer / refused.
#[no_mangle]
pub extern "C" fn mhgu_uds_connect(bssid: *const u8, name: *const u8, info: *mut u8, ip: *mut u32) -> u32 {
    if bssid.is_null() {
        return 1;
    }
    let b: Mac = unsafe { std::slice::from_raw_parts(bssid, 6) }.try_into().unwrap();
    let nm = if name.is_null() {
        String::new()
    } else {
        let f = unsafe { std::slice::from_raw_parts(name, 0x20) };
        let n = f.iter().position(|&c| c == 0).unwrap_or(0x20);
        String::from_utf8_lossy(&f[..n]).into_owned()
    };
    let started = with_core(|c| c.connect(Instant::now(), &b, &nm));
    match started {
        Some(Ok(outs)) => run(outs),
        _ => return 1,
    }
    let end = Instant::now() + JOIN_TIMEOUT + Duration::from_millis(500);
    while Instant::now() < end {
        std::thread::sleep(Duration::from_millis(20));
        let st = with_core(|c| (c.station_phase(), c.station_info()));
        flush_log();
        match st {
            Some((Some(Phase::Joined), Some((i, a)))) => {
                unsafe {
                    if !info.is_null() {
                        std::ptr::copy_nonoverlapping(i.as_ptr(), info, NETINFO);
                    }
                    if !ip.is_null() {
                        *ip = a;
                    }
                }
                return 0;
            }
            Some((None, _)) | None => return 2,
            _ => {}
        }
    }
    let outs = with_core(|c| c.leave()).unwrap_or_default();
    run(outs);
    2
}

/// The network we're in, now (false: none).
#[no_mangle]
pub extern "C" fn mhgu_uds_info(info: *mut u8) -> bool {
    let i = with_core(|c| match &c.role {
        Role::Host(_) => Some(c.host_ldn()),
        Role::Station(_) => c.station_info().map(|(i, _)| i),
        Role::Idle => None,
    })
    .flatten();
    match i {
        Some(i) if !info.is_null() => {
            unsafe { std::ptr::copy_nonoverlapping(i.as_ptr(), info, NETINFO) };
            true
        }
        _ => false,
    }
}

#[no_mangle]
pub extern "C" fn mhgu_uds_leave() {
    let outs = with_core(|c| c.leave()).unwrap_or_default();
    run(outs);
}

#[no_mangle]
pub extern "C" fn mhgu_uds_host(info: *const u8) -> bool {
    let Some(i) = netinfo(info) else { return false };
    let outs = with_core(|c| c.host(Instant::now(), i));
    let ok = outs.is_some();
    run(outs.unwrap_or_default());
    ok
}

#[no_mangle]
pub extern "C" fn mhgu_uds_host_update(info: *const u8) {
    let Some(i) = netinfo(info) else { return };
    let outs = with_core(|c| c.host_update(i)).unwrap_or_default();
    run(outs);
}

#[no_mangle]
pub extern "C" fn mhgu_uds_host_stop() {
    let outs = with_core(|c| c.host_stop()).unwrap_or_default();
    run(outs);
}

#[no_mangle]
pub extern "C" fn mhgu_uds_reject(ip: u32) {
    let outs = with_core(|c| c.reject(ip)).unwrap_or_default();
    run(outs);
}

#[no_mangle]
pub extern "C" fn mhgu_uds_send(src_ip: u32, dst_ip: u32, dst_port: u16, data: *const u8, len: u32) {
    let d = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    let outs = with_core(|c| c.send(src_ip, dst_ip, dst_port, d)).unwrap_or_default();
    run(outs);
}

#[cfg(test)]
mod tests {
    use super::*;

    const ME: Mac = [0x02, 0xAA, 0, 0, 0, 1];
    const MHXX: Mac = [0x40, 0xF4, 0x07, 1, 2, 3];
    const MHXX2: Mac = [0x40, 0xF4, 0x07, 4, 5, 6];

    fn frames(o: &[Out]) -> Vec<(u8, Mac)> {
        o.iter().filter_map(|x| if let Out::Frame { ty, dst, .. } = x { Some((*ty, *dst)) } else { None }).collect()
    }
    fn frame_data(o: &[Out], ty: u8) -> Vec<u8> {
        o.iter()
            .find_map(|x| if let Out::Frame { ty: t, data, .. } = x { (*t == ty).then(|| data.clone()) } else { None })
            .unwrap()
    }
    fn mhxx_net(total: u8) -> Network {
        let mut host = Node { seed: 0x0102_0304_0506_0708, id: 1, ..Default::default() };
        host.set_name("XXHunter");
        let mut nodes = vec![host, Node::default(), Node::default(), Node::default()];
        if total > 1 {
            nodes[1] = Node { seed: 77, id: 2, ..Default::default() };
        }
        Network {
            host: MHXX,
            channel: 11,
            comm_id: MHXX_COMM_ID,
            id: 5,
            attributes: MHXX_ATTRIBUTES,
            network_id: 0x1234_5678,
            total,
            max: 4,
            app: vec![0x11; 200],
            nodes,
        }
    }

    #[test]
    fn joining_an_mhxx_network() {
        let t0 = Instant::now();
        let mut c = Core::new(ME, 99);
        // its beacon lists it
        c.on_frame(t0, uds::T_BEACON, MHXX, BROADCAST_MAC, &uds::build_beacon(&mhxx_net(2)));
        let list = c.scan(t0);
        assert_eq!(list.len(), 1);
        assert_eq!(&list[0][ni::BSSID..ni::BSSID + 6], &MHXX);
        assert_eq!(list[0][ni::NODE_COUNT], 2);
        assert_eq!(c.id8, 5);
        // join: deauth, auth SEQ1
        let o = c.connect(t0, &MHXX, "GuHunter").unwrap();
        assert_eq!(frames(&o), vec![(uds::T_DEAUTH, MHXX), (uds::T_AUTH, MHXX)]);
        // host: SEQ2, association
        assert!(c.on_frame(t0, uds::T_AUTH, MHXX, ME, &uds::auth_frame(2)).is_empty());
        let o = c.on_frame(t0, uds::T_ASSOC_RESPONSE, MHXX, ME, &uds::assoc_response(1, 0x1234_5678));
        let start = frame_data(&o, uds::T_DATA);
        let Some(Data::Start { node, .. }) = uds::parse_data(&start) else { panic!() };
        // the lowest free node is 3: our seed is the one for 169.254.x.3
        let sub = subnet_for(&MHXX);
        assert_eq!(address_of_seed(node.seed), sub | 3);
        assert_eq!(node.name_utf8(), "GuHunter");
        // the host's EAPoL-Logoff: we are node 3
        let mut nodes = mhxx_net(2).nodes;
        nodes[2] = Node { id: 3, ..node };
        let o = c.on_frame(t0, uds::T_DATA, MHXX, ME, &uds::eapol_logoff(ME, 3, &nodes, 4, 3));
        assert!(matches!(&o[..], [Out::Info(Some(_))]));
        let (info, ip) = c.station_info().unwrap();
        assert_eq!(ip, sub | 3);
        assert_eq!(info[ni::NODE_COUNT], 3);
        assert_eq!(ldn_name(&info, 0), "XXHunter");
        // a game packet from the host to everyone
        let o = c.on_frame(t0, uds::T_DATA, MHXX, BROADCAST_MAC, &uds::data_frame(b"pia?", 0xF3, BROADCAST_NODE, 1, 1));
        assert_eq!(o, vec![Out::Packet { src: sub | 1, dst: sub | 0xFF, data: b"pia?".to_vec() }]);
        // ours to the host
        let o = c.send(sub | 3, sub | 1, PIA_PORT, b"hey");
        let d = frame_data(&o, uds::T_DATA);
        assert_eq!(frames(&o), vec![(uds::T_DATA, MHXX)]);
        assert_eq!(uds::parse_data(&d), Some(Data::Secure { channel: 0xF3, dest: 1, src: 3, payload: b"hey" }));
        // a heartbeat goes out
        let o = c.tick(t0 + Duration::from_millis(300));
        assert_eq!(frames(&o), vec![(uds::T_DATA, MHXX)]);
        // another network's broadcast is not ours
        let o = c.on_frame(t0, uds::T_DATA, MHXX2, BROADCAST_MAC, &uds::data_frame(b"x", 0xF3, BROADCAST_NODE, 2, 1));
        assert!(o.is_empty());
        // the host closes it
        let o = c.on_frame(t0, uds::T_DEAUTH, MHXX, BROADCAST_MAC, &[]);
        assert_eq!(o, vec![Out::Info(None)]);
        assert!(c.idle());
    }

    #[test]
    fn a_join_that_gets_no_answer_times_out() {
        let t0 = Instant::now();
        let mut c = Core::new(ME, 1);
        c.on_frame(t0, uds::T_BEACON, MHXX, BROADCAST_MAC, &uds::build_beacon(&mhxx_net(1)));
        c.connect(t0, &MHXX, "x").unwrap();
        let o = c.tick(t0 + Duration::from_secs(6));
        assert_eq!(frames(&o), vec![(uds::T_DEAUTH, MHXX)]);
        assert!(c.idle());
    }

    fn ldn_hosted() -> Vec<u8> {
        let mut i = vec![0u8; NETINFO];
        i[ni::NODE_MAX] = 4;
        i[ni::NODE_COUNT] = 1;
        let sub = subnet_for(&ME);
        let s = node_slot(&mut i, 0);
        s[..4].copy_from_slice(&(sub | 1).to_le_bytes());
        s[ni::N_CONNECTED] = 1;
        s[ni::N_NAME..ni::N_NAME + 6].copy_from_slice(b"Hostie");
        s[ni::N_LCV] = 1;
        let mut adv = vec![0x22u8; 200];
        adv[20..28].copy_from_slice(&pia::MHGU_MAGIC);
        i[ni::ADV_SIZE..ni::ADV_SIZE + 2].copy_from_slice(&200u16.to_le_bytes());
        i[ni::ADV..ni::ADV + 200].copy_from_slice(&adv);
        i
    }

    #[test]
    fn hosting_for_mhxx_players() {
        let t0 = Instant::now();
        let mut c = Core::new(ME, 7);
        let o = c.host(t0, &ldn_hosted());
        let b = uds::parse_beacon(ME, &frame_data(&o, uds::T_BEACON)).unwrap();
        assert_eq!((b.comm_id, b.total, b.max), (MHXX_COMM_ID, 1, 4));
        assert_eq!(b.nodes[0].name_utf8(), "Hostie");
        let sub = subnet_for(&ME);
        assert_eq!(address_of_seed(b.nodes[0].seed), sub | 1);
        assert_eq!(&b.app[20..28], &[0; 8]); // MHGU's magic is not the 3DS's
        // an MHXX player joins
        let o = c.on_frame(t0, uds::T_AUTH, MHXX, ME, &uds::auth_frame(1));
        assert_eq!(frames(&o), vec![(uds::T_AUTH, MHXX), (uds::T_ASSOC_RESPONSE, MHXX)]);
        let mut p = Node { seed: 0xABCD, ..Default::default() };
        p.set_name("Kirin");
        let o = c.on_frame(t0, uds::T_DATA, MHXX, ME, &uds::eapol_start(1, &p));
        assert_eq!(frames(&o), vec![(uds::T_DATA, MHXX), (uds::T_NODE_MAP, BROADCAST_MAC)]);
        let Some(Data::Logoff { assigned, total, .. }) = uds::parse_data(&frame_data(&o, uds::T_DATA)) else { panic!() };
        assert_eq!((assigned, total), (2, 2));
        let Some(Out::Info(Some(info))) = o.last() else { panic!() };
        assert_eq!(info[ni::NODE_COUNT], 2);
        assert_eq!(ldn_name(info, 1), "Kirin");
        let slot = &info[ni::NODES + ni::NODE..ni::NODES + 2 * ni::NODE];
        assert_eq!(&slot[..4], &(sub | 2).to_le_bytes());
        // its packet to us goes to the game
        let o = c.on_frame(t0, uds::T_DATA, MHXX, ME, &uds::data_frame(b"zz", 0xF3, 1, 2, 9));
        assert_eq!(o, vec![Out::Packet { src: sub | 2, dst: sub | 1, data: b"zz".to_vec() }]);
        // the game's to it
        let o = c.send(sub | 1, sub | 2, PIA_PORT, b"yo");
        assert_eq!(frames(&o), vec![(uds::T_DATA, MHXX)]);
        // a second player, then the first leaves the room
        c.on_frame(t0, uds::T_AUTH, MHXX2, ME, &uds::auth_frame(1));
        let o = c.on_frame(t0, uds::T_DATA, MHXX2, ME, &uds::eapol_start(1, &Node { seed: 5, ..Default::default() }));
        let Some(Data::Logoff { assigned, .. }) = uds::parse_data(&frame_data(&o, uds::T_DATA)) else { panic!() };
        assert_eq!(assigned, 3);
        let o = c.room_members(&[ME, MHXX2]);
        let Some(Out::Info(Some(info))) = o.last() else { panic!() };
        assert_eq!(info[ni::NODE_COUNT], 2);
        assert_eq!(info[ni::NODES + ni::NODE + ni::N_CONNECTED], 0);
        // beacons keep coming, and stop with the network
        let o = c.tick(t0 + Duration::from_millis(200));
        assert!(frames(&o).contains(&(uds::T_BEACON, BROADCAST_MAC)));
        let o = c.host_stop();
        assert_eq!(frames(&o).len(), 3);
        assert!(c.tick(t0 + Duration::from_secs(1)).is_empty());
    }

    #[test]
    fn hosting_with_hub_players_too() {
        // ldn.c's own host has a gu-native player in slot 1 (node 2, from the LAN or a hub server)
        let t0 = Instant::now();
        let sub = subnet_for(&ME);
        let mut i = ldn_hosted();
        i[ni::NODE_COUNT] = 2;
        let s = node_slot(&mut i, 1);
        s[..4].copy_from_slice(&(sub | 2).to_le_bytes());
        s[ni::N_MAC..ni::N_MAC + 6].copy_from_slice(&[2, 9, 9, 9, 9, 9]);
        s[ni::N_ID] = 1;
        s[ni::N_CONNECTED] = 1;
        s[ni::N_NAME..ni::N_NAME + 3].copy_from_slice(b"Hub");
        let mut c = Core::new(ME, 5);
        let o = c.host(t0, &i);
        let b = uds::parse_beacon(ME, &frame_data(&o, uds::T_BEACON)).unwrap();
        assert_eq!(b.total, 2);
        assert_eq!((b.nodes[1].id, b.nodes[1].name_utf8()), (2, "Hub".to_string()));
        assert_eq!(address_of_seed(b.nodes[1].seed), sub | 2);
        // an MHXX player gets node 3, past the hub player
        c.on_frame(t0, uds::T_AUTH, MHXX, ME, &uds::auth_frame(1));
        let o = c.on_frame(t0, uds::T_DATA, MHXX, ME, &uds::eapol_start(1, &Node { seed: 0xAB, ..Default::default() }));
        let Some(Data::Logoff { assigned, total, nodes, .. }) = uds::parse_data(&frame_data(&o, uds::T_DATA)) else { panic!() };
        assert_eq!((assigned, total, nodes[1].id), (3, 3, 2));
        let map = uds::parse_node_map(&frame_data(&o, uds::T_NODE_MAP));
        assert!(map.contains(&(ME, 2))); // (the hub player is reached through us)
        let Some(Out::Info(Some(info))) = o.last() else { panic!() };
        assert_eq!(info[ni::NODE_COUNT], 2); // (us and our MHXX player: ldn.c keeps its own)
        assert_eq!(info[ni::NODES + ni::NODE + ni::N_CONNECTED], 0);
        assert_eq!(info[ni::NODES + 2 * ni::NODE + ni::N_CONNECTED], 1);
        // MHXX -> the hub player: handed to ldn.c for it
        let o = c.on_frame(t0, uds::T_DATA, MHXX, ME, &uds::data_frame(b"to hub", 0xF3, 2, 3, 1));
        assert_eq!(o, vec![Out::Packet { src: sub | 3, dst: sub | 2, data: b"to hub".to_vec() }]);
        // the hub player -> MHXX: sent as node 2
        let o = c.send(sub | 2, sub | 3, PIA_PORT, b"hi");
        assert_eq!(uds::parse_data(&frame_data(&o, uds::T_DATA)), Some(Data::Secure { channel: 0xF3, dest: 3, src: 2, payload: b"hi" }));
        assert!(c.send(sub | 1, sub | 2, PIA_PORT, b"x").is_empty()); // (ldn.c reaches its own players)
        // the hub player leaves: the MHXX player is told, ldn.c gets nothing back
        let mut gone = i.clone();
        node_slot(&mut gone, 1).fill(0);
        gone[ni::NODE_COUNT] = 1;
        let o = c.host_update(&gone);
        assert_eq!(frames(&o), vec![(uds::T_DATA, MHXX), (uds::T_NODE_MAP, BROADCAST_MAC)]);
        let Some(Data::Logoff { total, .. }) = uds::parse_data(&frame_data(&o, uds::T_DATA)) else { panic!() };
        assert_eq!(total, 2);
        assert!(c.host_update(&gone).is_empty());
    }

    #[test]
    fn a_closed_or_full_network_ignores_joins() {
        let t0 = Instant::now();
        let mut c = Core::new(ME, 3);
        let mut i = ldn_hosted();
        i[ni::NODE_MAX] = 2;
        c.host(t0, &i);
        c.on_frame(t0, uds::T_AUTH, MHXX, ME, &uds::auth_frame(1));
        c.on_frame(t0, uds::T_DATA, MHXX, ME, &uds::eapol_start(1, &Node::default()));
        assert!(c.on_frame(t0, uds::T_AUTH, MHXX2, ME, &uds::auth_frame(1)).is_empty());
        let mut c = Core::new(ME, 3);
        let mut i = ldn_hosted();
        i[ni::ACCEPT_POLICY] = 1;
        c.host(t0, &i);
        assert!(c.on_frame(t0, uds::T_AUTH, MHXX, ME, &uds::auth_frame(1)).is_empty());
    }

    #[test]
    fn two_phones_in_one_network() {
        // one gu-native hosts, another joins it through the room: both translate, and it comes out the same
        let t0 = Instant::now();
        let other: Mac = [0x02, 0xBB, 0, 0, 0, 2];
        let mut host = Core::new(ME, 11);
        let mut guest = Core::new(other, 12);
        let mut queue: Vec<(Mac, Mac, Out)> = Vec::new();
        let push = |q: &mut Vec<(Mac, Mac, Out)>, from: Mac, o: Vec<Out>| {
            for x in o {
                q.push((from, [0; 6], x));
            }
        };
        let o = host.host(t0, &ldn_hosted());
        push(&mut queue, ME, o);
        let deliver = |q: &mut Vec<(Mac, Mac, Out)>, host: &mut Core, guest: &mut Core| {
            let mut got = Vec::new();
            while let Some((from, _, o)) = (!q.is_empty()).then(|| q.remove(0)) {
                if let Out::Frame { ty, dst, data } = o {
                    let to: &mut Core = if from == ME { &mut *guest } else { &mut *host };
                    let mac = to.mac;
                    if dst == mac || dst == BROADCAST_MAC {
                        let out = to.on_frame(t0, ty, from, dst, &data);
                        for x in out {
                            match x {
                                Out::Frame { .. } => q.push((mac, [0; 6], x)),
                                other => got.push((mac, other)),
                            }
                        }
                    }
                }
            }
            got
        };
        deliver(&mut queue, &mut host, &mut guest);
        assert_eq!(guest.scan(t0).len(), 1);
        let o = guest.connect(t0, &ME, "Guest").unwrap();
        push(&mut queue, other, o);
        deliver(&mut queue, &mut host, &mut guest);
        let (_, ip) = guest.station_info().unwrap();
        let sub = subnet_for(&ME);
        assert_eq!(ip, sub | 2);
        // a Pia packet naming the host's address goes host -> guest unchanged in the end
        let mut pk = vec![0x32, 0xAB, 0x98, 0x64, 1, 0, 0, 0, 0, 0, 0, 0];
        pk.extend_from_slice(&[0, 1, 0, 4, 0, 0, 0, 0]);
        pk.extend_from_slice(&(sub | 1).to_be_bytes());
        pk.extend_from_slice(&[0x02, 0, 0, 0, 0, 0, 0, 0]);
        pk.extend_from_slice(&(sub | 1).to_be_bytes());
        let o = host.send(sub | 1, sub | 2, PIA_PORT, &pk);
        push(&mut queue, ME, o);
        let got = deliver(&mut queue, &mut host, &mut guest);
        assert_eq!(got, vec![(other, Out::Packet { src: sub | 1, dst: sub | 2, data: pk })]);
    }
}
