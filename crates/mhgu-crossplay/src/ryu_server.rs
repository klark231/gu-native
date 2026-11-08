//! RyuLdn server: the Ryujinx/suyu LDN master server, in Rust.
//! Ported from runtime/net/ryusrv/{protocol.h,server.cpp}. Clients keep one TCP connection; the
//! server owns the lobbies (virtual 10.114.0.0/16) and relays game traffic between members.

use std::collections::{HashMap, HashSet};
use std::io::{Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub const MAGIC: u32 = u32::from_le_bytes(*b"RLDN");
pub const PROTOCOL_VERSION: u8 = 1;
pub const DEFAULT_PORT: u16 = 30456;
const MAX_PACKET: usize = 131072;
const NETWORK_BASE: u32 = 0x0A72_0000;
const SUBNET_MASK: u32 = 0xFFFF_0000;
const BROADCAST: u32 = NETWORK_BASE | !SUBNET_MASK;
const ALT_BROADCAST: u32 = 0xC0A8_00FF;
const NETINFO: usize = 0x480;
const NODE: usize = 0x40;

pub mod msg {
    pub const INITIALIZE: u8 = 0;
    pub const PASSPHRASE: u8 = 1;
    pub const CREATE: u8 = 2;
    pub const CREATE_PRIVATE: u8 = 3;
    pub const SYNC_NETWORK: u8 = 7;
    pub const REJECT: u8 = 8;
    pub const REJECT_REPLY: u8 = 9;
    pub const SCAN: u8 = 10;
    pub const SCAN_REPLY: u8 = 11;
    pub const SCAN_REPLY_END: u8 = 12;
    pub const CONNECT: u8 = 13;
    pub const CONNECT_PRIVATE: u8 = 14;
    pub const CONNECTED: u8 = 15;
    pub const DISCONNECT: u8 = 16;
    pub const PROXY_CONFIG: u8 = 17;
    pub const PROXY_CONNECT: u8 = 18;
    pub const PROXY_CONNECT_REPLY: u8 = 19;
    pub const PROXY_DATA: u8 = 20;
    pub const PROXY_DISCONNECT: u8 = 21;
    pub const SET_ACCEPT_POLICY: u8 = 22;
    pub const SET_ADVERTISE: u8 = 23;
    pub const PING: u8 = 254;
    pub const NETWORK_ERROR: u8 = 255;
}
pub mod err {
    pub const PORT_UNREACHABLE: u32 = 1;
    pub const TOO_MANY_PLAYERS: u32 = 2;
    pub const VERSION_TOO_LOW: u32 = 3;
    pub const VERSION_TOO_HIGH: u32 = 4;
    pub const CONNECT_FAILURE: u32 = 5;
    pub const CONNECT_NOT_FOUND: u32 = 6;
    pub const REJECT_FAILED: u32 = 9;
    pub const UNKNOWN: u32 = u32::MAX;
}

mod info {
    pub const LCID: usize = 0x00;
    pub const SCENE_ID: usize = 0x0A;
    pub const SESSION_ID: usize = 0x10;
    pub const BSSID: usize = 0x20;
    pub const SSID_LENGTH: usize = 0x26;
    pub const SSID_NAME: usize = 0x27;
    pub const CHANNEL: usize = 0x48;
    pub const LINK_LEVEL: usize = 0x4A;
    pub const NETWORK_TYPE: usize = 0x4B;
    pub const SECURITY_MODE: usize = 0x60;
    pub const ACCEPT_POLICY: usize = 0x62;
    pub const NODE_COUNT_MAX: usize = 0x66;
    pub const NODE_COUNT: usize = 0x67;
    pub const NODES: usize = 0x68;
    pub const ADVERT_SIZE: usize = 0x26A;
    pub const ADVERT: usize = 0x26C;
    pub const ADVERT_MAX: usize = 0x180;
}
mod nodeoff {
    pub const IP: usize = 0x00;
    pub const MAC: usize = 0x04;
    pub const ID: usize = 0x0A;
    pub const CONNECTED: usize = 0x0B;
    pub const NAME: usize = 0x0C;
    pub const LCV: usize = 0x2E;
}
mod cfg {
    pub const LCID: usize = 0x00;
    pub const SCENE_ID: usize = 0x0A;
    pub const CHANNEL: usize = 0x10;
    pub const NODE_COUNT_MAX: usize = 0x12;
    pub const LCV: usize = 0x14;
}
mod filter {
    pub const LCID: usize = 0x00;
    pub const SCENE_ID: usize = 0x0A;
    pub const SESSION_ID: usize = 0x10;
    pub const NETWORK_TYPE: usize = 0x20;
    pub const SSID_LENGTH: usize = 0x2A;
    pub const SSID_NAME: usize = 0x2B;
    pub const FLAG: usize = 0x5C;
    pub const FLAG_LCID: u32 = 1 << 0;
    pub const FLAG_SESSION_ID: u32 = 1 << 1;
    pub const FLAG_NETWORK_TYPE: u32 = 1 << 2;
    pub const FLAG_SSID: u32 = 1 << 4;
    pub const FLAG_SCENE_ID: u32 = 1 << 5;
}

fn rd32(d: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
}
fn rd16(d: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([d[at], d[at + 1]])
}
fn wr16(d: &mut [u8], at: usize, v: u16) {
    d[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
fn node(info: &[u8], i: usize) -> &[u8] {
    &info[info::NODES + i * NODE..info::NODES + (i + 1) * NODE]
}
fn node_mut(info: &mut [u8], i: usize) -> &mut [u8] {
    &mut info[info::NODES + i * NODE..info::NODES + (i + 1) * NODE]
}

fn encode(ty: u8, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(12 + payload.len());
    v.extend_from_slice(&MAGIC.to_le_bytes());
    v.push(ty);
    v.push(PROTOCOL_VERSION);
    v.extend_from_slice(&[0, 0]);
    v.extend_from_slice(&(payload.len() as i32).to_le_bytes());
    v.extend_from_slice(payload);
    v
}
fn log_on() -> bool {
    std::env::var_os("MHGU_LOG_NET").is_some()
}
macro_rules! netlog {
    ($($a:tt)*) => { if log_on() { eprintln!($($a)*); } };
}

struct Io {
    id: [u8; 16],
    stream: Mutex<Option<TcpStream>>,
    closed: AtomicBool,
}

impl Io {
    fn send(&self, ty: u8, payload: &[u8]) {
        if self.closed.load(Ordering::SeqCst) {
            return;
        }
        let bytes = encode(ty, payload);
        let mut g = self.stream.lock().unwrap();
        if let Some(s) = g.as_mut() {
            if s.write_all(&bytes).is_err() {
                self.closed.store(true, Ordering::SeqCst);
                let _ = s.shutdown(Shutdown::Both);
            }
        }
    }
    fn close(&self) {
        if !self.closed.swap(true, Ordering::SeqCst) {
            let mut g = self.stream.lock().unwrap();
            if let Some(s) = g.as_mut() {
                let _ = s.shutdown(Shutdown::Both);
            }
        }
    }
}

#[derive(Default)]
struct SessState {
    mac: [u8; 6],
    passphrase: String,
    initialized: bool,
    ip: u32,
    node_id: i32,
    game: Option<[u8; 16]>,
    bcast_logged: u32,
}

struct Game {
    info: [u8; NETINFO],
    owner: u64,
    passphrase: String,
    players: Vec<u64>,
    created: Instant,
    next_ip: u32,
    taken: HashSet<u32>,
    reserved: HashMap<[u8; 6], u32>,
}

struct State {
    sessions: HashMap<u64, SessState>,
    ios: HashMap<u64, Arc<Io>>,
    games: HashMap<[u8; 16], Game>,
    used_macs: HashSet<[u8; 6]>,
    seed: u64,
}

impl State {
    fn rand(&mut self) -> u64 {
        // xorshift64*: enough for ids/MACs, no dependency.
        let mut x = self.seed;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.seed = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }
    fn new_mac(&mut self) -> [u8; 6] {
        loop {
            let r = self.rand();
            let mut m = [0u8; 6];
            m.copy_from_slice(&r.to_le_bytes()[..6]);
            if m != [0; 6] && !self.used_macs.contains(&m) {
                self.used_macs.insert(m);
                return m;
            }
        }
    }
    fn send(&self, serial: u64, ty: u8, payload: &[u8]) {
        if let Some(io) = self.ios.get(&serial) {
            io.send(ty, payload);
        }
    }
    fn send_error(&self, serial: u64, error: u32) {
        netlog!("[ryusrv] client #{} gets error {}", serial, error);
        self.send(serial, msg::NETWORK_ERROR, &error.to_le_bytes());
    }
    fn broadcast_info(&self, id: &[u8; 16]) {
        let Some(g) = self.games.get(id) else { return };
        for p in &g.players {
            self.send(*p, msg::SYNC_NETWORK, &g.info);
        }
    }
    fn cycle_ip(g: &mut Game) {
        loop {
            g.next_ip = NETWORK_BASE | ((g.next_ip.wrapping_add(1)) & !SUBNET_MASK);
            if g.next_ip != NETWORK_BASE && g.next_ip != BROADCAST {
                return;
            }
        }
    }
    fn request_ip(g: &mut Game, mac: &[u8; 6]) -> u32 {
        if let Some(ip) = g.reserved.get(mac) {
            return *ip;
        }
        while g.taken.contains(&g.next_ip) {
            Self::cycle_ip(g);
        }
        let ip = g.next_ip;
        g.taken.insert(ip);
        Self::cycle_ip(g);
        ip
    }
    fn join_game(&mut self, id: [u8; 16], serial: u64, node_info: &[u8]) -> bool {
        let Some(io) = self.ios.get(&serial).cloned() else { return false };
        let Some(s) = self.sessions.get(&serial) else { return false };
        let mac = s.mac;
        let Some(g) = self.games.get_mut(&id) else { return false };
        let count = g.info[info::NODE_COUNT];
        if count >= g.info[info::NODE_COUNT_MAX] {
            return false;
        }
        let mut slot = 8;
        for i in 0..8 {
            if node(&g.info, i)[nodeoff::CONNECTED] == 0 {
                slot = i;
                break;
            }
        }
        if slot >= 8 {
            return false;
        }
        let ip = Self::request_ip(g, &mac);
        self.sessions.get_mut(&serial).unwrap().ip = ip;
        io.send(msg::PROXY_CONFIG, &{
            let mut b = Vec::with_capacity(8);
            b.extend_from_slice(&ip.to_le_bytes());
            b.extend_from_slice(&SUBNET_MASK.to_le_bytes());
            b
        });
        {
            let nd = node_mut(&mut g.info, slot);
            nd[nodeoff::IP..nodeoff::IP + 4].copy_from_slice(&ip.to_le_bytes());
            nd[nodeoff::MAC..nodeoff::MAC + 6].copy_from_slice(&mac);
            nd[nodeoff::ID] = slot as u8;
            nd[nodeoff::CONNECTED] = 1;
            nd[nodeoff::NAME..nodeoff::NAME + 0x21]
                .copy_from_slice(&node_info[nodeoff::NAME..nodeoff::NAME + 0x21]);
            nd[nodeoff::LCV..nodeoff::LCV + 2]
                .copy_from_slice(&node_info[nodeoff::LCV..nodeoff::LCV + 2]);
            let _ = node_info;
        }
        g.info[info::NODE_COUNT] = count + 1;
        self.sessions.get_mut(&serial).unwrap().node_id = slot as i32;
        self.broadcast_info(&id);
        let info = {
            let g = self.games.get_mut(&id).unwrap();
            g.players.push(serial);
            g.info
        };
        self.sessions.get_mut(&serial).unwrap().game = Some(id);
        io.send(msg::CONNECTED, &info);
        netlog!("[ryusrv] client #{} joined as node {} ({} bytes info)", serial, slot, info.len());
        true
    }
    fn leave_game(&mut self, serial: u64, tell_self: bool) {
        let Some(s) = self.sessions.get_mut(&serial) else { return };
        let Some(id) = s.game.take() else { return };
        let ip = s.ip;
        s.ip = 0;
        s.node_id = -1;
        let mut owner = false;
        if let Some(g) = self.games.get_mut(&id) {
            g.players.retain(|p| *p != serial);
            if !g.reserved.values().any(|v| *v == ip) {
                g.taken.remove(&ip);
            }
            for i in 0..8 {
                let nd = node_mut(&mut g.info, i);
                if nd[nodeoff::CONNECTED] != 0 && rd32(nd, nodeoff::IP) == ip {
                    nd[nodeoff::CONNECTED] = 0;
                    g.info[info::NODE_COUNT] = g.info[info::NODE_COUNT].saturating_sub(1);
                }
            }
            owner = g.owner == serial;
        }
        if tell_self {
            self.send(serial, msg::DISCONNECT, &[0, 0, 0, 0]);
        }
        self.broadcast_info(&id);
        if owner {
            self.close_game(&id);
        }
    }
    fn close_game(&mut self, id: &[u8; 16]) {
        let Some(g) = self.games.remove(id) else { return };
        for p in &g.players {
            self.send(*p, msg::DISCONNECT, &[0, 0, 0, 0]);
            if let Some(s) = self.sessions.get_mut(p) {
                s.ip = 0;
                s.node_id = -1;
                s.game = None;
            }
        }
        netlog!("[ryusrv] lobby closed ({} open)", self.games.len());
    }

    fn handle(&mut self, serial: u64, ty: u8, data: &[u8]) {
        match ty {
            msg::INITIALIZE => self.handle_initialize(serial, data),
            msg::PASSPHRASE => self.handle_passphrase(serial, data),
            msg::CREATE => self.handle_create(serial, data, false),
            msg::CREATE_PRIVATE => self.handle_create(serial, data, true),
            msg::CONNECT => self.handle_connect(serial, data, false),
            msg::CONNECT_PRIVATE => self.handle_connect(serial, data, true),
            msg::SCAN => self.handle_scan(serial, data),
            msg::DISCONNECT => self.leave_game(serial, false),
            msg::REJECT => self.handle_reject(serial, data),
            msg::SET_ACCEPT_POLICY => self.handle_accept(serial, data),
            msg::SET_ADVERTISE => self.handle_advertise(serial, data),
            msg::PROXY_DATA | msg::PROXY_CONNECT | msg::PROXY_CONNECT_REPLY | msg::PROXY_DISCONNECT => {
                self.route(serial, ty, data)
            }
            msg::PING => {}
            _ => {}
        }
    }

    fn handle_initialize(&mut self, serial: u64, data: &[u8]) {
        if data.len() < 0x16 {
            return;
        }
        if self.sessions.get(&serial).map(|s| s.initialized).unwrap_or(true) {
            return;
        }
        let mac = self.new_mac();
        {
            let Some(s) = self.sessions.get_mut(&serial) else { return };
            s.mac = mac;
            s.initialized = true;
        }
        let id = self.ios.get(&serial).map(|i| i.id).unwrap_or([0; 16]);
        let mut reply = [0u8; 22];
        reply[..16].copy_from_slice(&id);
        reply[16..22].copy_from_slice(&mac);
        self.send(serial, msg::INITIALIZE, &reply);
    }
    fn handle_passphrase(&mut self, serial: u64, data: &[u8]) {
        if data.len() < 0x80 {
            return;
        }
        let end = data[..0x80].iter().position(|b| *b == 0).unwrap_or(0x80);
        let text = String::from_utf8_lossy(&data[..end]).to_string();
        let valid = text.is_empty()
            || (text.len() == 16
                && text.starts_with("Ryujinx-")
                && text[8..].bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)));
        if let Some(s) = self.sessions.get_mut(&serial) {
            s.passphrase = if valid { text } else { String::new() };
        }
    }
    fn handle_create(&mut self, serial: u64, data: &[u8], private: bool) {
        let base = if private { 0x13C } else { 0xBC };
        if data.len() < base {
            return;
        }
        let Some(s) = self.sessions.get(&serial) else { return };
        if !s.initialized || s.game.is_some() {
            self.send_error(serial, err::UNKNOWN);
            return;
        }
        let mac = s.mac;
        let pass = s.passphrase.clone();
        let sec = &data[..0x44];
        let _ = &sec;
        let (user_off, net_off, ryu_off, addr_off) = if private {
            (0x64, 0x94, 0x114, Some(0xB4))
        } else {
            (0x44, 0x74, 0x94, None)
        };
        let mut id = [0u8; 16];
        if private {
            id.copy_from_slice(&data[0x44 + 0x10..0x44 + 0x20]);
        } else {
            let (a, b) = (self.rand(), self.rand());
            id[..8].copy_from_slice(&a.to_le_bytes());
            id[8..].copy_from_slice(&b.to_le_bytes());
        }
        if let Some(g) = self.games.get(&id) {
            if g.owner == serial {
                self.close_game(&id);
            } else {
                self.send_error(serial, err::UNKNOWN);
                return;
            }
        }
        if rd16(&data[ryu_off + 0x22..], 0) != 0 {
            // External proxy port: we relay everything instead.
            self.send_error(serial, err::PORT_UNREACHABLE);
        }
        let mut info = [0u8; NETINFO];
        info[info::LCID..info::LCID + 8].copy_from_slice(&data[net_off + cfg::LCID..net_off + cfg::LCID + 8]);
        info[info::SCENE_ID..info::SCENE_ID + 2].copy_from_slice(&data[net_off + cfg::SCENE_ID..net_off + cfg::SCENE_ID + 2]);
        info[info::SESSION_ID..info::SESSION_ID + 16].copy_from_slice(&id);
        info[info::BSSID..info::BSSID + 6].copy_from_slice(&mac);
        info[info::SSID_LENGTH] = 32;
        info[info::SSID_NAME..info::SSID_NAME + 32].copy_from_slice(b"12345678123456781234567812345678");
        info[info::CHANNEL..info::CHANNEL + 2].copy_from_slice(&data[net_off + cfg::CHANNEL..net_off + cfg::CHANNEL + 2]);
        info[info::LINK_LEVEL] = 3;
        info[info::NETWORK_TYPE] = 2;
        info[info::SECURITY_MODE..info::SECURITY_MODE + 2].copy_from_slice(&sec[..2]);
        info[info::NODE_COUNT_MAX] = data[net_off + cfg::NODE_COUNT_MAX].min(8);
        let adv = &data[base..];
        let n = adv.len().min(info::ADVERT_MAX);
        wr16(&mut info, info::ADVERT_SIZE, n as u16);
        info[info::ADVERT..info::ADVERT + n].copy_from_slice(&adv[..n]);

        let mut reserved = HashMap::new();
        let mut taken = HashSet::new();
        if let Some(a) = addr_off {
            for i in 0..8 {
                let ip = rd32(&data[a + i * 12..], 0);
                if ip == 0 {
                    break;
                }
                let mac: [u8; 6] = data[a + i * 12 + 4..a + i * 12 + 10].try_into().unwrap();
                reserved.insert(mac, ip);
                taken.insert(ip);
            }
        }
        let node_info = &data[user_off..user_off + 0x30];
        let mut node = [0u8; NODE];
        node[nodeoff::NAME..nodeoff::NAME + 0x21].copy_from_slice(&node_info[..0x21]);
        node[nodeoff::LCV..nodeoff::LCV + 2].copy_from_slice(&data[net_off + cfg::LCV..net_off + cfg::LCV + 2]);
        self.games.insert(id, Game {
            info,
            owner: serial,
            passphrase: pass,
            players: Vec::new(),
            created: Instant::now(),
            next_ip: NETWORK_BASE + 1,
            taken,
            reserved,
        });
        if !self.join_game(id, serial, &node) {
            self.games.remove(&id);
            self.send_error(serial, err::UNKNOWN);
            return;
        }
        netlog!("[ryusrv] lobby opened ({} open)", self.games.len());
    }
    fn handle_connect(&mut self, serial: u64, data: &[u8], private: bool) {
        let base = if private { 0xC0 } else { 0x500 };
        if data.len() < base {
            return;
        }
        let Some(s) = self.sessions.get(&serial) else { return };
        if !s.initialized {
            self.send_error(serial, err::CONNECT_FAILURE);
            return;
        }
        let (id_off, lcv_off, user_off) = if private { (0x54, 0x94, 0x64) } else { (0x90, 0x74, 0x44) };
        let id: [u8; 16] = data[id_off..id_off + 16].try_into().unwrap();
        let lcv = rd32(data, lcv_off) as u16;
        let host_lcv = {
            let Some(g) = self.games.get(&id) else {
                self.send_error(serial, err::CONNECT_NOT_FOUND);
                return;
            };
            rd16(node(&g.info, 0), nodeoff::LCV)
        };
        if lcv > host_lcv {
            self.send_error(serial, err::VERSION_TOO_HIGH);
            return;
        }
        if lcv < host_lcv {
            self.send_error(serial, err::VERSION_TOO_LOW);
            return;
        }
        let mut node = [0u8; NODE];
        node[nodeoff::NAME..nodeoff::NAME + 0x21].copy_from_slice(&data[user_off..user_off + 0x21]);
        node[nodeoff::LCV..nodeoff::LCV + 2].copy_from_slice(&lcv.to_le_bytes());
        self.leave_game(serial, false);
        if !self.join_game(id, serial, &node) {
            self.send_error(serial, err::TOO_MANY_PLAYERS);
        }
    }
    fn handle_scan(&mut self, serial: u64, data: &[u8]) {
        if data.len() < 0x60 {
            return;
        }
        let Some(pass) = self.sessions.get(&serial).map(|s| s.passphrase.clone()) else { return };
        let flag = rd32(data, filter::FLAG);
        let now = Instant::now();
        let mut replies: Vec<[u8; NETINFO]> = Vec::new();
        for g in self.games.values() {
            let i = &g.info;
            if g.passphrase != pass
                || g.players.is_empty()
                || now.duration_since(g.created) > Duration::from_secs(16 * 3600)
                || i[info::ACCEPT_POLICY] == 1
            {
                continue;
            }
            if flag & filter::FLAG_LCID != 0
                && rd32(i, info::LCID) != rd32(data, filter::LCID)
            {
                continue;
            }
            if flag & filter::FLAG_SCENE_ID != 0 && rd16(i, info::SCENE_ID) != rd16(data, filter::SCENE_ID) {
                continue;
            }
            if flag & filter::FLAG_SESSION_ID != 0
                && i[info::SESSION_ID..info::SESSION_ID + 16] != data[filter::SESSION_ID..filter::SESSION_ID + 16]
            {
                continue;
            }
            if flag & filter::FLAG_SSID != 0 {
                let len = data[filter::SSID_LENGTH].min(32) as usize;
                if i[info::SSID_LENGTH] as usize != len
                    || i[info::SSID_NAME..info::SSID_NAME + len] != data[filter::SSID_NAME..filter::SSID_NAME + len]
                {
                    continue;
                }
            }
            if flag & filter::FLAG_NETWORK_TYPE != 0
                && i[info::NETWORK_TYPE] as u32 != rd32(data, filter::NETWORK_TYPE)
            {
                continue;
            }
            replies.push(*i);
        }
        for r in replies {
            self.send(serial, msg::SCAN_REPLY, &r);
        }
        self.send(serial, msg::SCAN_REPLY_END, &[]);
    }
    fn handle_reject(&mut self, serial: u64, data: &[u8]) {
        if data.len() < 8 {
            return;
        }
        let node_id = rd32(data, 0);
        let mut target = None;
        if let Some(id) = self.sessions.get(&serial).and_then(|s| s.game) {
            if let Some(g) = self.games.get(&id) {
                if g.owner == serial {
                    for p in &g.players {
                        if self.sessions.get(p).map(|s| s.node_id == node_id as i32).unwrap_or(false) {
                            target = Some(*p);
                        }
                    }
                }
            }
        }
        if let Some(t) = target {
            self.leave_game(t, true);
        } else {
            self.send_error(serial, err::REJECT_FAILED);
        }
        self.send(serial, msg::REJECT_REPLY, &[]);
    }
    fn handle_accept(&mut self, serial: u64, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        let Some(id) = self.sessions.get(&serial).and_then(|s| s.game) else { return };
        let owner = self.games.get(&id).map(|g| g.owner == serial).unwrap_or(false);
        if owner {
            let policy = data[0];
            if let Some(g) = self.games.get_mut(&id) {
                g.info[info::ACCEPT_POLICY] = policy;
            }
            self.broadcast_info(&id);
        }
    }
    fn handle_advertise(&mut self, serial: u64, data: &[u8]) {
        let Some(id) = self.sessions.get(&serial).and_then(|s| s.game) else { return };
        let owner = self.games.get(&id).map(|g| g.owner == serial).unwrap_or(false);
        if !owner {
            return;
        }
        if let Some(g) = self.games.get_mut(&id) {
            let n = data.len().min(info::ADVERT_MAX);
            wr16(&mut g.info, info::ADVERT_SIZE, n as u16);
            g.info[info::ADVERT..info::ADVERT + info::ADVERT_MAX].fill(0);
            g.info[info::ADVERT..info::ADVERT + n].copy_from_slice(&data[..n]);
        }
        self.broadcast_info(&id);
    }
    fn route(&mut self, serial: u64, ty: u8, data: &[u8]) {
        if data.len() < 0x10 {
            return;
        }
        let Some(sid) = self.sessions.get(&serial).and_then(|s| s.game) else { return };
        let ip = self.sessions.get(&serial).map(|s| s.ip).unwrap_or(0);
        let mut src = rd32(data, 0);
        if src == 0 {
            src = ip;
        } else if src != ip {
            return;
        }
        let mut dest = rd32(data, 6);
        if dest == ALT_BROADCAST || dest == 0xFFFF_FFFF {
            dest = BROADCAST;
        }
        if dest == BROADCAST {
            let s = self.sessions.get_mut(&serial).unwrap();
            if s.bcast_logged < 10 {
                s.bcast_logged += 1;
                netlog!("[ryusrv] broadcast from {} ({} bytes)", src, data.len());
            }
        }
        let mut packet = Vec::with_capacity(data.len());
        packet.extend_from_slice(&src.to_le_bytes());
        packet.extend_from_slice(&data[4..6]);
        packet.extend_from_slice(&dest.to_le_bytes());
        packet.extend_from_slice(&data[10..]);
        let targets: Vec<u64> = {
            let Some(g) = self.games.get(&sid) else { return };
            g.players
                .iter()
                .filter(|p| {
                    if dest == BROADCAST {
                        true
                    } else {
                        self.sessions.get(p).map(|s| s.ip == dest).unwrap_or(false)
                    }
                })
                .copied()
                .collect()
        };
        for t in targets {
            self.send(t, ty, &packet);
        }
    }
}

pub struct Status {
    pub running: bool,
    pub port: u16,
    pub connections: usize,
    pub lobbies: usize,
    pub players: usize,
}

pub struct Server {
    running: Arc<AtomicBool>,
    port: u16,
    state: Arc<Mutex<State>>,
    accept: Option<std::thread::JoinHandle<()>>,
}

impl Server {
    pub fn start(address: &str, port: u16) -> Result<Server, String> {
        let listener = TcpListener::bind((address, port)).map_err(|e| e.to_string())?;
        listener.set_nonblocking(true).map_err(|e| e.to_string())?;
        let port = listener.local_addr().map_err(|e| e.to_string())?.port();
        let state = Arc::new(Mutex::new(State {
            sessions: HashMap::new(),
            ios: HashMap::new(),
            games: HashMap::new(),
            used_macs: HashSet::new(),
            seed: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(0x9E37_79B9_7F4A_7C15)
                | 1,
        }));
        let running = Arc::new(AtomicBool::new(true));
        let accept = {
            let running = Arc::clone(&running);
            let state = Arc::clone(&state);
            std::thread::spawn(move || accept_loop(listener, running, state))
        };
        eprintln!("[ryusrv] listening on {address}:{port}");
        Ok(Server { running, port, state, accept: Some(accept) })
    }
    pub fn stop(&mut self) {
        if !self.running.swap(false, Ordering::SeqCst) {
            return;
        }
        if let Some(t) = self.accept.take() {
            let _ = t.join();
        }
        let mut st = self.state.lock().unwrap();
        for io in st.ios.values() {
            io.close();
        }
        st.games.clear();
        eprintln!("[ryusrv] stopped");
    }
    pub fn status(&self) -> Status {
        let st = self.state.lock().unwrap();
        Status {
            running: self.running.load(Ordering::SeqCst),
            port: self.port,
            connections: st.ios.len(),
            lobbies: st.games.len(),
            players: st.games.values().map(|g| g.players.len()).sum(),
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
    }
}

fn accept_loop(listener: TcpListener, running: Arc<AtomicBool>, state: Arc<Mutex<State>>) {
    static NEXT_SERIAL: AtomicU64 = AtomicU64::new(1);
    while running.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, addr)) => {
                // Windows accepted sockets inherit the listener's nonblocking mode.
                let _ = stream.set_nonblocking(false);
                let _ = stream.set_nodelay(true);
                let serial = NEXT_SERIAL.fetch_add(1, Ordering::SeqCst);
                let id = {
                    let mut st = state.lock().unwrap();
                    let mut id = [0u8; 16];
                    id[..8].copy_from_slice(&st.rand().to_le_bytes());
                    id[8..].copy_from_slice(&st.rand().to_le_bytes());
                    st.sessions.insert(serial, SessState::default());
                    id
                };
                let write_stream = stream.try_clone().expect("clone socket");
                let io = Arc::new(Io {
                    id,
                    stream: Mutex::new(Some(write_stream)),
                    closed: AtomicBool::new(false),
                });
                state.lock().unwrap().ios.insert(serial, Arc::clone(&io));
                netlog!("[ryusrv] client #{} connected from {}", serial, addr);
                let st2 = Arc::clone(&state);
                std::thread::spawn(move || reader_loop(stream, serial, st2));
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(20));
            }
            Err(_) => std::thread::sleep(Duration::from_millis(50)),
        }
    }
}

