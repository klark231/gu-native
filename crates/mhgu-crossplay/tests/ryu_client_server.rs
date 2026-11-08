use mhgu_crossplay::{ryu, ryu_server::Server};

#[test]
fn game_client_creates_lobby_on_the_rust_server() {
    let srv = Server::start("127.0.0.1", 0).unwrap();
    let port = srv.status().port;
    ryu::set_config(&format!("127.0.0.1:{port}"), "", "");
    let mut cfg = [0u8; 0x20];
    cfg[0x12] = 4;
    cfg[0x14..0x16].copy_from_slice(&1u16.to_le_bytes());
    let sec = [0u8; 0x44];
    let mut user = [0u8; 0x30];
    user[..5].copy_from_slice(b"host\0");
    let adv = b"advertise";
    let (ip, mask, info) = ryu::create(0, &cfg, &sec, &user, adv).expect("create");
    assert_eq!(ip >> 16, 0x0A72);
    assert_eq!(mask, 0xFFFF0000);
    assert_eq!(info[0x67], 1);
    assert_eq!(srv.status().lobbies, 1);
    assert_eq!(srv.status().players, 1);
    ryu::leave();
}
