//! Mixed hubs (xbridge): a hub hosted on a Ryujinx LDN server that suyu-room players can join too.
//! Ported from runtime/net/xbridge.c. The host's game is the room's host member; every room player
//! that joins is bridged as one more client of the Ryu server (a "peer"), and addresses/MACs are
//! rewritten between the room's view (192.168.x.y) and the server's (10.114.x.y).

use crate::room;
use crate::ryu::{self, RyuPeer, RyuPeerHooks, RyuProxy};
use std::sync::{LazyLock, Mutex};

const NETINFO: usize = 0x480;
const NODE: usize = 0x40;
const MAX_NODES: usize = 8;
const MAXP: usize = 7;
const NI_BSSID: usize = 0x20;
const NI_SSID: usize = 0x26;
const NI_NODE_MAX: usize = 0x66;
const NI_NODE_COUNT: usize = 0x67;
const NI_NODES: usize = 0x68;
const NODE_IP: usize = 0x00;
const NODE_MAC: usize = 0x04;
const NODE_CONNECTED: usize = 0x0B;
const NODE_NAME: usize = 0x0C;
const NODE_LCV: usize = 0x2E;
const MAXSUB: usize = 64;

fn rd32(d: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
}
fn wr32(d: &mut [u8], at: usize, v: u32) {
    d[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn rd16(d: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([d[at], d[at + 1]])
}
fn ip_u32(b: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*b)
}
fn ip_bytes(ip: u32) -> [u8; 4] {
    ip.to_be_bytes()
}
fn alias_ip(slot: usize) -> u32 {
    0xC0A8_FA00 | slot as u32
}
fn node(info: &[u8], i: usize) -> &[u8] {
    &info[NI_NODES + i * NODE..NI_NODES + (i + 1) * NODE]
}
fn node_mut(info: &mut [u8], i: usize) -> &mut [u8] {
    &mut info[NI_NODES + i * NODE..NI_NODES + (i + 1) * NODE]
}

#[derive(Clone, Copy)]
struct Sub {
    from: [u8; 6],
    to: [u8; 6],
    len: usize,
}

struct Peer {
    used: bool,
    joined: bool,
    dead: bool,
    room_ip: [u8; 4],
    mac: [u8; 6],
    node: [u8; NODE],
    ryu_ip: u32,
    rp: Option<RyuPeer>,
}

impl Default for Peer {
    fn default() -> Peer {
        Peer {
            used: false,
            joined: false,
            dead: false,
            room_ip: [0; 4],
            mac: [0; 6],
            node: [0; NODE],
            ryu_ip: 0,
            rp: None,
        }
    }
}

struct Bridge {
    active: bool,
    lobby: [u8; NETINFO],
    host_ip: u32,
    host_mask: u32,
    peers: [Peer; MAXP],
}

static B: LazyLock<Mutex<Bridge>> = LazyLock::new(|| {
    Mutex::new(Bridge {
        active: false,
        lobby: [0; NETINFO],
        host_ip: 0,
        host_mask: 0,
        peers: Default::default(),
    })
});

fn host_mac(room_ip: &[u8; 4]) -> [u8; 6] {
    [0x02, 0, room_ip[0], room_ip[1], room_ip[2], room_ip[3]]
}

fn add_ip(s: &mut Vec<Sub>, from: u32, to: u32) {
    if from == 0 || to == 0 || from == to || s.len() + 2 > MAXSUB {
        return;
    }
    for be in [true, false] {
        let mut f = [0u8; 6];
        let mut t = [0u8; 6];
        for i in 0..4 {
            f[i] = if be { (from >> (24 - 8 * i)) as u8 } else { (from >> (8 * i)) as u8 };
            t[i] = if be { (to >> (24 - 8 * i)) as u8 } else { (to >> (8 * i)) as u8 };
        }
        s.push(Sub { from: f, to: t, len: 4 });
    }
}
fn add_mac(s: &mut Vec<Sub>, from: &[u8; 6], to: &[u8; 6]) {
    if s.len() >= MAXSUB || from == to || *from == [0; 6] || *to == [0; 6] {
        return;
    }
    let mut f = [0u8; 6];
    let mut t = [0u8; 6];
    f.copy_from_slice(from);
    t.copy_from_slice(to);
    s.push(Sub { from: f, to: t, len: 6 });
}

fn build_subs(b: &Bridge, own: [u8; 4]) -> Vec<Sub> {
    let hm = host_mac(&own);
    let mut s = Vec::with_capacity(MAXSUB);
    add_ip(&mut s, ip_u32(&own), b.host_ip);
    add_mac(&mut s, &hm, &node(&b.lobby, 0)[NODE_MAC..NODE_MAC + 6].try_into().unwrap());
    for p in &b.peers {
        if !p.used || !p.joined {
            continue;
        }
        add_ip(&mut s, ip_u32(&p.room_ip), p.ryu_ip);
        for k in 1..MAX_NODES {
            let nd = node(&b.lobby, k);
            if nd[NODE_CONNECTED] != 0 && rd32(nd, NODE_IP) == p.ryu_ip {
                let to: [u8; 6] = nd[NODE_MAC..NODE_MAC + 6].try_into().unwrap();
                add_mac(&mut s, &p.mac, &to);
            }
        }
    }
    for k in 1..MAX_NODES {
        let nd = node(&b.lobby, k);
        if nd[NODE_CONNECTED] != 0 {
            add_ip(&mut s, alias_ip(k), rd32(nd, NODE_IP));
        }
    }
    s
}

fn translate(d: &mut [u8], subs: &[Sub], to_hub: bool) {
    for s in subs {
        let (from, to) = if to_hub { (&s.from, &s.to) } else { (&s.to, &s.from) };
        let mut i = 0;
        while i + s.len <= d.len() {
            if &d[i..i + s.len] == &from[..s.len] {
                d[i..i + s.len].copy_from_slice(&to[..s.len]);
                i += s.len;
            } else {
                i += 1;
            }
        }
    }
}

fn room_view(b: &Bridge, own: [u8; 4]) -> [u8; NETINFO] {
    let mut out = b.lobby;
    let hm = host_mac(&own);
    out[NI_BSSID..NI_BSSID + 6].copy_from_slice(&hm);
    let ssid = b"\x12YuzuFakeSsidForLdn";
    out[NI_SSID..NI_SSID + ssid.len()].copy_from_slice(ssid);
    for i in 0..MAX_NODES {
        let nd = node_mut(&mut out, i);
        if nd[NODE_CONNECTED] == 0 {
            continue;
        }
        if i == 0 {
            wr32(nd, NODE_IP, ip_u32(&own));
            nd[NODE_MAC..NODE_MAC + 6].copy_from_slice(&hm);
            continue;
        }
        let ryu = rd32(&b.lobby, NI_NODES + i * NODE + NODE_IP);
        let mut mapped = alias_ip(i);
        let mut mac: Option<[u8; 6]> = None;
        for p in &b.peers {
            if p.used && p.joined && p.ryu_ip == ryu {
                mapped = ip_u32(&p.room_ip);
                mac = Some(p.mac);
            }
        }
        wr32(nd, NODE_IP, mapped);
        if let Some(m) = mac {
            nd[NODE_MAC..NODE_MAC + 6].copy_from_slice(&m);
        }
    }
    out
}

fn send_ldn(ty: u8, to: &[u8; 4], data: &[u8]) {
    let own = room::own_ip();
    room::send_ldn(ty, own, *to, false, data);
}

fn sync_all() {
    let own = room::own_ip();
    let (view, targets) = {
        let b = B.lock().unwrap();
        if !b.active {
            return;
        }
        let view = room_view(&b, own);
        let targets: Vec<[u8; 4]> = b.peers.iter().filter(|p| p.used && p.joined).map(|p| p.room_ip).collect();
        (view, targets)
    };
    for t in targets {
        send_ldn(crate::room::RLDN_SYNC_NETWORK, &t, &view);
    }
}

fn peer_by_room(b: &Bridge, ip: &[u8; 4]) -> Option<usize> {
    b.peers.iter().position(|p| p.used && &p.room_ip == ip)
}
fn peer_by_ryu(b: &Bridge, ip: u32) -> Option<usize> {
    b.peers.iter().position(|p| p.used && p.joined && p.ryu_ip == ip)
}

fn retire(index: usize, tell: bool) {
    let (room_ip, rp) = {
        let mut b = B.lock().unwrap();
        let p = &mut b.peers[index];
        if !p.used || p.dead {
            return;
        }
        p.dead = true;
        p.joined = false;
        let room_ip = p.room_ip;
        let rp = p.rp.take();
        p.used = false;
        (room_ip, rp)
    };
    if tell {
        send_ldn(crate::room::RLDN_DESTROY_NETWORK, &room_ip, &[]);
    }
    if let Some(rp) = rp {
        std::thread::spawn(move || ryu::peer_close(&rp));
    }
}

extern "C" fn peer_lobby(ud: *mut core::ffi::c_void, info: *const u8) {
    if info.is_null() {
        retire(ud as usize, true);
    }
}

extern "C" fn peer_data(ud: *mut core::ffi::c_void, r: *const RyuProxy, data: *const u8, len: u32) {
    let index = ud as usize;
    let Some(r) = (unsafe { r.as_ref() }) else { return };
    if r.proto != 17 || len as usize > 0x10000 {
        return;
    }
    let own = room::own_ip();
    let (target, subs, sip, sport, dport, payload) = {
        let b = B.lock().unwrap();
        let Some(p) = b.peers.get(index) else { return };
        if !b.active || !p.used || !p.joined {
            return;
        }
        let bcast = (b.host_ip & b.host_mask) | !b.host_mask;
        if !(r.dst_ip == p.ryu_ip || r.dst_ip == bcast || r.dst_ip == 0xFFFF_FFFF) {
            return;
        }
        if peer_by_ryu(&b, r.src_ip).is_some() {
            return;
        }
        let sip = if r.src_ip == b.host_ip {
            ip_u32(&own)
        } else {
            let mut found = 0;
            for k in 1..MAX_NODES {
                let nd = node(&b.lobby, k);
                if nd[NODE_CONNECTED] != 0 && rd32(nd, NODE_IP) == r.src_ip {
                    found = alias_ip(k);
                }
            }
            if found == 0 {
                return;
            }
            found
        };
        (p.room_ip, build_subs(&b, own), sip, r.src_port, r.dst_port, unsafe {
            std::slice::from_raw_parts(data, len as usize)
        }.to_vec())
    };
    let mut buf = payload;
    translate(&mut buf, &subs, false);
    room::send_proxy(ip_bytes(sip), sport, target, dport, 4, false, &buf);
}

fn join_worker(index: usize, lobby: [u8; NETINFO], node_info: [u8; NODE]) {
    let sec = {
        let mut s = [0u8; 0x44];
        s[..2].copy_from_slice(&lobby[0x60..0x62]);
        s
    };
    let mut user = [0u8; 0x30];
    user[..0x20].copy_from_slice(&node_info[NODE_NAME..NODE_NAME + 0x20]);
    let mut lcv = rd16(&node_info, NODE_LCV);
    if lcv == 0 {
        lcv = rd16(&lobby, NI_NODES + NODE_LCV);
    }
    let hooks = RyuPeerHooks { lobby: Some(peer_lobby), data: Some(peer_data) };
    let r = ryu::peer_join(&lobby, &sec, &user, lcv, hooks, index as *mut core::ffi::c_void);
    match r {
        Ok((peer, ip, _mask, out)) => {
            let keep = {
                let mut b = B.lock().unwrap();
                let keep = b.active && b.peers.get(index).map(|p| p.used && !p.dead).unwrap_or(false);
                if keep {
                    b.peers[index].rp = Some(peer);
                    b.peers[index].ryu_ip = ip;
                    b.peers[index].joined = true;
                    b.lobby = out;
                }
                keep
            };
            if keep {
                eprintln!(
                    "[xbridge] a room player joined the hub as {}.{}.{}.{}",
                    ip >> 24,
                    (ip >> 16) & 255,
                    (ip >> 8) & 255,
                    ip & 255
                );
                sync_all();
            } else {
                let rp = { B.lock().unwrap().peers[index].rp.take() };
                if let Some(rp) = rp {
                    ryu::peer_close(&rp);
                }
                let room_ip = { B.lock().unwrap().peers[index].room_ip };
                {
                    let mut b = B.lock().unwrap();
                    b.peers[index].used = false;
                }
                send_ldn(crate::room::RLDN_DESTROY_NETWORK, &room_ip, &[]);
            }
        }
        Err(res) => {
            eprintln!("[xbridge] a room player could not be joined to the hub (result {res:#x})");
            let room_ip = { B.lock().unwrap().peers[index].room_ip };
            {
                let mut b = B.lock().unwrap();
                b.peers[index].used = false;
            }
            send_ldn(crate::room::RLDN_DESTROY_NETWORK, &room_ip, &[]);
        }
    }
}

pub fn begin(info: &[u8], host_ip: u32, mask: u32) {
    let mut b = B.lock().unwrap();
    b.lobby.copy_from_slice(&info[..NETINFO]);
    b.host_ip = host_ip;
    b.host_mask = mask;
    b.peers = Default::default();
    b.active = true;
    drop(b);
    eprintln!("[xbridge] the hub is also offered to the room's players");
}

pub fn end() {
    {
        let b = B.lock().unwrap();
        if !b.active {
            return;
        }
    }
    let used: Vec<usize> = {
        let b = B.lock().unwrap();
        b.peers.iter().enumerate().filter(|(_, p)| p.used && !p.dead).map(|(i, _)| i).collect()
    };
    for i in used {
        retire(i, true);
    }
    B.lock().unwrap().active = false;
    eprintln!("[xbridge] the hub is no longer offered to the room");
}

pub fn active() -> bool {
    B.lock().unwrap().active
}

pub fn players() -> i32 {
    B.lock().unwrap().peers.iter().filter(|p| p.used && p.joined).count() as i32
}

pub fn lobby(info: Option<&[u8]>) {
    match info {
        None => end(),
        Some(info) => {
            {
                let mut b = B.lock().unwrap();
                if b.active {
                    b.lobby.copy_from_slice(&info[..NETINFO]);
                }
            }
            sync_all();
        }
    }
}

pub fn room_ldn(ty: u8, lip: &[u8; 4], _rip: &[u8; 4], _bc: bool, data: &[u8]) {
    match ty {
        crate::room::RLDN_SCAN => {
            eprintln!("[xbridge] a room player scans ({}.{}.{}.{})", lip[0], lip[1], lip[2], lip[3]);
            let own = room::own_ip();
            let view = {
                let b = B.lock().unwrap();
                let n = b.lobby[NI_NODE_COUNT];
                let ok = b.active && n > 0 && n < b.lobby[NI_NODE_MAX] && b.lobby[0x62] != 1;
                if ok {
                    Some(room_view(&b, own))
                } else {
                    None
                }
            };
            if let Some(v) = view {
                send_ldn(crate::room::RLDN_SCAN_RESP, lip, &v);
            }
        }
        crate::room::RLDN_CONNECT => {
            eprintln!(
                "[xbridge] a room player wants to join ({}.{}.{}.{}, {} bytes)",
                lip[0],
                lip[1],
                lip[2],
                lip[3],
                data.len()
            );
            if data.len() < NODE {
                return;
            }
            let (index, lobby) = {
                let mut b = B.lock().unwrap();
                if peer_by_room(&b, lip).is_some() {
                    drop(b);
                    sync_all();
                    return;
                }
                let k = match b.peers.iter().position(|p| !p.used) {
                    Some(k) => k,
                    None => {
                        drop(b);
                        send_ldn(crate::room::RLDN_DESTROY_NETWORK, lip, &[]);
                        return;
                    }
                };
                if !b.active || b.lobby[NI_NODE_COUNT] >= b.lobby[NI_NODE_MAX] {
                    drop(b);
                    send_ldn(crate::room::RLDN_DESTROY_NETWORK, lip, &[]);
                    return;
                }
                let mut n = [0u8; NODE];
                n.copy_from_slice(&data[..NODE]);
                let mac: [u8; 6] = n[NODE_MAC..NODE_MAC + 6].try_into().unwrap();
                b.peers[k] = Peer {
                    used: true,
                    joined: false,
                    dead: false,
                    room_ip: *lip,
                    mac,
                    node: n,
                    ryu_ip: 0,
                    rp: None,
                };
                (k, b.lobby)
            };
            let node = { B.lock().unwrap().peers[index].node };
            std::thread::spawn(move || join_worker(index, lobby, node));
        }
        crate::room::RLDN_DISCONNECT => {
            let idx = {
                let b = B.lock().unwrap();
                peer_by_room(&b, lip)
            };
            if let Some(i) = idx {
                retire(i, false);
            }
        }
        _ => {}
    }
}

pub fn room_members(ips: &[[u8; 4]]) {
    let stale: Vec<usize> = {
        let b = B.lock().unwrap();
        b.peers
            .iter()
            .enumerate()
            .filter(|(_, p)| p.used && !p.dead && !ips.iter().any(|ip| ip == &p.room_ip))
            .map(|(i, _)| i)
            .collect()
    };
    for i in stale {
        retire(i, false);
    }
}

pub fn room_proxy(sip: &[u8; 4], sport: u16, dip: &[u8; 4], dport: u16, proto: u8, bc: bool, data: &[u8]) {

    if proto != 4 || data.len() > 0x10000 {
        return;
    }
    let own = room::own_ip();
    let (dst, subs, payload) = {
        let b = B.lock().unwrap();
        let Some(idx) = (if b.active { peer_by_room(&b, sip) } else { None }) else { return };
        let Some(p) = b.peers.get(idx) else { return };
        if !p.joined || p.rp.is_none() {
            return;
        }
        let d = ip_u32(dip);
        let dst = if bc || d == 0xFFFF_FFFF || (d & 0xFF) == 0xFF {
            (b.host_ip & b.host_mask) | !b.host_mask
        } else if d == ip_u32(&own) {
            b.host_ip
        } else if (d & 0xFFFF_FF00) == 0xC0A8_FA00 {
            let k = (d & 0xFF) as usize;
            if k >= 1 && k < MAX_NODES && node(&b.lobby, k)[NODE_CONNECTED] != 0 {
                rd32(&b.lobby, NI_NODES + k * NODE + NODE_IP)
            } else {
                0
            }
        } else {
            0
        };
        if dst == 0 {
            return;
        }
        (dst, build_subs(&b, own), data.to_vec())
    };
    let mut buf = payload;
    translate(&mut buf, &subs, true);
    let (rp, peer_ip) = {
        let b = B.lock().unwrap();
        let Some(idx) = peer_by_room(&b, sip) else { return };
        (b.peers[idx].rp.clone(), b.peers[idx].ryu_ip)
    };
    if let Some(rp) = rp {
        let r = RyuProxy { src_ip: peer_ip, dst_ip: dst, src_port: sport, dst_port: dport, proto: 17 };
        ryu::peer_send(&rp, &r, &buf);
    }
}
