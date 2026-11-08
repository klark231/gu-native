//! A suyu-family multiplayer room that runs inside the game: the room server written from the
//! members' protocol (runtime/net/roomsrv.c). Members join, are told who is in, their LDN and game
//! packets are relayed, and a packet for an address nobody has goes to the host (the gateway).

use rusty_enet::{Event, Host, HostSettings, Packet, PeerID};
use std::collections::HashMap;
use std::net::UdpSocket;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{LazyLock, Mutex};
use std::time::{Duration, Instant};

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
const ROOM_VERSION: u32 = 1;
const MAX_MEMBERS: usize = 16;
const NAME_MAX: usize = 63;
const TEXT_MAX: usize = 96;
const MAX_MESSAGE: usize = 0x20000;
const JOIN_MS: u64 = 10_000;

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
    fn new(d: &'a [u8], at: usize) -> Rd<'a> {
        Rd { d, at, bad: false }
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
    fn u32(&mut self) -> u32 {
        self.bytes(4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]])).unwrap_or(0)
    }
    fn u64(&mut self) -> u64 {
        let hi = self.u32() as u64;
        hi << 32 | self.u32() as u64
    }
    fn string(&mut self, max: usize) -> String {
        let n = self.u32() as usize;
        match self.bytes(n) {
            Some(q) => String::from_utf8_lossy(&q[..q.len().min(max - 1)]).into_owned(),
            None => String::new(),
        }
    }
}

struct Member {
    peer: PeerID,
    nick: String,
    ip: [u8; 4],
    game: String,
    version: String,
    game_id: u64,
    host: bool,
}

struct Srv {
    up: bool,
    name: String,
    password: String,
    host_nick: String,
    port: u16,
    members: Vec<Member>,
}

static SRV: LazyLock<Mutex<Srv>> = LazyLock::new(|| {
    Mutex::new(Srv {
        up: false,
        name: String::new(),
        password: String::new(),
        host_nick: String::new(),
        port: 0,
        members: Vec::new(),
    })
});
static RUN: AtomicBool = AtomicBool::new(false);
static THREAD: Mutex<Option<std::thread::JoinHandle<()>>> = Mutex::new(None);

fn send_to(host: &mut Host<UdpSocket>, to: PeerID, d: &[u8]) {
    let _ = host.peer_mut(to).send(0, &Packet::reliable(d.to_vec()));
}

fn broadcast_info(host: &mut Host<UdpSocket>, srv: &Srv) {
    let mut m = Vec::new();
    m.push(M_ROOM_INFO);
    wr_str(&mut m, &srv.name);
    wr_str(&mut m, "");
    m.extend_from_slice(&put_u32(MAX_MEMBERS as u32));
    m.extend_from_slice(&put_u16(srv.port));
    wr_str(&mut m, "");
    wr_str(&mut m, &srv.host_nick);
    m.extend_from_slice(&put_u32(srv.members.len() as u32));
    for p in &srv.members {
        wr_str(&mut m, &p.nick);
        m.extend_from_slice(&p.ip);
        wr_str(&mut m, &p.game);
        m.extend_from_slice(&put_u64(p.game_id));
        wr_str(&mut m, &p.version);
        wr_str(&mut m, "");
        wr_str(&mut m, "");
        wr_str(&mut m, "");
    }
    for p in &srv.members {
        send_to(host, p.peer, &m);
    }
}

fn by_ip(srv: &Srv, ip: &[u8; 4]) -> Option<usize> {
    srv.members.iter().position(|m| &m.ip == ip)
}
fn by_nick(srv: &Srv, nick: &str) -> Option<usize> {
    srv.members.iter().position(|m| m.nick == nick)
}
fn reserved(ip: &[u8; 4]) -> bool {
    ip[0] == 192 && ip[1] == 168 && ip[2] == 250
}
fn free_ip(srv: &Srv) -> [u8; 4] {
    for i in 1..255u16 {
        for j in 1..255u16 {
            let c = [10, 255, i as u8, j as u8];
            if by_ip(srv, &c).is_none() {
                return c;
            }
        }
    }
    [10, 255, 0, 0]
}

