//! RyuLdn: the lobby protocol of Ryujinx's LDN servers (Ryubing/LdnServer), for crossplay with
//! Ryujinx players. Ported from the C client in runtime/net/ryuldn.c (MIT, written from the
//! protocol of Ryujinx's LdnServer and LDN client).
//!
//! Every message: a 12-byte header {u32 magic 'RLDN', u8 type, u8 version (1), 2 bytes padding,
//! s32 payload size} and the payload, little-endian, in the C# servers' sequential struct layout.

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

pub const RYU_NETINFO: usize = 0x480;
pub const MAX_SERVERS: usize = 4;
const HDR: usize = 12;
const MAGIC: u32 = 0x4E44_4C52; // 'R' 'L' 'D' 'N'
const MAX_PACKET: usize = 131_072;
const MAX_PAYLOAD: usize = MAX_PACKET - HDR - 1;
const SEC_SIZE: usize = 0x44;
const USER_SIZE: usize = 0x30;
const NETCFG_SIZE: usize = 0x20;
const CREATE_SIZE: usize = 0xBC;
const CONNECT_SIZE: usize = 0x500;
const PROXYINFO: usize = 0x10;
const ADV_MAX: usize = 0x180;
const DEFAULT_PORT: u16 = 30456;
const CONNECT_MS: u64 = 4000;
const SCAN_MS: u64 = 4000;
const REJECT_MS: u64 = 4000;
const SEND_MS: u64 = 5000;

pub const R_CONNECT_FAILURE: u32 = (203 & 0x1FF) | (64 << 9);
pub const R_CONNECT_NOT_FOUND: u32 = (203 & 0x1FF) | (65 << 9);
pub const R_CONNECT_TIMEOUT: u32 = (203 & 0x1FF) | (66 << 9);
pub const R_CONNECT_REJECTED: u32 = (203 & 0x1FF) | (67 << 9);
pub const R_VERSION_TOO_LOW: u32 = (203 & 0x1FF) | (113 << 9);
pub const R_VERSION_TOO_HIGH: u32 = (203 & 0x1FF) | (114 << 9);
pub const R_TOO_MANY_PLAYERS: u32 = (203 & 0x1FF) | (144 << 9);
pub const R_ACCESS_POINT_FAILED: u32 = (203 & 0x1FF) | (48 << 9);

mod msg {
    pub const INITIALIZE: u8 = 0;
    pub const PASSPHRASE: u8 = 1;
    pub const CREATE: u8 = 2;
    pub const EXTERNAL_PROXY: u8 = 4;
    pub const SYNC_NETWORK: u8 = 7;
    pub const REJECT: u8 = 8;
    pub const REJECT_REPLY: u8 = 9;
    pub const SCAN: u8 = 10;
    pub const SCAN_REPLY: u8 = 11;
    pub const SCAN_REPLY_END: u8 = 12;
    pub const CONNECT: u8 = 13;
    pub const CONNECTED: u8 = 15;
    pub const DISCONNECT: u8 = 16;
    pub const PROXY_CONFIG: u8 = 17;
    pub const PROXY_DATA: u8 = 20;
    pub const SET_ACCEPT_POLICY: u8 = 22;
    pub const SET_ADVERTISE: u8 = 23;
    pub const PING: u8 = 254;
    pub const NETWORK_ERROR: u8 = 255;
}

mod err {
    pub const NONE: i32 = 0;
    pub const PORT_UNREACHABLE: i32 = 1;
    pub const TOO_MANY_PLAYERS: i32 = 2;
    pub const VERSION_TOO_LOW: i32 = 3;
    pub const VERSION_TOO_HIGH: i32 = 4;
    pub const CONNECT_FAILURE: i32 = 5;
    pub const CONNECT_NOT_FOUND: i32 = 6;
    pub const CONNECT_TIMEOUT: i32 = 7;
    pub const CONNECT_REJECTED: i32 = 8;
}

/// A game packet in the server's terms (host order); proto 17 UDP, 6 TCP.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RyuProxy {
    pub src_ip: u32,
    pub dst_ip: u32,
    pub src_port: u16,
    pub dst_port: u16,
    pub proto: u32,
}

pub type LobbyHook = Option<extern "C" fn(*const u8)>;
pub type DataHook = Option<extern "C" fn(*const RyuProxy, *const u8, u32)>;

#[derive(Clone, Copy, Default)]
pub struct RyuHooks {
    pub lobby: LobbyHook,
    pub data: DataHook,
}

pub type PeerLobbyHook = Option<extern "C" fn(*mut core::ffi::c_void, *const u8)>;
pub type PeerDataHook = Option<extern "C" fn(*mut core::ffi::c_void, *const RyuProxy, *const u8, u32)>;

#[derive(Clone, Copy, Default)]
pub struct RyuPeerHooks {
    pub lobby: PeerLobbyHook,
    pub data: PeerDataHook,
}

