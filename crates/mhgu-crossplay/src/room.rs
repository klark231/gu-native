//! A suyu-family multiplayer room, as a member: the emulators' LDN stand-in over ENet.
//! Ported from runtime/net/room.c (wire format: suyu's room.cpp/room_member.cpp, ENet, one
//! reliable channel, big-endian numbers). Payloads are zstd-compressed by the room.

use rusty_enet::{Event, Host, HostSettings, Packet, PeerID};
use std::collections::VecDeque;
use std::net::UdpSocket;

use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};

pub const RLDN_SCAN: u8 = 0;
pub const RLDN_SCAN_RESP: u8 = 1;
pub const RLDN_CONNECT: u8 = 2;
pub const RLDN_SYNC_NETWORK: u8 = 3;
pub const RLDN_DISCONNECT: u8 = 4;
pub const RLDN_DESTROY_NETWORK: u8 = 5;

const M_JOIN_REQUEST: u8 = 1;
const M_JOIN_SUCCESS: u8 = 2;
const M_ROOM_INFO: u8 = 3;
const M_SET_GAME_INFO: u8 = 4;
const M_PROXY_PACKET: u8 = 5;
const M_LDN_PACKET: u8 = 6;
const M_NAME_COLLISION: u8 = 8;
const M_IP_COLLISION: u8 = 9;
const M_VERSION_MISMATCH: u8 = 10;
const M_WRONG_PASSWORD: u8 = 11;
const M_CLOSE_ROOM: u8 = 12;
const M_ROOM_IS_FULL: u8 = 13;
const M_HOST_KICKED: u8 = 15;
const M_HOST_BANNED: u8 = 16;
const M_JOIN_SUCCESS_AS_MOD: u8 = 24;
const NETWORK_VERSION: u32 = 1;
const DEFAULT_PORT: u16 = 24872;
const CONNECT_MS: u64 = 5000;
const MAX_MESSAGE: usize = 0x20000;

pub type RoomLdnHook = Option<extern "C" fn(u8, *const u8, *const u8, bool, *const u8, u32)>;
pub type RoomProxyHook = Option<extern "C" fn(*const u8, u16, *const u8, u16, u8, bool, *const u8, u32)>;
pub type RoomLostHook = Option<extern "C" fn()>;
pub type RoomMembersHook = Option<extern "C" fn(*const [u8; 4], i32)>;

#[derive(Clone, Copy, Default)]
pub struct RoomHooks {
    pub ldn: RoomLdnHook,
    pub proxy: RoomProxyHook,
    pub lost: RoomLostHook,
    pub members: RoomMembersHook,
}

struct Shared {
    joined: bool,
    ip: [u8; 4],
    error: u8,
    run: bool,
    peer: Option<PeerID>,
    queue: VecDeque<Vec<u8>>,
    hooks: RoomHooks,
    leaving: bool,
}

static SHARED: LazyLock<Mutex<Shared>> = LazyLock::new(|| {
    Mutex::new(Shared {
        joined: false,
        ip: [0; 4],
        error: 0,
        run: false,
        peer: None,
        queue: VecDeque::new(),
        hooks: RoomHooks::default(),
        leaving: false,
    })
});

fn shared() -> &'static Mutex<Shared> {
    &SHARED
}

static LOOP_THREAD: Mutex<Option<std::thread::JoinHandle<()>>> = Mutex::new(None);

pub fn set_hooks(h: RoomHooks) {
    shared().lock().unwrap().hooks = h;
}
pub fn connected() -> bool {
    shared().lock().unwrap().joined
}
pub fn own_ip() -> [u8; 4] {
    shared().lock().unwrap().ip
}

fn put_u16(v: u16) -> [u8; 2] {
    v.to_be_bytes()
}
fn put_u32(v: u32) -> [u8; 4] {
    v.to_be_bytes()
}
fn put_u64(v: u64) -> [u8; 8] {
    v.to_be_bytes()
}
fn wr_str(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(&put_u32(s.len() as u32));
    out.extend_from_slice(s.as_bytes());
}

struct Rd<'a> {
    d: &'a [u8],
    at: usize,
    bad: bool,
}
impl<'a> Rd<'a> {
    fn new(d: &'a [u8]) -> Rd<'a> {
        Rd { d, at: 0, bad: false }
    }
    fn bytes(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.bad || self.d.len() - self.at < n {
            self.bad = true;
            return None;
        }
        let q = &self.d[self.at..self.at + n];
        self.at += n;
        Some(q)
    }
    fn u8(&mut self) -> u8 {
        self.bytes(1).map(|b| b[0]).unwrap_or(0)
    }
    fn u16(&mut self) -> u16 {
        self.bytes(2).map(|b| u16::from_be_bytes([b[0], b[1]])).unwrap_or(0)
    }
    fn u32(&mut self) -> u32 {
        self.bytes(4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]])).unwrap_or(0)
    }
    fn skip(&mut self, n: usize) {
        self.bytes(n);
    }
    fn skip_str(&mut self) {
        let n = self.u32() as usize;
        self.skip(n);
    }
}

