//! A Citra-family multiplayer room (Azahar, Citra), as a member: the 3DS emulators' local-wireless stand-in over ENet.
//! Wire format: Azahar's network/room.cpp and room_member.cpp (ENet, one reliable channel, big-endian numbers, room
//! protocol version 4). Unlike the Yuzu-family rooms (room.rs: LDN packets, zstd), a Citra room carries raw 802.11
//! frames (its `WifiPacket`) between its members, addressed by MAC.
//! This file is only the transport: joining, leaving, sending and receiving frames. What the frames say (UDS, the
//! 3DS's local wireless) is up to the caller.

use rusty_enet::{Event, Host, HostSettings, Packet, PeerID};
use std::collections::VecDeque;
use std::net::{ToSocketAddrs, UdpSocket};
use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};

pub type Mac = [u8; 6];

// Azahar's RoomMessageTypes
const M_JOIN_REQUEST: u8 = 1;
const M_JOIN_SUCCESS: u8 = 2;
const M_ROOM_INFO: u8 = 3;
const M_SET_GAME_INFO: u8 = 4;
const M_WIFI_PACKET: u8 = 5;
const M_NAME_COLLISION: u8 = 7;
const M_MAC_COLLISION: u8 = 8;
const M_VERSION_MISMATCH: u8 = 9;
const M_WRONG_PASSWORD: u8 = 10;
const M_CLOSE_ROOM: u8 = 11;
const M_ROOM_IS_FULL: u8 = 12;
const M_CONSOLE_ID_COLLISION: u8 = 13;
const M_HOST_KICKED: u8 = 15;
const M_HOST_BANNED: u8 = 16;
const M_JOIN_SUCCESS_AS_MOD: u8 = 24;

/// The room protocol version Azahar and Citra speak (network_version in network/room.h).
pub const NETWORK_VERSION: u32 = 4;
pub const DEFAULT_PORT: u16 = 24872;
const CONNECT_MS: u64 = 5000;
const MAX_MESSAGE: usize = 0x20000;
/// Asks the room to give this member a MAC address.
const NO_PREFERRED_MAC: Mac = [0xFF; 6];

/// A frame from the room: type (0 beacon, 1 data, 2 authentication, 3 association response, 4 deauthentication,
/// 5 node map), channel, transmitter MAC, destination MAC, the frame, its size.
pub type CitraFrameHook = Option<extern "C" fn(u8, u8, *const u8, *const u8, *const u8, u32)>;
pub type CitraLostHook = Option<extern "C" fn()>;
/// The MACs of the room's members (this one included), when the room says they changed.
pub type CitraMembersHook = Option<extern "C" fn(*const [u8; 6], i32)>;

#[derive(Clone, Copy, Default)]
pub struct CitraHooks {
    pub frame: CitraFrameHook,
    pub lost: CitraLostHook,
    pub members: CitraMembersHook,
}

struct Shared {
    joined: bool,
    mac: Mac,
    error: u8,
    run: bool,
    peer: Option<PeerID>,
    queue: VecDeque<Vec<u8>>,
    hooks: CitraHooks,
}

static SHARED: LazyLock<Mutex<Shared>> = LazyLock::new(|| {
    Mutex::new(Shared {
        joined: false,
        mac: [0; 6],
        error: 0,
        run: false,
        peer: None,
        queue: VecDeque::new(),
        hooks: CitraHooks::default(),
    })
});

fn shared() -> &'static Mutex<Shared> {
    &SHARED
}

static LOOP_THREAD: Mutex<Option<std::thread::JoinHandle<()>>> = Mutex::new(None);

pub fn set_hooks(h: CitraHooks) {
    shared().lock().unwrap().hooks = h;
}
pub fn connected() -> bool {
    shared().lock().unwrap().joined
}
pub fn own_mac() -> Mac {
    shared().lock().unwrap().mac
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
    fn u32(&mut self) -> u32 {
        self.bytes(4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]])).unwrap_or(0)
    }
    fn mac(&mut self) -> Option<Mac> {
        self.bytes(6).map(|b| [b[0], b[1], b[2], b[3], b[4], b[5]])
    }
    fn skip(&mut self, n: usize) {
        self.bytes(n);
    }
    fn skip_str(&mut self) {
        let n = self.u32() as usize;
        self.skip(n);
    }
}

fn queue(msg: Vec<u8>) {
    let mut s = shared().lock().unwrap();
    if !s.run {
        return;
    }
    s.queue.push_back(msg);
}

/// An 802.11 frame into the room (type, channel, transmitter, destination: the MAC of one member, or ff:ff:ff:ff:ff:ff
/// for all the others).
pub fn send_frame(ty: u8, channel: u8, tx: Mac, dst: Mac, data: &[u8]) {
    if !connected() || data.len() > MAX_MESSAGE {
        return;
    }
    let mut m = Vec::with_capacity(data.len() + 24);
    m.push(M_WIFI_PACKET);
    m.push(ty);
    m.push(channel);
    m.extend_from_slice(&tx);
    m.extend_from_slice(&dst);
    m.extend_from_slice(&put_u32(data.len() as u32));
    m.extend_from_slice(data);
    queue(m);
}