#[derive(Clone)]
struct Server {
    host: String,
    port: u16,
}

fn put_u16(p: &mut [u8], v: u16) {
    p[..2].copy_from_slice(&v.to_le_bytes());
}
fn put_u32(p: &mut [u8], v: u32) {
    p[..4].copy_from_slice(&v.to_le_bytes());
}
fn get_u16(p: &[u8]) -> u16 {
    u16::from_le_bytes([p[0], p[1]])
}
fn get_u32(p: &[u8]) -> u32 {
    u32::from_le_bytes([p[0], p[1], p[2], p[3]])
}

fn parse_server(t: &str) -> Option<Server> {
    let t = t.trim();
    if t.is_empty() {
        return None;
    }
    let colon = t.rfind(':');
    let (host, port) = match colon {
        Some(i) if t[..i].find(':').is_none() => {
            let p = t[i + 1..].parse::<u32>().ok();
            match p {
                Some(p) if (1..=65535).contains(&p) => (t[..i].trim_end().to_string(), p as u16),
                _ => (t.to_string(), DEFAULT_PORT),
            }
        }
        _ => (t.to_string(), DEFAULT_PORT),
    };
    if host.is_empty() {
        return None;
    }
    Some(Server { host, port })
}

struct Config {
    servers: Vec<Server>,
    loaded: bool,
    host: String,
    pass: String,
    own: String,
    hooks: RyuHooks,
}

static CONFIG: Mutex<Option<Config>> = Mutex::new(None);

fn with_cfg<R>(f: impl FnOnce(&mut Config) -> R) -> R {
    let mut g = CONFIG.lock().unwrap();
    let c = g.get_or_insert_with(|| Config {
        servers: Vec::new(),
        loaded: false,
        host: String::new(),
        pass: String::new(),
        own: String::new(),
        hooks: RyuHooks::default(),
    });
    f(c)
}

/// [Multiplayer] RyuServers, RyuHost and RyuPassphrase (as saved now).
pub fn set_config(servers: &str, host: &str, pass: &str) {
    with_cfg(|c| {
        c.servers = servers
            .split([',', ';'])
            .filter_map(parse_server)
            .take(MAX_SERVERS)
            .collect();
        c.loaded = true;
        c.host = host.trim().to_string();
        c.pass = pass.to_string();
    })
}

/// The server this game runs itself, when it does ("" when not).
pub fn set_self(addr: Option<&str>) {
    with_cfg(|c| c.own = addr.unwrap_or("").to_string())
}

pub fn set_hooks(h: RyuHooks) {
    with_cfg(|c| c.hooks = h)
}

pub fn server_count() -> usize {
    with_cfg(|c| {
        if !c.loaded {
            c.servers.clear();
            c.loaded = true;
        }
        c.servers.len()
    })
}

fn host_addr() -> String {
    with_cfg(|c| {
        if c.host.is_empty() {
            c.own.clone()
        } else {
            c.host.clone()
        }
    })
}

pub fn enabled() -> bool {
    server_count() > 0 || !host_addr().is_empty()
}

fn hooks() -> RyuHooks {
    with_cfg(|c| c.hooks)
}

struct Flags {
    alive: bool,
    scan_done: bool,
    join_done: bool,
    reject_done: bool,
    join_err: i32,
    net_err: i32,
    have_proxy: bool,
    proxy_ip: u32,
    proxy_mask: u32,
    info: [u8; RYU_NETINFO],
    have_info: bool,
    in_lobby: bool,
    closing: bool,
}

struct Conn {
    srv: i32,
    deliver: bool,
    p2p: bool,
    peer: bool,
    stream: Mutex<TcpStream>,
    write: Mutex<()>,
    flags: Mutex<Flags>,
    cv: Condvar,
    scan: Mutex<Vec<Vec<u8>>>,
    peer_hooks: Mutex<Option<(RyuPeerHooks, usize)>>,
    thread: Mutex<Option<std::thread::JoinHandle<()>>>,
}

impl Conn {
    fn send(&self, ty: u8, a: &[u8], b: &[u8]) -> bool {
        if a.len() + b.len() > MAX_PAYLOAD {
            return false;
        }
        let f = self.flags.lock().unwrap();
        if !f.alive || f.closing {
            return false;
        }
        drop(f);
        let w = self.write.lock().unwrap();
        let mut h = [0u8; HDR];
        put_u32(&mut h, MAGIC);
        h[4] = ty;
        h[5] = 1;
        put_u32(&mut h[8..], (a.len() + b.len()) as u32);
        let mut s = self.stream.lock().unwrap();
        let ok = s.write_all(&h).and_then(|_| s.write_all(a)).and_then(|_| s.write_all(b)).is_ok();
        if !ok {
            self.flags.lock().unwrap().closing = true;
            let _ = s.shutdown(std::net::Shutdown::Both);
        }
        drop(w);
        ok
    }