fn on_join(host: &mut Host<UdpSocket>, srv: &mut Srv, peer: PeerID, d: &[u8], loopback: bool) {
    if srv.members.iter().any(|m| m.peer == peer) {
        return;
    }
    if srv.members.len() >= MAX_MEMBERS {
        send_to(host, peer, &[M_ROOM_IS_FULL]);
        return;
    }
    let mut r = Rd::new(d, 1);
    let nick = r.string(NAME_MAX + 1);
    let want = r.bytes(4).map(|b| [b[0], b[1], b[2], b[3]]);
    let version = r.u32();
    let pass = r.string(64);
    let _token = r.string(64);
    let Some(want) = want else { return };
    if r.bad {
        return;
    }
    if pass != srv.password {
        send_to(host, peer, &[M_WRONG_PASSWORD]);
        host.peer_mut(peer).disconnect_later(0);
        return;
    }
    let host_member = !srv.host_nick.is_empty() && nick == srv.host_nick;
    if nick.is_empty() || by_nick(srv, &nick).is_some() || (host_member && !loopback) {
        send_to(host, peer, &[M_NAME_COLLISION]);
        return;
    }
    let ip = if want != [0xFF; 4] {
        if by_ip(srv, &want).is_some() || reserved(&want) {
            send_to(host, peer, &[M_IP_COLLISION]);
            return;
        }
        want
    } else {
        free_ip(srv)
    };
    if version != ROOM_VERSION {
        let mut b = vec![M_VERSION_MISMATCH, 0, 0, 0];
        b.extend_from_slice(&put_u32(ROOM_VERSION));
        send_to(host, peer, &b);
        return;
    }
    srv.members.push(Member {
        peer,
        nick,
        ip,
        game: String::new(),
        version: String::new(),
        game_id: 0,
        host: host_member,
    });
    let mut ok = vec![M_JOIN_SUCCESS];
    ok.extend_from_slice(&ip);
    send_to(host, peer, &ok);
    broadcast_info(host, srv);
}

fn on_game_info(host: &mut Host<UdpSocket>, srv: &mut Srv, peer: PeerID, d: &[u8]) {
    let mut r = Rd::new(d, 1);
    let game = r.string(TEXT_MAX);
    let id = r.u64();
    let version = r.string(32);
    if r.bad {
        return;
    }
    if let Some(m) = srv.members.iter_mut().find(|m| m.peer == peer) {
        m.game = game;
        m.version = version;
        m.game_id = id;
    }
    broadcast_info(host, srv);
}

fn relay(host: &mut Host<UdpSocket>, srv: &Srv, from: PeerID, d: &[u8], off: usize, bc_off: usize) {
    let Some(m) = srv.members.iter().find(|m| m.peer == from) else { return };
    if d.len() < bc_off + 1 {
        return;
    }
    if !m.host && d[2..6] != m.ip {
        return;
    }
    if d[bc_off] != 0 {
        for p in &srv.members {
            if p.peer != from {
                send_to(host, p.peer, d);
            }
        }
        return;
    }
    let to = d.get(off..off + 4).and_then(|a| {
        let ip = [a[0], a[1], a[2], a[3]];
        by_ip(srv, &ip)
    });
    let idx = to.or_else(|| srv.members.iter().position(|m| m.host && m.peer != from));
    if let Some(i) = idx {
        send_to(host, srv.members[i].peer, d);
    }
}

fn on_message(host: &mut Host<UdpSocket>, srv: &mut Srv, peer: PeerID, d: &[u8], loopback: bool) {
    if d.is_empty() {
        return;
    }
    match d[0] {
        M_JOIN_REQUEST => on_join(host, srv, peer, d, loopback),
        M_SET_GAME_INFO => on_game_info(host, srv, peer, d),
        M_PROXY_PACKET => relay(host, srv, peer, d, 1 + 1 + 4 + 2 + 1, 1 + 7 + 7 + 1),
        M_LDN_PACKET => relay(host, srv, peer, d, 1 + 1 + 4, 1 + 1 + 4 + 4),
        _ => {}
    }
}