fn reader_loop(mut stream: TcpStream, serial: u64, state: Arc<Mutex<State>>) {
    stream.set_nodelay(true).ok();
    let mut header = [0u8; 12];
    loop {
        if let Err(e) = stream.read_exact(&mut header) {
            netlog!("[ryusrv] client #{} read header: {}", serial, e);
            break;
        }
        if u32::from_le_bytes(header[..4].try_into().unwrap()) != MAGIC
            || header[5] != PROTOCOL_VERSION
        {
            netlog!("[ryusrv] client #{} invalid header {:?}", serial, header);
            break;
        }
        let size = i32::from_le_bytes(header[8..12].try_into().unwrap());
        if size < 0 || size as usize + 12 > MAX_PACKET {
            netlog!("[ryusrv] client #{} invalid size {}", serial, size);
            break;
        }
        let mut data = vec![0u8; size as usize];
        if stream.read_exact(&mut data).is_err() {
            break;
        }
        let mut st = state.lock().unwrap();
        st.handle(serial, header[4], &data);
    }
    {
        let mut st = state.lock().unwrap();
        st.leave_game(serial, false);
        if let Some(io) = st.ios.remove(&serial) {
            io.close();
        }
        st.sessions.remove(&serial);
    }
    netlog!("[ryusrv] client #{} disconnected", serial);
}