/// The peers whose payloads came uncompressed.
static RAW: Mutex<Vec<([u8; 4], bool)>> = Mutex::new(Vec::new());
fn raw_is(ip: &[u8; 4]) -> bool {
    RAW.lock().unwrap().iter().find(|(k, _)| k == ip).map(|(_, r)| *r).unwrap_or(false)
}
fn raw_note(ip: &[u8; 4], raw: bool) {
    let mut g = RAW.lock().unwrap();
    if let Some(e) = g.iter_mut().find(|(k, _)| k == ip) {
        e.1 = raw;
    } else if g.len() < 32 {
        g.push((*ip, raw));
    } else {
        g[ip[3] as usize % 32] = (*ip, raw);
    }
}
fn is_zstd(d: &[u8]) -> bool {
    d.len() >= 4 && d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD
}

fn queue(msg: Vec<u8>) {
    let mut s = shared().lock().unwrap();
    if !s.run {
        return;
    }
    s.queue.push_back(msg);
}

/// A game packet into the room (proto 3 TCP, 4 UDP; the emulators' room proxy).
pub fn send_proxy(sip: [u8; 4], sport: u16, dip: [u8; 4], dport: u16, proto: u8, bc: bool, data: &[u8]) {
    if !connected() || data.len() > MAX_MESSAGE {
        return;
    }
    let mut payload: Vec<u8>;
    if !data.is_empty() && (bc || !raw_is(&dip)) {
        payload = match zstd::bulk::compress(data, 3) {
            Ok(z) if z.len() <= MAX_MESSAGE => z,
            _ => return,
        };
    } else {
        payload = data.to_vec();
    }
    let mut m = Vec::with_capacity(payload.len() + 48);
    m.push(M_PROXY_PACKET);
    m.push(1);
    m.extend_from_slice(&sip);
    m.extend_from_slice(&put_u16(sport));
    m.push(1);
    m.extend_from_slice(&dip);
    m.extend_from_slice(&put_u16(dport));
    m.push(proto);
    m.push(bc as u8);
    let len = payload.len() as u32;
    m.extend_from_slice(&put_u32(len));
    m.append(&mut payload);
    queue(m);
}

pub fn send_ldn(ty: u8, lip: [u8; 4], rip: [u8; 4], bc: bool, data: &[u8]) {
    if !connected() || data.len() > MAX_MESSAGE {
        return;
    }
    let mut m = Vec::with_capacity(data.len() + 32);
    m.push(M_LDN_PACKET);
    m.push(ty);
    m.extend_from_slice(&lip);
    m.extend_from_slice(&rip);
    m.push(bc as u8);
    let len = data.len() as u32;
    m.extend_from_slice(&put_u32(len));
    m.extend_from_slice(data);
    queue(m);
}

