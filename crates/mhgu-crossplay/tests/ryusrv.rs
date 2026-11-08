use mhgu_crossplay::ryu_server::{msg, Server};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

fn frame(ty: u8, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    v.extend_from_slice(b"RLDN");
    v.push(ty);
    v.push(1);
    v.extend_from_slice(&[0, 0]);
    v.extend_from_slice(&(payload.len() as i32).to_le_bytes());
    v.extend_from_slice(payload);
    v
}

fn recv(s: &mut TcpStream) -> (u8, Vec<u8>) {
    s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let mut h = [0u8; 12];
    s.read_exact(&mut h).unwrap();
    assert_eq!(&h[..4], b"RLDN");
    let n = i32::from_le_bytes(h[8..12].try_into().unwrap()) as usize;
    let mut d = vec![0u8; n];
    s.read_exact(&mut d).unwrap();
    (h[4], d)
}
fn send(s: &mut TcpStream, ty: u8, p: &[u8]) {
    s.write_all(&frame(ty, p)).unwrap();
}
fn until(s: &mut TcpStream, ty: u8) -> Vec<u8> {
    for _ in 0..20 {
        let (t, d) = recv(s);
        if t == ty {
            return d;
        }
    }
    panic!("packet {ty} not received");
}
fn connect(port: u16) -> TcpStream {
    TcpStream::connect(("127.0.0.1", port)).unwrap()
}
fn join(port: u16, name: &[u8]) -> (TcpStream, u32, [u8; 0x480]) {
    let mut s = connect(port);
    send(&mut s, msg::INITIALIZE, &[0u8; 22]);
    until(&mut s, msg::INITIALIZE);
    send(&mut s, msg::PASSPHRASE, &[0u8; 0x80]);
    let mut create = vec![0u8; 0xBC];
    create[0x74..0x7C].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
    create[0x74 + 0x12] = 8;
    create[0x74 + 0x14..0x74 + 0x16].copy_from_slice(&1u16.to_le_bytes());
    create[0x44..0x44 + name.len()].copy_from_slice(name);
    create[0x94..0x94 + 6].copy_from_slice(b"1.4.0\0");
    send(&mut s, msg::CREATE, &create);
    let ip = until_joined(&mut s);
    let info = scan(port);
    (s, ip, info)
}
fn until_joined(s: &mut TcpStream) -> u32 {
    let mut ip = 0;
    for _ in 0..20 {
        let (t, d) = recv(s);
        if t == msg::PROXY_CONFIG {
            ip = u32::from_le_bytes(d[..4].try_into().unwrap());
        }
        if t == msg::CONNECTED {
            return ip;
        }
    }
    panic!("not connected");
}
fn scan(port: u16) -> [u8; 0x480] {
    let mut s = connect(port);
    send(&mut s, msg::INITIALIZE, &[0u8; 22]);
    until(&mut s, msg::INITIALIZE);
    send(&mut s, msg::PASSPHRASE, &[0u8; 0x80]);
    send(&mut s, msg::SCAN, &[0u8; 0x60]);
    let (t, d) = recv(&mut s);
    assert_eq!(t, msg::SCAN_REPLY);
    let (t, _) = recv(&mut s);
    assert_eq!(t, msg::SCAN_REPLY_END);
    d.try_into().unwrap()
}
fn join_info(port: u16, info: &[u8; 0x480], name: &[u8]) -> (TcpStream, u32) {
    let mut s = connect(port);
    send(&mut s, msg::INITIALIZE, &[0u8; 22]);
    until(&mut s, msg::INITIALIZE);
    send(&mut s, msg::PASSPHRASE, &[0u8; 0x80]);
    let mut c = vec![0u8; 0x500];
    c[0x44..0x44 + name.len()].copy_from_slice(name);
    c[0x74..0x78].copy_from_slice(&1u32.to_le_bytes());
    c[0x80..0x80 + 0x480].copy_from_slice(info);
    send(&mut s, msg::CONNECT, &c);
    let ip = until_joined(&mut s);
    (s, ip)
}

#[test]
fn lobby_scan_join_and_relay() {
    let srv = Server::start("127.0.0.1", 0).unwrap();
    let port = srv.status().port;
    let (mut a, a_ip, info) = join(port, b"host");
    let (mut b, b_ip) = join_info(port, &info, b"guest");
    assert_ne!(a_ip, b_ip);

    let p = [0xAA, 0xBB, 0xCC, 0xDD];
    let mut pkt = Vec::new();
    pkt.extend_from_slice(&a_ip.to_le_bytes());
    pkt.extend_from_slice(&[0x11, 0x22]);
    pkt.extend_from_slice(&b_ip.to_le_bytes());
    pkt.extend_from_slice(&[0x33, 0x44]);
    pkt.extend_from_slice(&17u32.to_le_bytes());
    pkt.extend_from_slice(&(p.len() as u32).to_le_bytes());
    pkt.extend_from_slice(&p);
    send(&mut a, msg::PROXY_DATA, &pkt);
    let (t, d) = recv(&mut b);
    assert_eq!(t, msg::PROXY_DATA);
    assert_eq!(&d[0x14..], &p);
    assert_eq!(u32::from_le_bytes(d[6..10].try_into().unwrap()), b_ip);

    assert_eq!(srv.status().lobbies, 1);
    assert_eq!(srv.status().players, 2);
}
