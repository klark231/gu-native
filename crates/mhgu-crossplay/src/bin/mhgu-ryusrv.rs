// Standalone RyuLdn master server (the one Ryujinx/suyu LDN clients connect to).
// Usage: mhgu-ryusrv [-addr 0.0.0.0] [-port 30456]
use mhgu_crossplay::ryu_server::{Server, DEFAULT_PORT};

fn arg(name: &str, default: &str) -> String {
    let a: Vec<String> = std::env::args().collect();
    a.iter().position(|s| s == name).and_then(|i| a.get(i + 1)).cloned().unwrap_or_else(|| default.into())
}

fn main() {
    let addr = arg("-addr", "0.0.0.0");
    let port: u16 = arg("-port", &DEFAULT_PORT.to_string()).parse().unwrap_or(DEFAULT_PORT);
    let _srv = match Server::start(&addr, port) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("mhgu-ryusrv: cannot listen on {addr}:{port}: {e}");
            std::process::exit(1);
        }
    };
    loop {
        std::thread::sleep(std::time::Duration::from_secs(3600));
    }
}