    fn close(&self) {
        {
            let mut f = self.flags.lock().unwrap();
            f.alive = false;
            f.closing = true;
            self.cv.notify_all();
        }
        if let Ok(s) = self.stream.lock() {
            let _ = s.shutdown(std::net::Shutdown::Both);
            let _ = s.set_nonblocking(true);
        }
        let h = self.thread.lock().unwrap().take();
        if let Some(h) = h {
            let _ = h.join();
        }
    }

    fn wait_flag(&self, pick: impl Fn(&Flags) -> bool, ms: u64) -> bool {
        let end = Instant::now() + Duration::from_millis(ms);
        let mut f = self.flags.lock().unwrap();
        loop {
            if pick(&f) {
                return true;
            }
            if !f.alive {
                return false;
            }
            let now = Instant::now();
            if now >= end {
                return false;
            }
            let (g, _) = self.cv.wait_timeout(f, end - now).unwrap();
            f = g;
        }
    }
}

fn io_connect(sv: &Server) -> Option<TcpStream> {
    let addrs: Vec<SocketAddr> = (sv.host.as_str(), sv.port).to_socket_addrs().ok()?.collect();
    let addr = addrs.into_iter().next()?;
    let s = TcpStream::connect_timeout(&addr, Duration::from_millis(CONNECT_MS)).ok()?;
    let _ = s.set_nodelay(true);
    let _ = s.set_write_timeout(Some(Duration::from_millis(SEND_MS)));
    Some(s)
}

fn spawn_reader(c: &Arc<Conn>) {
    let c2 = Arc::clone(c);
    let handle = std::thread::spawn(move || reader(c2));
    *c.thread.lock().unwrap() = Some(handle);
}

fn conn_open(sv: &Server, srv: i32, deliver: bool, handshake: bool, p2p: bool, peer: bool) -> Option<Arc<Conn>> {
    let s = match io_connect(sv) {
        Some(s) => s,
        None => {
            eprintln!("[ryuldn] can't reach {}:{}", sv.host, sv.port);
            return None;
        }
    };
    let read = s.try_clone().ok()?;
    let c = Arc::new(Conn {
        srv,
        deliver,
        p2p,
        peer,
        stream: Mutex::new(s),
        write: Mutex::new(()),
        flags: Mutex::new(Flags {
            alive: true,
            scan_done: false,
            join_done: false,
            reject_done: false,
            join_err: err::NONE,
            net_err: err::NONE,
            have_proxy: false,
            proxy_ip: 0,
            proxy_mask: 0,
            info: [0; RYU_NETINFO],
            have_info: false,
            in_lobby: false,
            closing: false,
        }),
        cv: Condvar::new(),
        scan: Mutex::new(Vec::new()),
        peer_hooks: Mutex::new(None),
        thread: Mutex::new(None),
    });
    spawn_reader(&c);
    if !handshake {
        return Some(c);
    }
    let id: [u8; 22] = if srv >= 0 {
        IDENT.lock().unwrap().get(srv as usize % MAX_SERVERS).map(|m| *m.lock().unwrap()).unwrap_or([0; 22])
    } else {
        [0; 22]
    };
    c.send(msg::INITIALIZE, &id, &[]);
    let pass = with_cfg(|cfg| cfg.pass.clone());
    let mut p = [0u8; 0x80];
    let ok = pass.len() == 16 && pass.starts_with("Ryujinx-") && pass[8..].chars().all(|ch| ch.is_ascii_hexdigit());
    if ok {
        p[..16].copy_from_slice(&pass.as_bytes()[..16]);
    }
    c.send(msg::PASSPHRASE, &p, &[]);
    let _ = read;
    Some(c)
}

static IDENT: Mutex<Vec<Mutex<[u8; 22]>>> = Mutex::new(Vec::new());

fn ident_init() {
    let mut g = IDENT.lock().unwrap();
    while g.len() < MAX_SERVERS + 1 {
        g.push(Mutex::new([0u8; 22]));
    }
}

struct Global {
    main: Option<Arc<Conn>>,
    p2p: Option<Arc<Conn>>,
    in_lobby: bool,
    p2p_expected: bool,
    p2p_ready: bool,
}
static G: Mutex<Global> = Mutex::new(Global {
    main: None,
    p2p: None,
    in_lobby: false,
    p2p_expected: false,
    p2p_ready: false,
});

fn in_lobby_conn(c: &Conn) -> bool {
    if c.peer {
        c.flags.lock().unwrap().in_lobby
    } else {
        G.lock().unwrap().in_lobby
    }
}

