//! Crossplay with emulator networks: Ryujinx LDN servers (RyuLdn) and suyu-family rooms.
//! The C runtime calls the `extern "C"` wrappers in `ffi.rs`; the protocols live in Rust.

pub mod bridge;
pub mod ffi;
pub mod room;
pub mod roomsrv;
pub mod ryu;
pub mod ryu_server;
