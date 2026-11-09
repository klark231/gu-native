//! "All rooms" hosting: the hub this game hosts (runtime/net/ldn.c) let into a Yuzu-family room too, beside the
//! LAN / hub server and the Citra room (citra_bridge.rs). The hub is ldn.c's (its own subnet, 169.254.x.0); to the
//! room's players it looks like a room hub of ours, as xbridge (bridge.rs) shows a RyuLdn lobby:
//!  - we are its host at our room IP, each room player at its own room IP, and everyone else in the hub (LAN, hub
//!    server and Citra players) at an alias, 192.168.250.k (k: the node's slot), which a room passes to its host member
//!    when no member has it (the room this app runs does; another room software may not);
//!  - the room's view and the hub's differ only in those IPs and MACs: they are swapped in every packet both ways.
//!  - the game is the same on both sides (MHGU), so nothing else changes.
//! `Door` is the protocol (events in, actions out, for the tests); the driver and the C API are at the end.

use crate::room::{self, RLDN_CONNECT, RLDN_DESTROY_NETWORK, RLDN_DISCONNECT, RLDN_SCAN, RLDN_SCAN_RESP, RLDN_SYNC_NETWORK};
use std::sync::Mutex;

pub const NETINFO: usize = 0x480;
const NODE: usize = 0x40;
const MAX_NODES: usize = 8;
const NI_BSSID: usize = 0x20;
const NI_SSID: usize = 0x26;
const NI_ACCEPT_POLICY: usize = 0x62;
const NI_NODE_MAX: usize = 0x66;
const NI_NODE_COUNT: usize = 0x67;
const NI_NODES: usize = 0x68;
const N_IP: usize = 0x00;
const N_MAC: usize = 0x04;
const N_ID: usize = 0x0A;
const N_CONNECTED: usize = 0x0B;
const N_LCV: usize = 0x2E;
const ROOM_SSID: &[u8] = b"\x12YuzuFakeSsidForLdn"; // (what the emulators' scans expect)

fn rd32(d: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
}
fn wr32(d: &mut [u8], at: usize, v: u32) {
    d[at..at + 4].copy_from_slice(&v.to_le_bytes());
}
fn slot(info: &[u8], i: usize) -> &[u8] {
    &info[NI_NODES + i * NODE..NI_NODES + (i + 1) * NODE]
}
fn slot_mut(info: &mut [u8], i: usize) -> &mut [u8] {
    &mut info[NI_NODES + i * NODE..NI_NODES + (i + 1) * NODE]
}
fn ip_u32(b: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*b)
}
pub fn alias_ip(k: usize) -> u32 {
    0xC0A8_FA00 | k as u32
}
fn host_mac(room_ip: &[u8; 4]) -> [u8; 6] {
    [0x02, 0, room_ip[0], room_ip[1], room_ip[2], room_ip[3]]
}

#[derive(Debug, PartialEq, Eq)]
pub enum DoorOut {
    /// an LDN message to a room member
    Ldn { ty: u8, to: [u8; 4], data: Vec<u8> },
    /// a game packet into the room (room view)
    Proxy { src: [u8; 4], sport: u16, dst: [u8; 4], dport: u16, bc: bool, data: Vec<u8> },
    /// the hub with the room's players in it
    Info(Vec<u8>),
    /// a room player's packet for the hub (hub view)
    Packet { src: u32, dst: u32, sport: u16, dport: u16, data: Vec<u8> },
}

#[derive(Clone)]
struct Member {
    room_ip: [u8; 4],
    node: [u8; NODE], // as it joined (name, MAC, version)
}

pub struct Door {
    own: [u8; 4], // our room IP
    subnet: u32,
    hub: Vec<u8>, // ldn.c's hub without our members
    members: Vec<Option<Member>>, // by slot; [0] the host
    pub log: Vec<String>,
}

impl Door {
    pub fn new(hub: &[u8], own: [u8; 4]) -> Door {
        let subnet = rd32(hub, NI_NODES + N_IP) & 0xFFFF_FF00;
        Door { own, subnet, hub: hub[..NETINFO].to_vec(), members: vec![None; MAX_NODES], log: Vec::new() }
    }
    fn max(&self) -> usize {
        (self.hub[NI_NODE_MAX] as usize).clamp(1, MAX_NODES)
    }
    fn is_other(&self, i: usize) -> bool {
        // (another of the hub's players: LAN, hub server, Citra room)
        i > 0 && self.members[i].is_none() && slot(&self.hub, i)[N_CONNECTED] != 0
    }
    fn hub_ip(&self, i: usize) -> u32 {
        if i == 0 { rd32(&self.hub, NI_NODES + N_IP) } else { self.subnet | (i as u32 + 1) }
    }