fn reader(c: Arc<Conn>) {
    let mut stream = match c.stream.lock().unwrap().try_clone() {
        Ok(s) => s,
        Err(_) => return,
    };
    let mut buf = vec![0u8; MAX_PACKET];
    loop {
        let mut h = [0u8; HDR];
        if stream.read_exact(&mut h).is_err() {
            break;
        }
        let size = i32::from_le_bytes([h[8], h[9], h[10], h[11]]);
        if get_u32(&h) != MAGIC || h[5] != 1 || size < 0 || size as usize > MAX_PAYLOAD {
            break;
        }
        let n = size as usize;
        if n > 0 && stream.read_exact(&mut buf[..n]).is_err() {
            break;
        }
        handle(&c, h[4], &buf[..n]);
    }
    let was;
    {
        let mut f = c.flags.lock().unwrap();
        was = f.alive;
        f.alive = false;
        f.in_lobby = false;
        c.cv.notify_all();
    }
    if !was || c.flags.lock().unwrap().closing {
        return;
    }
    if c.peer && c.deliver {
        let hooks = c.peer_hooks.lock().unwrap().as_ref().copied();
        if let Some((h, ud)) = hooks {
            if let Some(f) = h.lobby {
                f(ud as *mut core::ffi::c_void, std::ptr::null());
            }
        }
    } else if c.deliver {
        let ours = {
            let mut g = G.lock().unwrap();
            let ours = g.main.as_ref().map(|m| Arc::ptr_eq(m, &c)).unwrap_or(false)
                || g.p2p.as_ref().map(|p| Arc::ptr_eq(p, &c)).unwrap_or(false);
            if ours && g.in_lobby {
                if g.p2p.as_ref().map(|p| Arc::ptr_eq(p, &c)).unwrap_or(false) {
                    if let Some(m) = &g.main {
                        m.send(msg::DISCONNECT, &[0, 0, 0, 0], &[]);
                    }
                }
                g.in_lobby = false;
                true
            } else {
                false
            }
        };
        if ours {
            eprintln!("[ryuldn] the connection to the lobby was lost");
            if let Some(f) = hooks().lobby {
                f(std::ptr::null());
            }
        }
    }
}

fn handle(c: &Arc<Conn>, ty: u8, d: &[u8]) {
    match ty {
        msg::INITIALIZE => {
            if d.len() >= 22 && c.srv >= -1 && !c.p2p {
                ident_init();
                let idx = if c.srv < 0 { MAX_SERVERS } else { c.srv as usize };
                if let Some(slot) = IDENT.lock().unwrap().get(idx) {
                    slot.lock().unwrap().copy_from_slice(&d[..22]);
                }
            }
        }
        msg::PING => {
            if d.len() >= 2 && d[0] == 0 {
                c.send(msg::PING, &d[..2], &[]);
            }
        }
        msg::NETWORK_ERROR => {
            if d.len() >= 4 {
                let e = get_u32(d) as i32;
                if e != err::PORT_UNREACHABLE {
                    eprintln!("[ryuldn] the server answered with error {e}");
                    let mut f = c.flags.lock().unwrap();
                    f.net_err = e;
                    f.join_err = e;
                    c.cv.notify_all();
                }
            }
        }
        msg::PROXY_CONFIG => {
            if d.len() >= 8 {
                let (ip, mask) = (get_u32(d), get_u32(&d[4..]));
                {
                    let mut f = c.flags.lock().unwrap();
                    f.have_proxy = true;
                    f.proxy_ip = ip;
                    f.proxy_mask = mask;
                    c.cv.notify_all();
                }
                if c.p2p {
                    G.lock().unwrap().p2p_ready = true;
                    if let Some(m) = G.lock().unwrap().main.clone() {
                        let mut f = m.flags.lock().unwrap();
                        f.have_proxy = true;
                        f.proxy_ip = ip;
                        f.proxy_mask = mask;
                        m.cv.notify_all();
                    }
                }
            }
        }
        msg::CONNECTED => {
            if d.len() >= RYU_NETINFO {
                {
                    let mut f = c.flags.lock().unwrap();
                    f.info.copy_from_slice(&d[..RYU_NETINFO]);
                    f.have_info = true;
                    f.join_done = true;
                    f.in_lobby = true;
                    c.cv.notify_all();
                }
                if c.deliver && in_lobby_conn(c) {
                    if c.peer {
                        let hooks = c.peer_hooks.lock().unwrap().as_ref().copied();
                        if let Some((h, ud)) = hooks {
                            if let Some(f) = h.lobby {
                                f(ud as *mut core::ffi::c_void, d.as_ptr());
                            }
                        }
                    } else if let Some(f) = hooks().lobby {
                        f(d.as_ptr());
                    }
                }
            }
        }
        msg::SYNC_NETWORK => {
            if d.len() >= RYU_NETINFO && c.deliver {
                {
                    let mut f = c.flags.lock().unwrap();
                    f.info.copy_from_slice(&d[..RYU_NETINFO]);
                    f.have_info = true;
                }
                if in_lobby_conn(c) {
                    if c.peer {
                        let hooks = c.peer_hooks.lock().unwrap().as_ref().copied();
                        if let Some((h, ud)) = hooks {
                            if let Some(f) = h.lobby {
                                f(ud as *mut core::ffi::c_void, d.as_ptr());
                            }
                        }
                    } else if let Some(f) = hooks().lobby {
                        f(d.as_ptr());
                    }
                }
            }
        }
        msg::DISCONNECT => {
            if c.deliver && c.peer {
                let was = {
                    let mut f = c.flags.lock().unwrap();
                    let was = f.in_lobby;
                    f.in_lobby = false;
                    was
                };
                if was {
                    let hooks = c.peer_hooks.lock().unwrap().as_ref().copied();
                    if let Some((h, ud)) = hooks {
                        if let Some(f) = h.lobby {
                            f(ud as *mut core::ffi::c_void, std::ptr::null());
                        }
                    }
                }
            } else if c.deliver {
                let was = {
                    let mut g = G.lock().unwrap();
                    let was = g.in_lobby;
                    g.in_lobby = false;
                    was
                };
                if was {
                    eprintln!("[ryuldn] the lobby was closed");
                    if let Some(f) = hooks().lobby {
                        f(std::ptr::null());
                    }
                }
            }
        }
        msg::SCAN_REPLY => {
            if d.len() >= RYU_NETINFO {
                let mut s = c.scan.lock().unwrap();
                if s.len() < 64 {
                    s.push(d[..RYU_NETINFO].to_vec());
                }
            }
        }
        msg::SCAN_REPLY_END => {
            let mut f = c.flags.lock().unwrap();
            f.scan_done = true;
            c.cv.notify_all();
        }
        msg::REJECT_REPLY => {
            let mut f = c.flags.lock().unwrap();
            f.reject_done = true;
            c.cv.notify_all();
        }
        msg::PROXY_DATA => {
            if c.deliver || c.peer {
                got_data(c, d);
            }
        }
        msg::EXTERNAL_PROXY => {
            if !c.p2p && c.deliver {
                p2p_start(c, d);
            }
        }
        _ => {}
    }
}

