//! The C API (crates/mhgu-crossplay/include/mhgu_crossplay.h), mirroring runtime/net/ryuldn.h.
use crate::ryu::{self, RyuHooks, RyuPeerHooks, RyuProxy};
use core::ffi::{c_char, c_void};
use std::ffi::CStr;

fn cstr(p: *const c_char) -> String {
    if p.is_null() { return String::new(); }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_set_config(servers: *const c_char, host: *const c_char, pass: *const c_char) {
    ryu::set_config(&cstr(servers), &cstr(host), &cstr(pass));
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_set_self(addr: *const c_char) {
    let s = cstr(addr);
    ryu::set_self(if s.is_empty() { None } else { Some(&s) });
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_set_hooks(lobby: Option<extern "C" fn(*const u8)>, data: Option<extern "C" fn(*const RyuProxy, *const u8, u32)>) {
    ryu::set_hooks(RyuHooks { lobby, data });
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_server_count() -> i32 { ryu::server_count() as i32 }

#[no_mangle]
pub extern "C" fn mhgu_ryu_enabled() -> bool { ryu::enabled() }

#[no_mangle]
pub extern "C" fn mhgu_ryu_scan(filter: *const u8, infos: *mut u8, server_of: *mut i32, max: i32) -> i32 {
    if filter.is_null() || infos.is_null() || server_of.is_null() || max <= 0 { return 0; }
    let f = unsafe { std::slice::from_raw_parts(filter, 0x60) };
    let got = ryu::scan(f);
    let n = got.len().min(max as usize);
    for (i, (info, srv)) in got.iter().take(n).enumerate() {
        unsafe {
            std::ptr::copy_nonoverlapping(info.as_ptr(), infos.add(i * ryu::RYU_NETINFO), ryu::RYU_NETINFO.min(info.len()));
            *server_of.add(i) = *srv as i32;
        }
    }
    n as i32
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_create(
    server: i32, net_cfg: *const u8, sec_cfg: *const u8, user: *const u8, adv: *const u8, adv_size: u32,
    info: *mut u8, ip: *mut u32, mask: *mut u32,
) -> u32 {
    if net_cfg.is_null() || sec_cfg.is_null() || user.is_null() || info.is_null() || ip.is_null() || mask.is_null() { return ryu::R_CONNECT_FAILURE; }
    let cfg = unsafe { std::slice::from_raw_parts(net_cfg, 0x20) };
    let sec = unsafe { std::slice::from_raw_parts(sec_cfg, 0x44) };
    let usr = unsafe { std::slice::from_raw_parts(user, 0x30) };
    let adv = if adv.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(adv, adv_size as usize) } };
    match ryu::create(server, cfg, sec, usr, adv) {
        Ok((i, m, blob)) => {
            unsafe {
                std::ptr::copy_nonoverlapping(blob.as_ptr(), info, blob.len());
                *ip = i; *mask = m;
            }
            0
        }
        Err(r) => r,
    }
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_connect(
    server: i32, netinfo: *const u8, sec_cfg: *const u8, user: *const u8, lcv: u16,
    info_out: *mut u8, ip: *mut u32, mask: *mut u32,
) -> u32 {
    if netinfo.is_null() || sec_cfg.is_null() || user.is_null() || info_out.is_null() || ip.is_null() || mask.is_null() { return ryu::R_CONNECT_FAILURE; }
    let ni = unsafe { std::slice::from_raw_parts(netinfo, ryu::RYU_NETINFO) };
    let sec = unsafe { std::slice::from_raw_parts(sec_cfg, 0x44) };
    let usr = unsafe { std::slice::from_raw_parts(user, 0x30) };
    match ryu::connect(server, ni, sec, usr, lcv) {
        Ok((i, m, blob)) => {
            unsafe {
                std::ptr::copy_nonoverlapping(blob.as_ptr(), info_out, blob.len());
                *ip = i; *mask = m;
            }
            0
        }
        Err(r) => r,
    }
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_leave() { ryu::leave() }

#[no_mangle]
pub extern "C" fn mhgu_ryu_in_lobby() -> bool { ryu::in_lobby() }

#[no_mangle]
pub extern "C" fn mhgu_ryu_set_advertise(data: *const u8, len: u32) {
    if data.is_null() { return; }
    ryu::set_advertise(unsafe { std::slice::from_raw_parts(data, len as usize) });
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_set_accept_policy(policy: u8) { ryu::set_accept_policy(policy) }

#[no_mangle]
pub extern "C" fn mhgu_ryu_reject(node_id: u32, reason: u32) -> bool { ryu::reject(node_id, reason) }

#[no_mangle]
pub extern "C" fn mhgu_ryu_send(p: *const RyuProxy, data: *const u8, len: u32) {
    if p.is_null() { return; }
    let d = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    ryu::send(unsafe { &*p }, d);
}

static PEER_STORE: std::sync::Mutex<Vec<usize>> = std::sync::Mutex::new(Vec::new());

#[no_mangle]
pub extern "C" fn mhgu_ryu_peer_join(
    netinfo: *const u8, sec_cfg: *const u8, user: *const u8, lcv: u16,
    lobby: Option<extern "C" fn(*mut c_void, *const u8)>,
    data: Option<extern "C" fn(*mut c_void, *const RyuProxy, *const u8, u32)>,
    ud: *mut c_void, info_out: *mut u8, ip: *mut u32, mask: *mut u32, result: *mut u32,
) -> *mut c_void {
    if netinfo.is_null() || sec_cfg.is_null() || user.is_null() { unsafe { *result = ryu::R_CONNECT_FAILURE; } return std::ptr::null_mut(); }
    let ni = unsafe { std::slice::from_raw_parts(netinfo, ryu::RYU_NETINFO) };
    let sec = unsafe { std::slice::from_raw_parts(sec_cfg, 0x44) };
    let usr = unsafe { std::slice::from_raw_parts(user, 0x30) };
    match ryu::peer_join(ni, sec, usr, lcv, RyuPeerHooks { lobby, data }, ud) {
        Ok((peer, pip, pmask, info)) => {
            let boxed = Box::new(peer);
            let ptr = Box::into_raw(boxed) as *mut c_void;
            PEER_STORE.lock().unwrap().push(ptr as usize);
            unsafe {
                *result = 0;
                if !ip.is_null() { *ip = pip; }
                if !mask.is_null() { *mask = pmask; }
                if !info_out.is_null() { std::ptr::copy_nonoverlapping(info.as_ptr(), info_out, info.len()); }
            }
            ptr
        }
        Err(r) => { unsafe { *result = r; } std::ptr::null_mut() }
    }
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_peer_send(peer: *mut c_void, p: *const RyuProxy, data: *const u8, len: u32) {
    if peer.is_null() || p.is_null() { return; }
    let d = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    // SAFETY: the pointer came from mhgu_ryu_peer_join and is alive until mhgu_ryu_peer_close.
    let rp = unsafe { &*(peer as *const ryu::RyuPeer) };
    ryu::peer_send(rp, unsafe { &*p }, d);
}

#[no_mangle]
pub extern "C" fn mhgu_ryu_peer_close(peer: *mut c_void) {
    if peer.is_null() { return; }
    PEER_STORE.lock().unwrap().retain(|p| *p != peer as usize);
    let boxed = unsafe { Box::from_raw(peer as *mut ryu::RyuPeer) };
    ryu::peer_close(&boxed);
}

// ---- suyu-family rooms (runtime/net/room.h)
use crate::room;
use crate::roomsrv;

pub type RoomLdnCb = Option<extern "C" fn(u8, *const u8, *const u8, bool, *const u8, u32)>;
pub type RoomProxyCb = Option<extern "C" fn(*const u8, u16, *const u8, u16, u8, bool, *const u8, u32)>;
pub type RoomLostCb = Option<extern "C" fn()>;
pub type RoomMembersCb = Option<extern "C" fn(*const [u8; 4], i32)>;

#[no_mangle]
pub extern "C" fn mhgu_room_set_hooks(ldn: RoomLdnCb, proxy: RoomProxyCb, lost: RoomLostCb, members: RoomMembersCb) {
    room::set_hooks(room::RoomHooks { ldn, proxy, lost, members });
}

#[no_mangle]
pub extern "C" fn mhgu_room_join(
    server: *const c_char, nick: *const c_char, password: *const c_char, ip: *mut u8,
    err: *mut c_char, err_size: i32,
) -> bool {
    match room::join(&cstr(server), &cstr(nick), &cstr(password)) {
        Ok(a) => {
            if !ip.is_null() { unsafe { std::ptr::copy_nonoverlapping(a.as_ptr(), ip, 4); } }
            true
        }
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
pub extern "C" fn mhgu_room_leave() { room::leave() }

#[no_mangle]
pub extern "C" fn mhgu_room_connected() -> bool { room::connected() }

#[no_mangle]
pub extern "C" fn mhgu_room_own_ip(ip: *mut u8) {
    if ip.is_null() { return; }
    unsafe { std::ptr::copy_nonoverlapping(room::own_ip().as_ptr(), ip, 4); }
}

#[no_mangle]
pub extern "C" fn mhgu_room_send_ldn(ty: u8, lip: *const u8, rip: *const u8, bc: bool, data: *const u8, len: u32) {
    if lip.is_null() || rip.is_null() { return; }
    let l = unsafe { [*lip, *lip.add(1), *lip.add(2), *lip.add(3)] };
    let r = unsafe { [*rip, *rip.add(1), *rip.add(2), *rip.add(3)] };
    let d = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    room::send_ldn(ty, l, r, bc, d);
}

#[no_mangle]
pub extern "C" fn mhgu_room_send_proxy(
    sip: *const u8, sport: u16, dip: *const u8, dport: u16, proto: u8, bc: bool, data: *const u8, len: u32,
) {
    if sip.is_null() || dip.is_null() { return; }
    let s = unsafe { [*sip, *sip.add(1), *sip.add(2), *sip.add(3)] };
    let d = unsafe { [*dip, *dip.add(1), *dip.add(2), *dip.add(3)] };
    let b = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    room::send_proxy(s, sport, d, dport, proto, bc, b);
}

#[no_mangle]
pub extern "C" fn mhgu_roomsrv_start(
    port: i32, name: *const c_char, password: *const c_char, host_nick: *const c_char,
    err: *mut c_char, err_size: i32,
) -> bool {
    match roomsrv::start(port.max(0) as u16, &cstr(name), &cstr(password), &cstr(host_nick)) {
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
pub extern "C" fn mhgu_roomsrv_stop() { roomsrv::stop() }

#[no_mangle]
pub extern "C" fn mhgu_roomsrv_running() -> bool { roomsrv::running() }

// ---- mixed hubs (runtime/net/xbridge.h)
use crate::bridge;

#[no_mangle]
pub extern "C" fn mhgu_xb_begin(info: *const u8, host_ip: u32, mask: u32) {
    if info.is_null() { return; }
    bridge::begin(unsafe { std::slice::from_raw_parts(info, 0x480) }, host_ip, mask);
}
#[no_mangle]
pub extern "C" fn mhgu_xb_end() { bridge::end() }
#[no_mangle]
pub extern "C" fn mhgu_xb_active() -> bool { bridge::active() }
#[no_mangle]
pub extern "C" fn mhgu_xb_players() -> i32 { bridge::players() }
#[no_mangle]
pub extern "C" fn mhgu_xb_lobby(info: *const u8) {
    if info.is_null() { bridge::lobby(None) } else { bridge::lobby(Some(unsafe { std::slice::from_raw_parts(info, 0x480) })) }
}
#[no_mangle]
pub extern "C" fn mhgu_xb_room_ldn(ty: u8, lip: *const u8, rip: *const u8, bc: bool, data: *const u8, len: u32) {
    if lip.is_null() || rip.is_null() { return; }
    let l = unsafe { [*lip, *lip.add(1), *lip.add(2), *lip.add(3)] };
    let r = unsafe { [*rip, *rip.add(1), *rip.add(2), *rip.add(3)] };
    let d = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    bridge::room_ldn(ty, &l, &r, bc, d);
}
#[no_mangle]
pub extern "C" fn mhgu_xb_room_members(ips: *const [u8; 4], n: i32) {
    if ips.is_null() || n <= 0 { bridge::room_members(&[]); return; }
    let v = unsafe { std::slice::from_raw_parts(ips, n as usize) };
    bridge::room_members(v);
}
#[no_mangle]
pub extern "C" fn mhgu_xb_room_proxy(
    sip: *const u8, sport: u16, dip: *const u8, dport: u16, proto: u8, bc: bool, data: *const u8, len: u32,
) {
    if sip.is_null() || dip.is_null() { return; }
    let s = unsafe { [*sip, *sip.add(1), *sip.add(2), *sip.add(3)] };
    let d = unsafe { [*dip, *dip.add(1), *dip.add(2), *dip.add(3)] };
    let b = if data.is_null() { &[][..] } else { unsafe { std::slice::from_raw_parts(data, len as usize) } };
    bridge::room_proxy(&s, sport, &d, dport, proto, bc, b);
}

// ---- RyuLdn server (runtime/net/ryusrv): one per process, the host's embedded server.
use crate::ryu_server;

#[repr(C)]
pub struct RyuServerStatusC {
    pub running: bool,
    pub port: u16,
    pub connections: u32,
    pub lobbies: u32,
    pub players: u32,
}

static RYU_SRV: std::sync::LazyLock<std::sync::Mutex<Option<ryu_server::Server>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(None));

#[no_mangle]
pub extern "C" fn mhgu_ryusrv_start(addr: *const c_char, port: u16, err: *mut c_char, err_size: i32) -> bool {
    let mut g = RYU_SRV.lock().unwrap();
    if g.is_some() { return true; }
    match ryu_server::Server::start(&cstr(addr), port) {
        Ok(s) => { *g = Some(s); true }
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
pub extern "C" fn mhgu_ryusrv_stop() {
    if let Some(mut s) = RYU_SRV.lock().unwrap().take() { s.stop(); }
}
#[no_mangle]
pub extern "C" fn mhgu_ryusrv_running() -> bool { RYU_SRV.lock().unwrap().is_some() }
#[no_mangle]
pub extern "C" fn mhgu_ryusrv_status(out: *mut RyuServerStatusC) -> bool {
    let g = RYU_SRV.lock().unwrap();
    let Some(s) = g.as_ref() else { return false };
    let st = s.status();
    if !out.is_null() {
        unsafe { *out = RyuServerStatusC {
            running: st.running, port: st.port, connections: st.connections as u32,
            lobbies: st.lobbies as u32, players: st.players as u32,
        }; }
    }
    true
}
