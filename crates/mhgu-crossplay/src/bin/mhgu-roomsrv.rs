// Standalone suyu-family room server (LDN rooms for the "Room" crossplay mode).
// Usage: mhgu-roomsrv [-port 24872] [-name MHGU] [-password ""]
use mhgu_crossplay::roomsrv;

fn arg(name: &str, default: &str) -> String {
    let a: Vec<String> = std::env::args().collect();
    a.iter().position(|s| s == name).and_then(|i| a.get(i + 1)).cloned().unwrap_or_else(|| default.into())
}

fn main() {
    let port: u16 = arg("-port", "24872").parse().unwrap_or(24872);
    let name = arg("-name", "MHGU");
    let pass = arg("-password", "");
    if let Err(e) = roomsrv::start(port, &name, &pass, "MHGU Host") {
        eprintln!("mhgu-roomsrv: cannot listen on 0.0.0.0:{port}: {e}");
        std::process::exit(1);
    }
    loop {
        std::thread::sleep(std::time::Duration::from_secs(3600));
    }
}
