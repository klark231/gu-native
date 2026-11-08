//! Crossplay with emulator networks: Ryujinx LDN servers (RyuLdn) and suyu-family rooms (room.rs) and Citra-family rooms (citra.rs, Azahar).
//! The C runtime calls the `extern "C"` wrappers in `ffi.rs`; the protocols live in Rust.

pub mod bridge;
pub mod citra;
pub mod ffi;
pub mod room;
pub mod roomsrv;
pub mod ryu;
pub mod ryu_server;