    /// The hub with our members in it (for ldn.c).
    pub fn hub_view(&self) -> Vec<u8> {
        let mut info = self.hub.clone();
        let mut n = 1u8;
        for i in 1..MAX_NODES {
            if let Some(m) = &self.members[i] {
                let s = slot_mut(&mut info, i);
                s.copy_from_slice(&m.node);
                wr32(s, N_IP, self.subnet | (i as u32 + 1));
                s[N_ID] = i as u8;
                s[N_CONNECTED] = 1;
                n += 1;
            } else if self.is_other(i) {
                n += 1;
            } else {
                slot_mut(&mut info, i).fill(0);
            }
        }
        info[NI_NODE_COUNT] = n;
        info
    }

    /// The hub as the room's players see it.
    pub fn room_view(&self) -> Vec<u8> {
        let mut out = self.hub_view();
        let hm = host_mac(&self.own);
        out[NI_BSSID..NI_BSSID + 6].copy_from_slice(&hm);
        out[NI_SSID..NI_SSID + 0x22].fill(0);
        out[NI_SSID..NI_SSID + ROOM_SSID.len()].copy_from_slice(ROOM_SSID);
        for i in 0..MAX_NODES {
            let s = slot_mut(&mut out, i);
            if s[N_CONNECTED] == 0 {
                continue;
            }
            if i == 0 {
                wr32(s, N_IP, ip_u32(&self.own));
                s[N_MAC..N_MAC + 6].copy_from_slice(&hm);
            } else if let Some(m) = &self.members[i] {
                wr32(s, N_IP, ip_u32(&m.room_ip));
            } else {
                wr32(s, N_IP, alias_ip(i));
            }
        }
        out
    }