fn got(d: &[u8]) {
    let mut r = Rd::new(d);
    let hooks = shared().lock().unwrap().hooks;
    match r.u8() {
        M_JOIN_SUCCESS | M_JOIN_SUCCESS_AS_MOD => {
            if let Some(mac) = r.mac() {
                let mut s = shared().lock().unwrap();
                s.mac = mac;
                s.joined = true;
            }
        }
        M_ROOM_INFO => {
            r.skip_str(); // name
            r.skip_str(); // description
            r.u32(); // member slots
            r.skip(2); // port
            r.skip_str(); // preferred game
            r.skip_str(); // host username
            let n = r.u32() as usize;
            if r.bad || n > 256 {
                return;
            }
            let mut macs: Vec<[u8; 6]> = Vec::with_capacity(n);
            for _ in 0..n {
                r.skip_str(); // nickname
                let mac = r.mac();
                r.skip_str(); // game name
                r.skip(8); // game id
                r.skip_str(); // username
                r.skip_str(); // display name
                r.skip_str(); // avatar url
                match mac {
                    Some(m) if !r.bad => macs.push(m),
                    _ => return,
                }
            }
            if let Some(f) = hooks.members {
                f(macs.as_ptr(), macs.len() as i32);
            }
        }
        M_WIFI_PACKET => {
            let ty = r.u8();
            let channel = r.u8();
            let tx = r.mac();
            let dst = r.mac();
            let len = r.u32() as usize;
            let data = r.bytes(len);
            if let (Some(tx), Some(dst), Some(data), Some(f)) = (tx, dst, data, hooks.frame) {
                f(ty, channel, tx.as_ptr(), dst.as_ptr(), data.as_ptr(), data.len() as u32);
            }
        }
        M_NAME_COLLISION | M_MAC_COLLISION | M_VERSION_MISMATCH | M_WRONG_PASSWORD | M_ROOM_IS_FULL
        | M_CONSOLE_ID_COLLISION | M_HOST_KICKED | M_HOST_BANNED | M_CLOSE_ROOM => {
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
    let hooks = shared().lock().unwrap().hooks;
    if was_joined {
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

/// A console id for the join request (the room refuses two members with the same one): 32 hex digits from the clock
/// and the process id.
fn console_id_hash() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let n = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(1);
    let mut x: u128 = n ^ ((std::process::id() as u128) << 64) ^ 0x9E37_79B9_7F4A_7C15_F39C_C060_5CED_C834u128;
    let mut out = String::with_capacity(32);
    for _ in 0..4 {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out.push_str(&format!("{:08x}", (x >> 32) as u32));
    }
    out
}

/// Joins the room at `server` (host[:port], default 24872). `token` is the join request's token (empty: anonymous,
/// which rooms accept). On success the MAC address the room gave this member is returned.
pub fn join(server: &str, nick: &str, password: &str, token: &str) -> Result<Mac, String> {
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
    // (a host name too, not only an IP address: the first IPv4 address it resolves to)
    let sa = addr
        .to_socket_addrs()
        .map_err(|_| "room address not found".to_string())?
        .find(|a| a.is_ipv4())
        .ok_or_else(|| "room address not found".to_string())?;
    let peer = h.connect(sa, 1, 0).map_err(|_| "could not connect".to_string())?;
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
    // Azahar's RoomMember::SendJoinRequest: nickname, console id hash, preferred MAC, version, password, token
    let mut m = Vec::new();
    m.push(M_JOIN_REQUEST);
    wr_str(&mut m, nick);
    wr_str(&mut m, &console_id_hash());
    m.extend_from_slice(&NO_PREFERRED_MAC);
    m.extend_from_slice(&put_u32(NETWORK_VERSION));
    wr_str(&mut m, password);
    wr_str(&mut m, token);
    let _ = h.peer_mut(id).send(0, &Packet::reliable(m));
    // The game this member plays (the room lists it)
    let mut m = Vec::new();
    m.push(M_SET_GAME_INFO);
    wr_str(&mut m, "Monster Hunter Generations Ultimate");
    m.extend_from_slice(&put_u64(0x0100770008DD8000));
    let _ = h.peer_mut(id).send(0, &Packet::reliable(m));
    h.flush();
    {
        let mut s = shared().lock().unwrap();
        s.run = true;
        s.joined = false;
        s.error = 0;
        s.mac = [0; 6];
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
        return Ok(s.mac);
    }
    let err = match s.error {
        M_NAME_COLLISION => "the name is taken in the room",
        M_MAC_COLLISION => "address collision in the room",
        M_VERSION_MISMATCH => "the room runs another protocol version",
        M_WRONG_PASSWORD => "wrong room password",
        M_ROOM_IS_FULL => "the room is full",
        M_CONSOLE_ID_COLLISION => "this console id is already in the room",
        M_HOST_BANNED => "banned from the room",
        M_HOST_KICKED => "kicked from the room",
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