fn got(d: &[u8]) {
    let mut r = Rd::new(d);
    let hooks = shared().lock().unwrap().hooks;
    match r.u8() {
        M_JOIN_SUCCESS | M_JOIN_SUCCESS_AS_MOD => {
            if let Some(ip) = r.bytes(4) {
                let mut s = shared().lock().unwrap();
                s.ip.copy_from_slice(ip);
                s.joined = true;
            }
        }
        M_ROOM_INFO => {
            r.skip_str();
            r.skip_str();
            r.u32();
            r.u16();
            r.skip_str();
            r.skip_str();
            let n = r.u32() as usize;
            if r.bad || n > 256 {
                return;
            }
            let mut ips: Vec<[u8; 4]> = Vec::with_capacity(n);
            for _ in 0..n {
                r.skip_str();
                let Some(ip) = r.bytes(4) else { break };
                let mut a = [0u8; 4];
                a.copy_from_slice(ip);
                r.skip_str();
                r.skip(8);
                r.skip_str();
                r.skip_str();
                r.skip_str();
                r.skip_str();
                if !r.bad {
                    ips.push(a);
                }
            }
            if !r.bad {
                if let Some(f) = hooks.members {
                    f(ips.as_ptr(), ips.len() as i32);
                }
            }
        }
        M_LDN_PACKET => {
            let ty = r.u8();
            let lip = r.bytes(4).map(|b| [b[0], b[1], b[2], b[3]]);
            let rip = r.bytes(4).map(|b| [b[0], b[1], b[2], b[3]]);
            let bc = r.u8() != 0;
            let len = r.u32() as usize;
            let data = r.bytes(len);
            if let (Some(lip), Some(rip)) = (lip, rip) {
                if let (Some(data), Some(f)) = (data, hooks.ldn) {
                    f(ty, lip.as_ptr(), rip.as_ptr(), bc, data.as_ptr(), data.len() as u32);
                }
            }
        }
        M_PROXY_PACKET => {
            r.u8();
            let sip = r.bytes(4).map(|b| [b[0], b[1], b[2], b[3]]);
            let sport = r.u16();
            r.u8();
            let dip = r.bytes(4).map(|b| [b[0], b[1], b[2], b[3]]);
            let dport = r.u16();
            let proto = r.u8();
            let bc = r.u8() != 0;
            let len = r.u32() as usize;
            let data = r.bytes(len);
            if let (Some(sip), Some(dip), Some(data), Some(f)) = (sip, dip, data, hooks.proxy) {
                if len > 0 && is_zstd(data) {
                    let n = zstd::zstd_safe::get_frame_content_size(data);
                    let n = match n {
                        Ok(Some(n)) if n as usize <= MAX_MESSAGE => n as usize,
                        _ => return,
                    };
                    let mut out = vec![0u8; n.max(1)];
                    match zstd::bulk::decompress_to_buffer(data, &mut out) {
                        Ok(got) => {
                            raw_note(&sip, false);
                            f(sip.as_ptr(), sport, dip.as_ptr(), dport, proto, bc, out.as_ptr(), got as u32);
                        }
                        Err(_) => {}
                    }
                } else {
                    raw_note(&sip, true);
                    f(sip.as_ptr(), sport, dip.as_ptr(), dport, proto, bc, data.as_ptr(), data.len() as u32);
                }
            }
        }
        M_NAME_COLLISION | M_IP_COLLISION | M_VERSION_MISMATCH | M_WRONG_PASSWORD | M_ROOM_IS_FULL | M_HOST_KICKED
        | M_HOST_BANNED | M_CLOSE_ROOM => {
            let mut s = shared().lock().unwrap();
            s.error = d[0];
            s.joined = false;
            s.run = false;
        }
        _ => {}
    }
}

