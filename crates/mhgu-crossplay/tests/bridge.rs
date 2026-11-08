// A room player joining a hub the host runs on its own RyuLdn server (xbridge).
use mhgu_crossplay::{bridge, room, ryu, ryu_server::Server, roomsrv};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

static RX: AtomicU32 = AtomicU32::new(0);
static LAST: Mutex<Vec<u8>> = Mutex::new(Vec::new());

extern "C" fn on_data(_p: *const ryu::RyuProxy, d: *const u8, n: u32) {
    let data = unsafe { std::slice::from_raw_parts(d, n as usize) };
    if data.starts_with(b"hit") {
        RX.fetch_add(1, Ordering::SeqCst);
        *LAST.lock().unwrap() = data.to_vec();
    }
}

fn wait(f: impl Fn() -> bool, secs: u64) -> bool {
    let end = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < end {
        if f() { return true; }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
}

#[test]
fn room_player_joins_the_ryu_hub_and_its_packets_reach_the_host() {
    let srv = Server::start("127.0.0.1", 0).unwrap();
    let rport = srv.status().port;
    ryu::set_config(&format!("127.0.0.1:{rport}"), "", "");
    ryu::set_hooks(ryu::RyuHooks { lobby: None, data: Some(on_data) });
    let mut cfg = [0u8; 0x20];
    cfg[0x12] = 4;
    cfg[0x14..0x16].copy_from_slice(&1u16.to_le_bytes());
    let (host_ip, mask, info) = ryu::create(0, &cfg, &[0u8; 0x44], &[0u8; 0x30], b"hub").expect("create");

    let room_port = 24800 + (std::process::id() % 400) as u16;
    roomsrv::start(room_port, "MHGU", "", "host").expect("room server");
    room::join(&format!("127.0.0.1:{room_port}"), "host", "").expect("room join");

    bridge::begin(&info, host_ip, mask);
    let peer = [192u8, 168, 1, 50];
    let mut node = [0u8; 0x40];
    node[0x0C..0x12].copy_from_slice(b"guest\0");
    node[0x2E..0x30].copy_from_slice(&1u16.to_le_bytes());
    bridge::room_ldn(2, &peer, &peer, false, &node); // RLDN_CONNECT
    assert!(wait(|| srv.status().players == 2 && bridge::players() == 1, 5), "the room player joined the hub");

    let room_host = room::own_ip();
    bridge::room_proxy(&peer, 5000, &room_host, 5001, 4, false, b"hit-bridge");
    assert!(wait(|| RX.load(Ordering::SeqCst) > 0, 5), "the host got the room player's packet");
    assert_eq!(LAST.lock().unwrap().as_slice(), b"hit-bridge");

    bridge::end();
    room::leave();
    roomsrv::stop();
    ryu::leave();
}
