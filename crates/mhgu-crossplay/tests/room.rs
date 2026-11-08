// The suyu room client and server together: A joins and broadcasts, B joins, answers.
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use mhgu_crossplay::{room, roomsrv};

static PING: AtomicBool = AtomicBool::new(false);
static PONG: AtomicBool = AtomicBool::new(false);
static MEMBERS: AtomicUsize = AtomicUsize::new(0);
static LAST: Mutex<Vec<u8>> = Mutex::new(Vec::new());

extern "C" fn on_ldn(_ty: u8, _lip: *const u8, rip: *const u8, _bc: bool, d: *const u8, n: u32) {
    let data = unsafe { std::slice::from_raw_parts(d, n as usize) };
    if data == b"ping" {
        PING.store(true, Ordering::SeqCst);
        let own = room::own_ip();
        let r = unsafe { [*_lip, *_lip.add(1), *_lip.add(2), *_lip.add(3)] };
        room::send_ldn(1, own, r, false, b"pong");
    } else if data == b"pong" {
        PONG.store(true, Ordering::SeqCst);
        *LAST.lock().unwrap() = data.to_vec();
    }
}
extern "C" fn on_members(_ips: *const [u8; 4], n: i32) { MEMBERS.store(n as usize, Ordering::SeqCst); }
fn wait(f: impl Fn() -> bool, secs: u64) -> bool {
    let end = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < end { if f() { return true; } std::thread::sleep(Duration::from_millis(20)); }
    false
}

#[test]
fn room_roundtrip() {
    let port = 24900 + (std::process::id() % 400) as u16;
    match std::env::var("ROOM_ROLE").as_deref() {
        Ok("b") => { role_b(port); return; }
        Ok("crash") => {
            let port: u16 = std::env::var("ROOM_PORT").unwrap().parse().unwrap();
            room::join(&format!("127.0.0.1:{port}"), "b", "").expect("b joined");
            std::process::exit(0);
        }
        _ => {}
    }
    roomsrv::start(port, "MHGU", "", "MHGU Host").expect("room server");
    room::set_hooks(room::RoomHooks { ldn: Some(on_ldn), members: Some(on_members), ..Default::default() });
    let ip = room::join(&format!("127.0.0.1:{port}"), "a", "").expect("a joined");
    assert_eq!(MEMBERS.load(Ordering::SeqCst), 1);
    let mut child = std::process::Command::new(std::env::current_exe().unwrap())
        .env("ROOM_ROLE", "b").env("ROOM_PORT", port.to_string())
        .arg("--exact").arg("room_roundtrip").arg("--nocapture")
        .spawn().expect("spawn b");
    assert!(wait(|| MEMBERS.load(Ordering::SeqCst) >= 2, 10), "b joined");
    room::send_ldn(1, ip, [10, 255, 1, 255], true, b"ping");
    assert!(wait(|| PONG.load(Ordering::SeqCst), 10), "pong from b");
    assert_eq!(LAST.lock().unwrap().as_slice(), b"pong");
    room::leave();
    let _ = child.wait();

    let mut crashed = std::process::Command::new(std::env::current_exe().unwrap())
        .env("ROOM_ROLE", "crash").env("ROOM_PORT", port.to_string())
        .arg("--exact").arg("room_roundtrip").arg("--nocapture")
        .spawn().expect("spawn crashed client");
    assert!(wait(|| MEMBERS.load(Ordering::SeqCst) >= 2, 10), "crashed client joined");
    assert_eq!(crashed.wait().unwrap().code(), Some(0));
    let deadline = Instant::now() + Duration::from_secs(22);
    let mut rejoined = false;
    while Instant::now() < deadline {
        if room::join(&format!("127.0.0.1:{port}"), "b", "").is_ok() {
            rejoined = true;
            break;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
    assert!(rejoined, "same nickname can rejoin after its peer times out");
    room::leave();
    roomsrv::stop();
}

fn role_b(port: u16) {
    let port: u16 = std::env::var("ROOM_PORT").unwrap().parse().unwrap();
    room::set_hooks(room::RoomHooks { ldn: Some(on_ldn), members: Some(on_members), ..Default::default() });
    room::join(&format!("127.0.0.1:{port}"), "b", "").expect("b joined");
    assert!(wait(|| PING.load(Ordering::SeqCst), 15), "ping from a");
    std::thread::sleep(Duration::from_millis(200));
    room::leave();
}
