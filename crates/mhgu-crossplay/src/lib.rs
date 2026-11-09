//! Crossplay with emulator networks: Ryujinx LDN servers (RyuLdn), suyu-family rooms (room.rs), and Citra-family rooms
//! (citra.rs, Azahar), where citra_bridge.rs plays one 3DS so Monster Hunter XX players can join (uds.rs, pia.rs).
//! The C runtime calls the `extern "C"` wrappers in `ffi.rs`; the protocols live in Rust.

pub mod bridge;
pub mod citra;
pub mod citra_bridge;
pub mod crypto;
pub mod ffi;
pub mod pia;
pub mod room;
pub mod roomsrv;
pub mod ryu;
pub mod ryu_server;
pub mod uds;
pub mod yuzu_door;