fn on_leave(host: &mut Host<UdpSocket>, srv: &mut Srv, peer: PeerID) {
    if let Some(i) = srv.members.iter().position(|m| m.peer == peer) {
        srv.members.remove(i);
        broadcast_info(host, srv);
    }
}

fn room_loop(mut host: Host<UdpSocket>) {
    let mut pending: HashMap<PeerID, Instant> = HashMap::new();
    while RUN.load(Ordering::Relaxed) {
        loop {
            enum Act { Connect(PeerID), Recv(PeerID, Vec<u8>, bool), Gone(PeerID), Done }
            let act = match host.service() {
                Ok(Some(Event::Connect { peer, .. })) => Act::Connect(peer.id()),
                Ok(Some(Event::Receive { peer, packet, .. })) => {
                    let id = peer.id();
                    let loopback = peer.address().map(|a| a.ip().is_loopback()).unwrap_or(false);
                    let d = if packet.data().len() <= MAX_MESSAGE { packet.data().to_vec() } else { Vec::new() };
                    Act::Recv(id, d, loopback)
                }
                Ok(Some(Event::Disconnect { peer, .. })) => Act::Gone(peer.id()),
                Ok(None) => Act::Done,
                Err(_) => Act::Done,
            };
            match act {
                Act::Connect(id) => {
                    let peer = host.peer_mut(id);
                    peer.set_ping_interval(1000);
                    peer.set_timeout(2, 5000, 15000);
                    pending.insert(id, Instant::now());
                }
                Act::Recv(id, d, loopback) if !d.is_empty() => {
                    let mut srv = SRV.lock().unwrap();
                    on_message(&mut host, &mut srv, id, &d, loopback);
                }
                Act::Gone(id) => {
                    pending.remove(&id);
                    let mut srv = SRV.lock().unwrap();
                    on_leave(&mut host, &mut srv, id);
                }
                _ => break,
            }
        }
        {
            let mut srv = SRV.lock().unwrap();
            let now = Instant::now();
            let stale: Vec<PeerID> = pending
                .iter()
                .filter(|(id, at)| {
                    now.duration_since(**at) > Duration::from_millis(JOIN_MS)
                        && !srv.members.iter().any(|m| m.peer == **id)
                })
                .map(|(id, _)| *id)
                .collect();
            for id in stale {
                pending.remove(&id);
                host.peer_mut(id).disconnect_now(0);
            }
        }
        host.flush();
        std::thread::sleep(Duration::from_millis(5));
    }
    let msgs: Vec<(PeerID, Vec<u8>)> = {
        let srv = SRV.lock().unwrap();
        srv.members.iter().map(|m| (m.peer, vec![M_CLOSE_ROOM])).collect()
    };
    for (id, m) in msgs {
        send_to(&mut host, id, &m);
        host.peer_mut(id).disconnect_now(0);
    }
    host.flush();
    SRV.lock().unwrap().members.clear();
}

pub fn start(port: u16, name: &str, password: &str, host_nick: &str) -> Result<(), String> {
    if SRV.lock().unwrap().up {
        return Ok(());
    }
    let sock = UdpSocket::bind(("0.0.0.0", port)).map_err(|_| "the port may be in use".to_string())?;
    let settings = HostSettings {
        peer_limit: MAX_MEMBERS + 1,
        channel_limit: 1,
        ..Default::default()
    };
    let host = Host::new(sock, settings).map_err(|_| "could not start the room".to_string())?;
    {
        let mut s = SRV.lock().unwrap();
        s.name = if name.is_empty() { "MHGU".into() } else { name.into() };
        s.password = password.into();
        s.host_nick = host_nick.into();
        s.port = port;
        s.members.clear();
        s.up = true;
    }
    RUN.store(true, Ordering::Relaxed);
    *THREAD.lock().unwrap() = Some(std::thread::spawn(move || room_loop(host)));
    Ok(())
}

pub fn stop() {
    if !SRV.lock().unwrap().up {
        return;
    }
    RUN.store(false, Ordering::Relaxed);
    if let Some(h) = THREAD.lock().unwrap().take() {
        let _ = h.join();
    }
    SRV.lock().unwrap().up = false;
}

pub fn running() -> bool {
    SRV.lock().unwrap().up
}