fn got_data(c: &Arc<Conn>, d: &[u8]) {
    if d.len() < PROXYINFO + 4 {
        return;
    }
    let len = get_u32(&d[PROXYINFO..]) as usize;
    if len != d.len() - PROXYINFO - 4 {
        return;
    }
    let p = RyuProxy {
        src_ip: get_u32(d),
        src_port: get_u16(&d[4..]),
        dst_ip: get_u32(&d[6..]),
        dst_port: get_u16(&d[10..]),
        proto: get_u32(&d[12..]),
    };
    let data = &d[PROXYINFO + 4..];
    if c.peer {
        let hooks = c.peer_hooks.lock().unwrap().as_ref().copied();
        if let Some((h, ud)) = hooks {
            if let Some(f) = h.data {
                f(ud as *mut core::ffi::c_void, &p, data.as_ptr(), data.len() as u32);
            }
        }
    } else if let Some(f) = hooks().data {
        f(&p, data.as_ptr(), data.len() as u32);
    }
}

fn p2p_start(main: &Arc<Conn>, d: &[u8]) {
    if d.len() < 0x26 || get_u32(&d[16..]) != 2 {
        eprintln!("[ryuldn] the host's proxy needs an address family this client doesn't speak");
        let mut f = main.flags.lock().unwrap();
        f.join_err = err::CONNECT_FAILURE;
        main.cv.notify_all();
        return;
    }
    let sv = Server {
        host: format!("{}.{}.{}.{}", d[0], d[1], d[2], d[3]),
        port: get_u16(&d[20..]),
    };
    {
        let mut g = G.lock().unwrap();
        g.p2p_expected = true;
        g.p2p_ready = false;
    }
    let p = match conn_open(&sv, -1, true, false, true, false) {
        Some(p) => p,
        None => {
            eprintln!("[ryuldn] can't reach the host's own proxy at {}:{}", sv.host, sv.port);
            let mut f = main.flags.lock().unwrap();
            f.join_err = err::CONNECT_FAILURE;
            main.cv.notify_all();
            return;
        }
    };
    {
        // Only one proxy per lobby, and only while this connection is still its main one.
        let g = G.lock().unwrap();
        let keep = g.main.as_ref().map(|m| Arc::ptr_eq(m, main)).unwrap_or(false) && g.p2p.is_none();
        if keep {
            // (set below, after the Arc is cloned as p2p)
        } else {
            drop(g);
            eprintln!("[ryuldn] ignored an offer of the host's own proxy");
            p.close();
            return;
        }
    }
    {
        let mut g = G.lock().unwrap();
        g.p2p = Some(Arc::clone(&p));
    }
    p.send(msg::EXTERNAL_PROXY, &d[..0x26], &[]);
    eprintln!("[ryuldn] the host runs its own proxy ({}:{}): connected to it", sv.host, sv.port);
}