    /// hub-view <-> room-view swaps: IPs (both byte orders, as packets carry both) and the host's MAC
    fn subs(&self) -> Vec<(Vec<u8>, Vec<u8>)> {
        let mut s: Vec<(Vec<u8>, Vec<u8>)> = Vec::new();
        let mut ip = |hub: u32, room: u32| {
            if hub != room && hub != 0 && room != 0 {
                s.push((hub.to_be_bytes().to_vec(), room.to_be_bytes().to_vec()));
                s.push((hub.to_le_bytes().to_vec(), room.to_le_bytes().to_vec()));
            }
        };
        ip(self.hub_ip(0), ip_u32(&self.own));
        for i in 1..MAX_NODES {
            if let Some(m) = &self.members[i] {
                ip(self.hub_ip(i), ip_u32(&m.room_ip));
            } else if self.is_other(i) {
                ip(self.hub_ip(i), alias_ip(i));
            }
        }
        let hub_mac = slot(&self.hub, 0)[N_MAC..N_MAC + 6].to_vec();
        let hm = host_mac(&self.own).to_vec();
        if hub_mac != hm && hub_mac != [0; 6] {
            s.push((hub_mac, hm));
        }
        s
    }
    fn translate(&self, d: &mut [u8], to_room: bool) {
        let subs = self.subs();
        let mut i = 0;
        'outer: while i < d.len() {
            for (h, r) in &subs {
                let (from, to) = if to_room { (h, r) } else { (r, h) };
                if d.len() - i >= from.len() && d[i..i + from.len()] == from[..] {
                    d[i..i + to.len()].copy_from_slice(to);
                    i += from.len();
                    continue 'outer;
                }
            }
            i += 1;
        }
    }

    fn member_of(&self, room_ip: &[u8; 4]) -> Option<usize> {
        (1..MAX_NODES).find(|&i| self.members[i].as_ref().map(|m| &m.room_ip) == Some(room_ip))
    }

    fn sync_all(&self) -> Vec<DoorOut> {
        let v = self.room_view();
        self.members
            .iter()
            .flatten()
            .map(|m| DoorOut::Ldn { ty: RLDN_SYNC_NETWORK, to: m.room_ip, data: v.clone() })
            .collect()
    }
    /// What ldn.c is handed: the local player and our room players only.
    pub fn members_view(&self) -> Vec<u8> {
        let mut info = self.hub_view();
        let mut n = 1u8;
        for i in 1..MAX_NODES {
            if self.members[i].is_some() {
                n += 1;
            } else {
                slot_mut(&mut info, i).fill(0);
            }
        }
        info[NI_NODE_COUNT] = n;
        info
    }
    fn changed(&self) -> Vec<DoorOut> {
        let mut o = self.sync_all();
        o.push(DoorOut::Info(self.members_view()));
        o
    }

    /// ldn.c's hub changed (without our members). Our members hear of it; nothing goes back to ldn.c.
    pub fn update(&mut self, hub: &[u8]) -> Vec<DoorOut> {
        let before = self.room_view();
        self.hub = hub[..NETINFO].to_vec();
        self.subnet = rd32(hub, NI_NODES + N_IP) & 0xFFFF_FF00;
        if self.room_view() != before { self.sync_all() } else { Vec::new() }
    }

    pub fn stop(&mut self) -> Vec<DoorOut> {
        let out = self
            .members
            .iter()
            .flatten()
            .map(|m| DoorOut::Ldn { ty: RLDN_DESTROY_NETWORK, to: m.room_ip, data: Vec::new() })
            .collect();
        self.members = vec![None; MAX_NODES];
        out
    }

    fn remove(&mut self, i: usize, why: &str) -> Vec<DoorOut> {
        if let Some(m) = self.members[i].take() {
            let r = m.room_ip;
            self.log.push(format!("[ldn] Yuzu room player {}.{}.{}.{} left the hub ({why})", r[0], r[1], r[2], r[3]));
        }
        self.changed()
    }

    pub fn reject(&mut self, hub_ip: u32) -> Vec<DoorOut> {
        let i = (hub_ip & 0xFF) as usize;
        if hub_ip & 0xFFFF_FF00 != self.subnet || i < 2 || i > MAX_NODES || self.members[i - 1].is_none() {
            return Vec::new();
        }
        let to = self.members[i - 1].as_ref().unwrap().room_ip;
        let mut o = vec![DoorOut::Ldn { ty: RLDN_DESTROY_NETWORK, to, data: Vec::new() }];
        o.extend(self.remove(i - 1, "sent away"));
        o
    }

    pub fn members_changed(&mut self, ips: &[[u8; 4]]) -> Vec<DoorOut> {
        let gone: Vec<usize> = (1..MAX_NODES)
            .filter(|&i| self.members[i].as_ref().map(|m| !ips.contains(&m.room_ip)).unwrap_or(false))
            .collect();
        let mut o = Vec::new();
        for i in gone {
            o.extend(self.remove(i, "left the room"));
        }
        o
    }

    /// An LDN message from the room.
    pub fn ldn(&mut self, ty: u8, lip: [u8; 4], data: &[u8]) -> Vec<DoorOut> {
        match ty {
            RLDN_SCAN => vec![DoorOut::Ldn { ty: RLDN_SCAN_RESP, to: lip, data: self.room_view() }],
            RLDN_CONNECT if data.len() >= NODE => {
                if self.member_of(&lip).is_some() {
                    return self.sync_all(); // (asked again: it has it)
                }
                let lcv = u16::from_le_bytes([data[N_LCV], data[N_LCV + 1]]);
                let our_lcv = u16::from_le_bytes([self.hub[NI_NODES + N_LCV], self.hub[NI_NODES + N_LCV + 1]]);
                let free = (1..self.max()).find(|&i| self.members[i].is_none() && !self.is_other(i));
                let closed = self.hub[NI_ACCEPT_POLICY] == 1 || (lcv != 0 && our_lcv != 0 && lcv != our_lcv);
                let Some(i) = free.filter(|_| !closed) else {
                    return vec![DoorOut::Ldn { ty: RLDN_DESTROY_NETWORK, to: lip, data: Vec::new() }];
                };
                let mut node = [0u8; NODE];
                node.copy_from_slice(&data[..NODE]);
                node[0x0C + 0x20] = 0; // (the name ends)
                self.members[i] = Some(Member { room_ip: lip, node });
                self.log.push(format!("[ldn] Yuzu room player {}.{}.{}.{} joined the hub as node {i}", lip[0], lip[1], lip[2], lip[3]));
                self.changed()
            }
            RLDN_DISCONNECT => match self.member_of(&lip) {
                Some(i) => self.remove(i, "it left"),
                None => Vec::new(),
            },
            _ => Vec::new(),
        }
    }

    /// A game packet from the room (room view): for the hub (hub view), if a member of ours sent it.
    pub fn proxy(&self, sip: [u8; 4], sport: u16, dip: [u8; 4], dport: u16, bc: bool, data: &[u8]) -> Option<DoorOut> {
        let from = self.member_of(&sip)?;
        let d = ip_u32(&dip);
        let dst = if bc || d == 0xFFFF_FFFF || d & 0xFF == 0xFF {
            self.subnet | 0xFF
        } else if dip == self.own {
            self.hub_ip(0)
        } else if d & 0xFFFF_FF00 == 0xC0A8_FA00 && self.is_other((d & 0xFF) as usize) {
            self.hub_ip((d & 0xFF) as usize)
        } else {
            return None; // (to another room player: the room took it there)
        };
        let mut buf = data.to_vec();
        self.translate(&mut buf, false);
        Some(DoorOut::Packet { src: self.hub_ip(from), dst, sport, dport, data: buf })
    }

    /// A packet in the hub (hub view) from someone not in the room: to the room's players it's for.
    pub fn send(&self, src: u32, dst: u32, sport: u16, dport: u16, data: &[u8]) -> Vec<DoorOut> {
        let bc = dst == 0xFFFF_FFFF || dst == self.subnet | 0xFF;
        let to = if bc {
            if !self.members.iter().any(|m| m.is_some()) {
                return Vec::new();
            }
            [0xFF; 4]
        } else {
            let i = (dst & 0xFF) as usize;
            match self.members.get(i.wrapping_sub(1)).and_then(|m| m.as_ref()) {
                Some(m) if dst & 0xFFFF_FF00 == self.subnet => m.room_ip,
                _ => return Vec::new(),
            }
        };
        let i = (src & 0xFF) as usize;
        let src_room = if src == self.hub_ip(0) {
            self.own
        } else if src & 0xFFFF_FF00 == self.subnet && i >= 2 && i <= MAX_NODES && self.is_other(i - 1) {
            alias_ip(i - 1).to_be_bytes()
        } else {
            return Vec::new();
        };
        let mut buf = data.to_vec();
        self.translate(&mut buf, true);
        vec![DoorOut::Proxy { src: src_room, sport, dst: to, dport, bc, data: buf }]
    }
}