fn room_loop(mut host: Host<UdpSocket>) {
    let mut was_joined = false;
    loop {
        {
            let s = shared().lock().unwrap();
            if !s.run {
                break;
            }
        }
        match host.service() {
            Ok(Some(Event::Receive { packet, .. })) => {
                if !packet.data().is_empty() && packet.data().len() <= MAX_MESSAGE {
                    let d = packet.data().to_vec();
                    got(&d);
                }
            }
            Ok(Some(Event::Disconnect { .. })) => {
                let mut s = shared().lock().unwrap();
                if s.joined {
                    s.error = M_CLOSE_ROOM;
                }
                s.joined = false;
                s.run = false;
            }
            Ok(_) => {}
            Err(_) => {
                let mut s = shared().lock().unwrap();
                s.joined = false;
                s.run = false;
            }
        }
        let queued: Vec<Vec<u8>> = {
            let mut s = shared().lock().unwrap();
            s.queue.drain(..).collect()
        };
        if !queued.is_empty() {
            let peer = { shared().lock().unwrap().peer };
            if let Some(id) = peer {
                let mut sent = false;
                for m in queued {
                    if host.peer_mut(id).send(0, &Packet::reliable(m)).is_ok() {
                        sent = true;
                    }
                }
                if sent {
                    host.flush();
                }
            }
        }
        {
            let s = shared().lock().unwrap();
            if s.joined {
                was_joined = true;
            }
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let joined = was_joined;
    let hooks = shared().lock().unwrap().hooks;
    if joined {
        if let Some(f) = hooks.lost {
            f();
        }
    }
    // Leave politely.
    let peer = { shared().lock().unwrap().peer };
    if let Some(id) = peer {
        host.peer_mut(id).disconnect_now(0);
        host.flush();
    }
}

fn parse_addr(server: &str) -> Option<(String, u16)> {
    let t = server.trim();
    if t.is_empty() {
        return None;
    }
    let (host, port) = match t.rfind(':') {
        Some(i) if t[..i].find(':').is_none() => {
            let p = t[i + 1..].parse::<u32>().ok();
            match p {
                Some(p) if (1..=65535).contains(&p) => (t[..i].to_string(), p as u16),
                _ => (t.to_string(), DEFAULT_PORT),
            }
        }
        _ => (t.to_string(), DEFAULT_PORT),
    };
    Some((host, port))
}

/// Joins the room at `server` (host[:port], default 24872). On success the fake address is returned.
/// A room nickname as the rooms take one: 4 to 20 of letters, digits, space, '.', '_' and '-'.
pub fn room_nick(s: &str) -> String {
    let n: String = s.chars().filter(|c| c.is_ascii_alphanumeric() || " ._-".contains(*c)).take(20).collect();
    let mut n = n.trim().to_string();
    while n.len() < 4 {
        n.push('_');
    }
    n
}

/// join, and if the nickname is taken in the room, the same with a number after it ("MHGU-2", ... "MHGU-5"); the
/// first try is the name as typed (the room this app runs knows its own host by it).
pub fn join_any_name(server: &str, nick: &str, password: &str) -> Result<[u8; 4], String> {
    let mut last = match join(server, nick, password) {
        Err(e) if e.contains("name is taken") => e,
        r => return r,
    };
    let base = room_nick(nick);
    for k in 2..=5 {
        let name = room_nick(&format!("{}-{k}", &base[..base.len().min(17)]));
        match join(server, &name, password) {
            Err(e) if e.contains("name is taken") => last = e,
            r => return r,
        }
    }
    Err(last)
}

pub fn join(server: &str, nick: &str, password: &str) -> Result<[u8; 4], String> {
    leave();
    let (host, port) = parse_addr(server).ok_or("bad room address")?;
    let addr = format!("{host}:{port}");
    let sock = UdpSocket::bind("0.0.0.0:0").map_err(|e| format!("could not create the client: {e}"))?;
    let settings = HostSettings {
        peer_limit: 1,
        channel_limit: 1,
        ..Default::default()
    };
    let mut h = Host::new(sock, settings).map_err(|_| "could not create the client".to_string())?;
    let peer = h.connect(addr.parse().map_err(|_| "room address not found".to_string())?, 1, 0)
        .map_err(|_| "could not connect".to_string())?;
    let id = peer.id();
    // Wait for the ENet connect.
    let end = Instant::now() + Duration::from_millis(CONNECT_MS);
    loop {
        match h.service() {
            Ok(Some(Event::Connect { .. })) => {
                h.peer_mut(id).set_ping_interval(1000);
                h.peer_mut(id).set_timeout(2, 5000, 15000);
                break;
            }
            Ok(_) => {}
            Err(_) => return Err("no answer from the room".into()),
        }
        if Instant::now() > end {
            return Err("no answer from the room".into());
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    let mut m = Vec::new();
    m.push(M_JOIN_REQUEST);
    wr_str(&mut m, nick);
    m.extend_from_slice(&[0xFF; 4]);
    m.extend_from_slice(&put_u32(NETWORK_VERSION));
    wr_str(&mut m, password);
    wr_str(&mut m, "");
    let _ = h.peer_mut(id).send(0, &Packet::reliable(m));
    let mut m = Vec::new();
    m.push(M_SET_GAME_INFO);
    wr_str(&mut m, "Monster Hunter Generations Ultimate");
    m.extend_from_slice(&put_u64(0x0100770008DD8000));
    wr_str(&mut m, "1.4.0");
    let _ = h.peer_mut(id).send(0, &Packet::reliable(m));
    h.flush();
    {
        let mut s = shared().lock().unwrap();
        s.run = true;
        s.joined = false;
        s.error = 0;
        s.ip = [0; 4];
        s.leaving = false;
        s.peer = Some(id);
    }
    *LOOP_THREAD.lock().unwrap() = Some(std::thread::spawn(move || room_loop(h)));
    for _ in 0..100 {
        {
            let s = shared().lock().unwrap();
            if s.joined || !s.run {
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    let s = shared().lock().unwrap();
    if s.joined {
        return Ok(s.ip);
    }
    let err = match s.error {
        M_NAME_COLLISION => "the name is taken in the room",
        M_IP_COLLISION => "address collision in the room",
        M_VERSION_MISMATCH => "the room runs another protocol version",
        M_WRONG_PASSWORD => "wrong room password",
        M_ROOM_IS_FULL => "the room is full",
        M_HOST_BANNED => "banned from the room",
        _ => "the room did not answer the join",
    };
    drop(s);
    leave();
    Err(err.into())
}

pub fn leave() {
    {
        let mut s = shared().lock().unwrap();
        s.run = false;
        s.leaving = true;
    }
    let h = LOOP_THREAD.lock().unwrap().take();
    if let Some(h) = h {
        let _ = h.join();
    }
    let mut s = shared().lock().unwrap();
    s.joined = false;
    s.peer = None;
    s.queue.clear();
}
