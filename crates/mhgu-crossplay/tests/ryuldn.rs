// RyuLdn client against tests/ryuldn_stub_server.py: a host creates a lobby, a joiner scans,
// joins and exchanges a packet (the upstream test_ryuldn.c, in two processes).
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use mhgu_crossplay::ryu::{self, RyuHooks, RyuProxy};

static GOT_DATA: AtomicUsize = AtomicUsize::new(0);
static GOT_LOBBY: AtomicUsize = AtomicUsize::new(0);
static SRC: AtomicU32 = AtomicU32::new(0);
static DST: AtomicU32 = AtomicU32::new(0);
static MSG: Mutex<Vec<u8>> = Mutex::new(Vec::new());

extern "C" fn on_lobby(_info: *const u8) { GOT_LOBBY.fetch_add(1, Ordering::SeqCst); }
extern "C" fn on_data(p: *const RyuProxy, d: *const u8, n: u32) {
    let p = unsafe { &*p };
    SRC.store(p.src_ip, Ordering::SeqCst);
    DST.store(p.dst_ip, Ordering::SeqCst);
    *MSG.lock().unwrap() = unsafe { std::slice::from_raw_parts(d, n as usize) }.to_vec();
    GOT_DATA.fetch_add(1, Ordering::SeqCst);
}
fn wait_data(secs: u64) -> bool {
    let end = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < end {
        if GOT_DATA.load(Ordering::SeqCst) > 0 { return true; }
        std::thread::sleep(Duration::from_millis(10));
    }
    false
}
fn check(c: bool, what: &str) -> bool { if !c { eprintln!("[test] FAIL: {what}"); } else { println!("[test] ok: {what}"); } c }

#[test]
fn host_and_join_against_stub_server() {
    if std::env::var("RYU_ROLE").as_deref() == Ok("host") { run_host(); return; }
    let port = 30500 + (std::process::id() % 200) as u16;
    let stub = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/ryuldn_stub_server.py");
    let python = if cfg!(windows) { "python" } else { "python3" };
    let mut srv = std::process::Command::new(python).arg(stub).arg(port.to_string())
        .stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).spawn().expect("stub server");
    std::thread::sleep(Duration::from_millis(800));
    let addr = format!("127.0.0.1:{port}");
    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .env("RYU_ROLE", "host").env("RYU_ADDR", &addr)
        .arg("--exact").arg("host_and_join_against_stub_server").arg("--nocapture")
        .spawn().expect("spawn host");
    std::thread::sleep(Duration::from_millis(1200));

    ryu::set_config(&addr, &addr, "");
    ryu::set_hooks(RyuHooks { lobby: Some(on_lobby), data: Some(on_data) });
    assert!(check(ryu::server_count() == 1 && ryu::enabled(), "server configured"));
    let mut infos = Vec::new();
    for _ in 0..40 {
        let found = ryu::scan(&[0u8; 0x60]);
        if !found.is_empty() { infos = found; break; }
        std::thread::sleep(Duration::from_millis(200));
    }
    assert!(check(!infos.is_empty(), "scan found the lobby"));
    assert!(check(infos[0].1 == 0 && infos[0].0[0x26C..0x270] == *b"ADV!", "scan result has the advertise data"));
    let sec = [0u8; 0x44];
    let mut user = [0u8; 0x30];
    user[..8].copy_from_slice(b"TestJoin");
    let (ip, _mask, info) = ryu::connect(infos[0].1 as i32, &infos[0].0, &sec, &user, 1).expect("joined");
    assert!(check((0x0A72_0002..0x0A72_0009).contains(&ip), "joiner address 10.114.0.x"));
    assert!(check(info[0x67] >= 2, "NetworkInfo: 2 or more players"));
    let p = RyuProxy { src_ip: ip, dst_ip: 0x0A72_0001, src_port: 12345, dst_port: 12345, proto: 17 };
    ryu::send(&p, b"ping");
    assert!(check(wait_data(10), "answer from the host"));
    assert!(check(MSG.lock().unwrap().as_slice() == b"pong" && SRC.load(Ordering::SeqCst) == 0x0A72_0001, "answer payload"));
    ryu::leave();
    assert!(check(!ryu::in_lobby(), "left the lobby"));
    let _ = child.kill(); let _ = child.wait(); let _ = srv.kill(); let _ = srv.wait();
}

fn run_host() {
    let addr = std::env::var("RYU_ADDR").unwrap();
    ryu::set_config(&addr, &addr, "");
    ryu::set_hooks(RyuHooks { lobby: Some(on_lobby), data: Some(on_data) });
    let sec = [0u8; 0x44];
    let mut user = [0u8; 0x30];
    user[..8].copy_from_slice(b"TestHost");
    let mut cfg = [0u8; 0x20];
    cfg[0x12] = 4;
    cfg[0x14] = 1;
    cfg[..8].copy_from_slice(&[0x00, 0x80, 0xDD, 0x08, 0x00, 0x77, 0x00, 0x01]);
    let r = ryu::create(-1, &cfg, &sec, &user, b"ADV!").expect("lobby created");
    assert!(check(r.0 == 0x0A72_0001 && r.1 == 0xFFFF_0000, "host address 10.114.0.1/16"));
    assert!(check(r.2[0x67] == 1 && r.2[0x66] == 4, "NetworkInfo: 1 of 4 players"));
    assert!(check(&r.2[0x26C..0x270] == b"ADV!", "advertise data in the NetworkInfo"));
    assert!(check(wait_data(20), "packet from the joiner"));
    assert!(check(DST.load(Ordering::SeqCst) == 0x0A72_0001 && SRC.load(Ordering::SeqCst) == 0x0A72_0002, "packet addresses"));
    assert!(check(MSG.lock().unwrap().as_slice() == b"ping", "packet payload"));
    let back = RyuProxy { src_ip: r.0, dst_ip: SRC.load(Ordering::SeqCst), src_port: 12345, dst_port: 12345, proto: 17 };
    ryu::send(&back, b"pong");
    std::thread::sleep(Duration::from_millis(500));
    assert!(check(GOT_LOBBY.load(Ordering::SeqCst) >= 1, "lobby update after the join"));
    ryu::leave();
}