// ================= the driver and the C API (runtime/net/net.h)

pub type InfoCb = Option<extern "C" fn(*const u8)>;
pub type PacketCb = Option<extern "C" fn(u32, u32, u16, u16, *const u8, u32)>;

struct Driver {
    door: Option<Door>,
    info: InfoCb,
    packet: PacketCb,
}
static D: Mutex<Driver> = Mutex::new(Driver { door: None, info: None, packet: None });

fn run(outs: Vec<DoorOut>) {
    if outs.is_empty() {
        return;
    }
    let (info, packet, log) = {
        let mut d = D.lock().unwrap();
        let log = d.door.as_mut().map(|x| std::mem::take(&mut x.log)).unwrap_or_default();
        (d.info, d.packet, log)
    };
    for l in log {
        eprintln!("{l}");
    }
    let own = room::own_ip();
    for o in outs {
        match o {
            DoorOut::Ldn { ty, to, data } => room::send_ldn(ty, own, to, false, &data),
            DoorOut::Proxy { src, sport, dst, dport, bc, data } => room::send_proxy(src, sport, dst, dport, 4, bc, &data),
            DoorOut::Info(i) => {
                if let Some(f) = info {
                    f(i.as_ptr());
                }
            }
            DoorOut::Packet { src, dst, sport, dport, data } => {
                if let Some(f) = packet {
                    f(src, dst, sport, dport, data.as_ptr(), data.len() as u32);
                }
            }
        }
    }
}
fn with<R>(f: impl FnOnce(&mut Door) -> R) -> Option<R> {
    D.lock().unwrap().door.as_mut().map(f)
}
fn ip4(p: *const u8) -> [u8; 4] {
    if p.is_null() { [0; 4] } else { unsafe { [*p, *p.add(1), *p.add(2), *p.add(3)] } }
}
fn bytes<'a>(p: *const u8, n: u32) -> &'a [u8] {
    if p.is_null() { &[] } else { unsafe { std::slice::from_raw_parts(p, n as usize) } }
}