fn wait_joined(c: &Arc<Conn>) -> Result<(u32, u32, [u8; RYU_NETINFO]), u32> {
    let end = Instant::now() + Duration::from_millis(2 * CONNECT_MS);
    let err;
    loop {
        let p2p_wait = {
            let g = G.lock().unwrap();
            g.p2p_expected && !g.p2p_ready
        };
        {
            let f = c.flags.lock().unwrap();
            if f.join_err != err::NONE {
                err = f.join_err;
                break;
            }
            if f.join_done && f.have_proxy && !p2p_wait {
                err = err::NONE;
                break;
            }
            if !f.alive {
                err = err::CONNECT_FAILURE;
                break;
            }
        }
        if Instant::now() > end {
            let f = c.flags.lock().unwrap();
            err = if f.join_done { err::CONNECT_FAILURE } else { err::CONNECT_TIMEOUT };
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    if err == err::NONE {
        let f = c.flags.lock().unwrap();
        return Ok((f.proxy_ip, f.proxy_mask, f.info));
    }
    Err(match err {
        err::VERSION_TOO_LOW => R_VERSION_TOO_LOW,
        err::VERSION_TOO_HIGH => R_VERSION_TOO_HIGH,
        err::TOO_MANY_PLAYERS => R_TOO_MANY_PLAYERS,
        err::CONNECT_NOT_FOUND => R_CONNECT_NOT_FOUND,
        err::CONNECT_TIMEOUT => R_CONNECT_TIMEOUT,
        err::CONNECT_REJECTED => R_CONNECT_REJECTED,
        _ => R_CONNECT_FAILURE,
    })
}

fn begin(server: i32, host: bool) -> Result<Arc<Conn>, u32> {
    leave();
    let cfg_servers = with_cfg(|c| c.servers.clone());
    let h = host_addr();
    let sv = if host && !h.is_empty() && server < 0 {
        parse_server(&h).ok_or(R_CONNECT_FAILURE)?
    } else if server >= 0 && (server as usize) < cfg_servers.len() {
        cfg_servers[server as usize].clone()
    } else {
        return Err(R_CONNECT_NOT_FOUND);
    };
    let idx = if host && !h.is_empty() && server < 0 {
        -1
    } else {
        server
    };
    let c = conn_open(&sv, idx, true, true, false, false).ok_or(R_CONNECT_TIMEOUT)?;
    {
        let mut g = G.lock().unwrap();
        g.main = Some(Arc::clone(&c));
        g.in_lobby = true;
        g.p2p_expected = false;
        g.p2p_ready = false;
    }
    Ok(c)
}

fn fail_lobby() {
    G.lock().unwrap().in_lobby = false;
    leave();
}

pub fn create(
    server: i32,
    net_cfg: &[u8],
    sec_cfg: &[u8],
    user: &[u8],
    adv: &[u8],
) -> Result<(u32, u32, [u8; RYU_NETINFO]), u32> {
    let c = begin(server, true)?;
    let mut req = vec![0u8; CREATE_SIZE + ADV_MAX];
    req[..SEC_SIZE].copy_from_slice(&sec_cfg[..SEC_SIZE]);
    req[SEC_SIZE..SEC_SIZE + USER_SIZE].copy_from_slice(&user[..USER_SIZE]);
    req[0x74..0x74 + NETCFG_SIZE].copy_from_slice(&net_cfg[..NETCFG_SIZE]);
    req[0x94..0x94 + 6].copy_from_slice(b"1.4.0\0");
    let n = adv.len().min(ADV_MAX);
    req[CREATE_SIZE..CREATE_SIZE + n].copy_from_slice(&adv[..n]);
    c.send(msg::CREATE, &req[..CREATE_SIZE], &req[CREATE_SIZE..CREATE_SIZE + n]);
    match wait_joined(&c) {
        Ok(v) => {
            eprintln!(
                "[ryuldn] lobby created as {}.{}.{}.{}",
                v.0 >> 24,
                (v.0 >> 16) & 255,
                (v.0 >> 8) & 255,
                v.0 & 255
            );
            Ok(v)
        }
        Err(r) => {
            let r = if r == R_CONNECT_FAILURE { R_ACCESS_POINT_FAILED } else { r };
            fail_lobby();
            Err(r)
        }
    }
}

pub fn connect(
    server: i32,
    netinfo: &[u8],
    sec_cfg: &[u8],
    user: &[u8],
    lcv: u16,
) -> Result<(u32, u32, [u8; RYU_NETINFO]), u32> {
    let c = begin(server, false)?;
    let mut req = vec![0u8; CONNECT_SIZE];
    req[..SEC_SIZE].copy_from_slice(&sec_cfg[..SEC_SIZE]);
    req[SEC_SIZE..SEC_SIZE + USER_SIZE].copy_from_slice(&user[..USER_SIZE]);
    put_u32(&mut req[0x74..], lcv as u32);
    req[0x80..0x80 + RYU_NETINFO].copy_from_slice(&netinfo[..RYU_NETINFO]);
    c.send(msg::CONNECT, &req, &[]);
    match wait_joined(&c) {
        Ok(v) => {
            eprintln!(
                "[ryuldn] joined a lobby as {}.{}.{}.{}",
                v.0 >> 24,
                (v.0 >> 16) & 255,
                (v.0 >> 8) & 255,
                v.0 & 255
            );
            Ok(v)
        }
        Err(r) => {
            fail_lobby();
            Err(r)
        }
    }
}

pub fn leave() {
    let (m, p, was) = {
        let mut g = G.lock().unwrap();
        let m = g.main.take();
        let p = g.p2p.take();
        let was = g.in_lobby;
        g.in_lobby = false;
        (m, p, was)
    };
    if was {
        if let Some(m) = &m {
            m.send(msg::DISCONNECT, &[0, 0, 0, 0], &[]);
        }
    }
    if let Some(p) = p {
        p.close();
    }
    if let Some(m) = m {
        m.close();
    }
}

pub fn in_lobby() -> bool {
    G.lock().unwrap().in_lobby
}

pub fn set_advertise(data: &[u8]) {
    let m = {
        let g = G.lock().unwrap();
        if !g.in_lobby { None } else { g.main.clone() }
    };
    if let Some(m) = m {
        m.send(msg::SET_ADVERTISE, &data[..data.len().min(ADV_MAX)], &[]);
    }
}

pub fn set_accept_policy(policy: u8) {
    let m = {
        let g = G.lock().unwrap();
        if !g.in_lobby { None } else { g.main.clone() }
    };
    if let Some(m) = m {
        m.send(msg::SET_ACCEPT_POLICY, &[policy], &[]);
    }
}

pub fn reject(node_id: u32, reason: u32) -> bool {
    let m = G.lock().unwrap().main.clone();
    let Some(m) = m else { return false };
    if !G.lock().unwrap().in_lobby {
        return false;
    }
    {
        let mut f = m.flags.lock().unwrap();
        f.reject_done = false;
        f.net_err = err::NONE;
    }
    let mut r = [0u8; 8];
    put_u32(&mut r, node_id);
    put_u32(&mut r[4..], reason);
    m.send(msg::REJECT, &r, &[]);
    m.wait_flag(|f| f.reject_done, REJECT_MS);
    let f = m.flags.lock().unwrap();
    f.reject_done && f.net_err == err::NONE
}

pub fn send(p: &RyuProxy, data: &[u8]) {
    if PROXYINFO + 4 + data.len() > MAX_PAYLOAD {
        return;
    }
    let (m, p2p) = {
        let g = G.lock().unwrap();
        (g.main.clone(), g.p2p.clone())
    };
    if !G.lock().unwrap().in_lobby {
        return;
    }
    let mut h = [0u8; PROXYINFO + 4];
    put_u32(&mut h, p.src_ip);
    put_u16(&mut h[4..], p.src_port);
    put_u32(&mut h[6..], p.dst_ip);
    put_u16(&mut h[10..], p.dst_port);
    put_u32(&mut h[12..], p.proto);
    put_u32(&mut h[PROXYINFO..], data.len() as u32);
    if let Some(c) = p2p.or(m) {
        c.send(msg::PROXY_DATA, &h, data);
    }
}

/// One more client of the server we host on, for a player in a suyu room (xbridge).
#[derive(Clone)]
pub struct RyuPeer(Arc<Conn>);

pub fn peer_join(
    netinfo: &[u8],
    sec_cfg: &[u8],
    user: &[u8],
    lcv: u16,
    hooks: RyuPeerHooks,
    ud: *mut core::ffi::c_void,
) -> Result<(RyuPeer, u32, u32, [u8; RYU_NETINFO]), u32> {
    // The host's own server when it runs one, else the first configured server (the one create() used).
    let h = host_addr();
    let sv = if h.is_empty() {
        with_cfg(|c| c.servers.first().cloned()).ok_or(R_CONNECT_FAILURE)?
    } else {
        parse_server(&h).ok_or(R_CONNECT_FAILURE)?
    };
    let c = conn_open(&sv, -1, false, true, false, true).ok_or(R_CONNECT_TIMEOUT)?;
    {
        let mut f = c.flags.lock().unwrap();
        f.in_lobby = true;
        f.alive = true;
    }
    *c.peer_hooks.lock().unwrap() = Some((hooks, ud as usize));
    let mut req = vec![0u8; CONNECT_SIZE];
    req[..SEC_SIZE].copy_from_slice(&sec_cfg[..SEC_SIZE]);
    req[SEC_SIZE..SEC_SIZE + USER_SIZE].copy_from_slice(&user[..USER_SIZE]);
    put_u32(&mut req[0x74..], lcv as u32);
    req[0x80..0x80 + RYU_NETINFO].copy_from_slice(&netinfo[..RYU_NETINFO]);
    c.send(msg::CONNECT, &req, &[]);
    match wait_joined(&c) {
        Ok(v) => Ok((RyuPeer(c), v.0, v.1, v.2)),
        Err(r) => {
            c.close();
            Err(r)
        }
    }
}

pub fn peer_send(peer: &RyuPeer, p: &RyuProxy, data: &[u8]) {
    if PROXYINFO + 4 + data.len() > MAX_PAYLOAD {
        return;
    }
    let mut h = [0u8; PROXYINFO + 4];
    put_u32(&mut h, p.src_ip);
    put_u16(&mut h[4..], p.src_port);
    put_u32(&mut h[6..], p.dst_ip);
    put_u16(&mut h[10..], p.dst_port);
    put_u32(&mut h[12..], p.proto);
    put_u32(&mut h[PROXYINFO..], data.len() as u32);
    peer.0.send(msg::PROXY_DATA, &h, data);
}

pub fn peer_close(peer: &RyuPeer) {
    if peer.0.flags.lock().unwrap().in_lobby {
        peer.0.send(msg::DISCONNECT, &[0, 0, 0, 0], &[]);
    }
    peer.0.close();
}

// ---- scan (the game scans again and again; a worker asks the servers)
struct ScanState {
    running: bool,
    started: Option<Instant>,
    ok_at: Option<Instant>,
    filter: [u8; 0x60],
    infos: Vec<Vec<u8>>,
    server_of: Vec<usize>,
    down_until: Vec<Instant>,
}
static SCAN: Mutex<ScanState> = Mutex::new(ScanState {
    running: false,
    started: None,
    ok_at: None,
    filter: [0; 0x60],
    infos: Vec::new(),
    server_of: Vec::new(),
    down_until: Vec::new(),
});
static SCAN_CV: Condvar = Condvar::new();

fn scan_worker() {
    let (filter, servers, down_until) = {
        let s = SCAN.lock().unwrap();
        (s.filter, with_cfg(|c| c.servers.clone()), s.down_until.clone())
    };
    let mut stage = Vec::new();
    let mut server_of = Vec::new();
    let mut down = down_until;
    down.resize(servers.len(), Instant::now());
    for (i, sv) in servers.iter().enumerate() {
        if Instant::now() < down[i] {
            continue;
        }
        let c = match conn_open(sv, i as i32, false, true, false, false) {
            Some(c) => c,
            None => {
                down[i] = Instant::now() + Duration::from_secs(10);
                continue;
            }
        };
        let ok = c.send(msg::SCAN, &filter, &[]) && c.wait_flag(|f| f.scan_done, SCAN_MS);
        if !ok {
            down[i] = Instant::now() + Duration::from_secs(10);
        }
        let replies = std::mem::take(&mut *c.scan.lock().unwrap());
        for r in replies {
            stage.push(r);
            server_of.push(i);
        }
        c.close();
    }
    let mut s = SCAN.lock().unwrap();
    s.infos = stage;
    s.server_of = server_of;
    s.down_until = down;
    s.ok_at = Some(Instant::now());
    s.running = false;
    SCAN_CV.notify_all();
}

pub fn scan(filter: &[u8]) -> Vec<(Vec<u8>, usize)> {
    with_cfg(|c| {
        if !c.loaded {
            c.loaded = true;
        }
    });
    let n = server_count();
    if n == 0 {
        return Vec::new();
    }
    let mut start_worker = false;
    let first;
    {
        let mut s = SCAN.lock().unwrap();
        first = s.ok_at.is_none() && !s.running;
        if !s.running && s.started.map(|t| t.elapsed() > Duration::from_secs(2)).unwrap_or(true) {
            s.filter = [0u8; 0x60];
            s.filter[..filter.len().min(0x60)].copy_from_slice(&filter[..filter.len().min(0x60)]);
            s.running = true;
            s.started = Some(Instant::now());
            start_worker = true;
        }
    }
    if start_worker {
        std::thread::spawn(scan_worker);
    }
    if first {
        let mut s = SCAN.lock().unwrap();
        while s.running {
            let (g, _) = SCAN_CV.wait_timeout(s, Duration::from_millis(5)).unwrap();
            s = g;
        }
    }
    let s = SCAN.lock().unwrap();
    s.infos.iter().cloned().zip(s.server_of.iter().copied()).collect()
}