#[no_mangle]
pub extern "C" fn mhgu_ydoor_set_hooks(info: InfoCb, packet: PacketCb) {
    let mut d = D.lock().unwrap();
    d.info = info;
    d.packet = packet;
}
/// Opens the hub (ldn.c's, its players but no room ones) to the Yuzu room we're in. false: not in a room.
#[no_mangle]
pub extern "C" fn mhgu_ydoor_host(hub: *const u8) -> bool {
    if hub.is_null() || !room::connected() {
        return false;
    }
    let door = Door::new(bytes(hub, NETINFO as u32), room::own_ip());
    let old = {
        let mut d = D.lock().unwrap();
        d.door.replace(door)
    };
    if let Some(mut o) = old {
        run(o.stop());
    }
    true
}
/// The hub changed (without the room's players): calls nothing back.
#[no_mangle]
pub extern "C" fn mhgu_ydoor_update(hub: *const u8) {
    if hub.is_null() {
        return;
    }
    let o = with(|x| x.update(bytes(hub, NETINFO as u32))).unwrap_or_default();
    run(o);
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_stop() {
    let o = {
        let mut d = D.lock().unwrap();
        d.door.take().map(|mut x| x.stop()).unwrap_or_default()
    };
    run(o);
}
/// The room is gone: so are its players (no messages).
#[no_mangle]
pub extern "C" fn mhgu_ydoor_lost() {
    let had = D.lock().unwrap().door.take().is_some();
    let _ = had;
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_active() -> bool {
    D.lock().unwrap().door.is_some()
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_ldn(ty: u8, lip: *const u8, data: *const u8, len: u32) {
    let o = with(|x| x.ldn(ty, ip4(lip), bytes(data, len))).unwrap_or_default();
    run(o);
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_proxy(sip: *const u8, sport: u16, dip: *const u8, dport: u16, proto: u8, bc: bool, data: *const u8, len: u32) {
    if proto != 4 {
        return;
    }
    let o = with(|x| x.proxy(ip4(sip), sport, ip4(dip), dport, bc, bytes(data, len))).flatten();
    run(o.into_iter().collect());
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_members(ips: *const [u8; 4], n: i32) {
    if ips.is_null() || n <= 0 {
        return;
    }
    let list = unsafe { std::slice::from_raw_parts(ips, n as usize) }.to_vec();
    let o = with(|x| x.members_changed(&list)).unwrap_or_default();
    run(o);
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_reject(hub_ip: u32) {
    let o = with(|x| x.reject(hub_ip)).unwrap_or_default();
    run(o);
}
#[no_mangle]
pub extern "C" fn mhgu_ydoor_send(src: u32, dst: u32, sport: u16, dport: u16, data: *const u8, len: u32) {
    let o = with(|x| x.send(src, dst, sport, dport, bytes(data, len))).unwrap_or_default();
    run(o);
}

#[cfg(test)]
mod tests {
    use super::*;

    const OWN: [u8; 4] = [192, 168, 120, 1];
    const P1: [u8; 4] = [192, 168, 120, 7];
    const SUB: u32 = 0xA9FE_3300;

    fn hub_with_other() -> Vec<u8> {
        // the local player, and a LAN player in slot 1
        let mut h = vec![0u8; NETINFO];
        h[NI_NODE_MAX] = 4;
        h[NI_NODE_COUNT] = 2;
        let s = slot_mut(&mut h, 0);
        wr32(s, N_IP, SUB | 1);
        s[N_MAC..N_MAC + 6].copy_from_slice(&[2, 1, 1, 1, 1, 1]);
        s[N_CONNECTED] = 1;
        s[N_LCV] = 1;
        let s = slot_mut(&mut h, 1);
        wr32(s, N_IP, SUB | 2);
        s[N_ID] = 1;
        s[N_CONNECTED] = 1;
        h
    }
    fn node_of(ip: [u8; 4]) -> Vec<u8> {
        let mut n = vec![0u8; NODE];
        wr32(&mut n, N_IP, ip_u32(&ip));
        n[N_MAC..N_MAC + 6].copy_from_slice(&[2, 0, 7, 7, 7, 7]);
        n[N_CONNECTED] = 1;
        n[0x0C..0x0F].copy_from_slice(b"Eve");
        n[N_LCV] = 1;
        n
    }

    #[test]
    fn a_room_player_joins_the_hub() {
        let mut d = Door::new(&hub_with_other(), OWN);
        // the scan answer shows us at our room IP, the LAN player at its alias
        let o = d.ldn(RLDN_SCAN, P1, &[]);
        let DoorOut::Ldn { ty, data: v, .. } = &o[0] else { panic!() };
        assert_eq!(*ty, RLDN_SCAN_RESP);
        assert_eq!(rd32(v, NI_NODES + N_IP), ip_u32(&OWN));
        assert_eq!(rd32(v, NI_NODES + NODE + N_IP), alias_ip(1));
        assert_eq!(&v[NI_SSID..NI_SSID + ROOM_SSID.len()], ROOM_SSID);
        // it joins: slot 2 (1 is the LAN player's)
        let o = d.ldn(RLDN_CONNECT, P1, &node_of(P1));
        assert!(matches!(&o[0], DoorOut::Ldn { ty: RLDN_SYNC_NETWORK, to, .. } if *to == P1));
        let DoorOut::Info(h) = o.last().unwrap() else { panic!() };
        assert_eq!(h[NI_NODE_COUNT], 2); // (us and it: ldn.c keeps the LAN player)
        assert_eq!(rd32(h, NI_NODES + 2 * NODE + N_IP), SUB | 3); // its hub IP
        assert_eq!(d.hub_view()[NI_NODE_COUNT], 3);
        // its packet to the LAN player (alias) comes into the hub with hub addresses
        let mut pk = OWN.to_vec();
        pk.extend_from_slice(&P1);
        let o = d.proxy(P1, 12345, alias_ip(1).to_be_bytes(), 12345, false, &pk).unwrap();
        let mut want = (SUB | 1).to_be_bytes().to_vec();
        want.extend_from_slice(&(SUB | 3).to_be_bytes());
        assert_eq!(o, DoorOut::Packet { src: SUB | 3, dst: SUB | 2, sport: 12345, dport: 12345, data: want.clone() });
        // and the LAN player's answer goes out in the room's view
        let o = d.send(SUB | 2, SUB | 3, 12345, 12345, &want);
        assert_eq!(o, vec![DoorOut::Proxy { src: alias_ip(1).to_be_bytes(), sport: 12345, dst: P1, dport: 12345, bc: false, data: pk }]);
        // a packet between room players is the room's business
        assert!(d.proxy(P1, 1, [192, 168, 120, 9], 1, false, b"x").is_none());
        // it leaves the room
        let o = d.members_changed(&[OWN]);
        let DoorOut::Info(h) = o.last().unwrap() else { panic!() };
        assert_eq!(h[NI_NODE_COUNT], 1);
    }

    #[test]
    fn full_or_closed_hubs_send_them_away() {
        let mut h = hub_with_other();
        h[NI_NODE_MAX] = 2;
        let mut d = Door::new(&h, OWN);
        let o = d.ldn(RLDN_CONNECT, P1, &node_of(P1));
        assert_eq!(o, vec![DoorOut::Ldn { ty: RLDN_DESTROY_NETWORK, to: P1, data: Vec::new() }]);
        let mut h = hub_with_other();
        h[NI_ACCEPT_POLICY] = 1;
        let mut d = Door::new(&h, OWN);
        assert!(matches!(&d.ldn(RLDN_CONNECT, P1, &node_of(P1))[0], DoorOut::Ldn { ty: RLDN_DESTROY_NETWORK, .. }));
    }

    #[test]
    fn hub_changes_reach_the_room_players_only() {
        let mut d = Door::new(&hub_with_other(), OWN);
        d.ldn(RLDN_CONNECT, P1, &node_of(P1));
        let mut h = hub_with_other();
        slot_mut(&mut h, 1).fill(0);
        h[NI_NODE_COUNT] = 1;
        let o = d.update(&h);
        assert_eq!(o.len(), 1);
        assert!(matches!(&o[0], DoorOut::Ldn { ty: RLDN_SYNC_NETWORK, .. }));
        assert!(d.update(&h).is_empty());
    }
}
