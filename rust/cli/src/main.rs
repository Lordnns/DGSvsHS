use gameplay::{launch_server, ServerConfig};

fn main() {
    let cfg = parse_args(std::env::args().skip(1));
    launch_server(cfg);
}

fn parse_args<I: Iterator<Item = String>>(args: I) -> ServerConfig {
    let mut cfg = ServerConfig::default();
    for raw in args {
        let arg = raw.strip_prefix("--").unwrap_or(&raw);
        let (key, val) = match arg.find('=') {
            Some(i) => (&arg[..i], &arg[i + 1..]),
            None => (arg, ""),
        };
        match key {
            "god-mode" => cfg.god_mode = parse_bool(val),
            "seed" => cfg.seed = parse_seed(val),
            "duration" => cfg.run_for_seconds = val.parse::<f32>().ok(),
            "help" | "h" | "?" => {
                print_help();
                std::process::exit(0);
            }
            _ => eprintln!("[cli] unknown arg: {}", raw),
        }
    }
    cfg
}

fn parse_bool(s: &str) -> bool {
    matches!(s, "" | "1" | "true" | "True" | "yes" | "on")
}

fn parse_seed(s: &str) -> u64 {
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).unwrap_or_else(|_| {
            eprintln!("[cli] bad hex seed: {}", s);
            std::process::exit(2);
        })
    } else {
        s.parse::<u64>().unwrap_or_else(|_| {
            eprintln!("[cli] bad seed: {}", s);
            std::process::exit(2);
        })
    }
}

fn print_help() {
    println!("DGSvsHS Bevy server — Rust/Bevy/Avian dedicated server (Build 2)");
    println!("Usage: dgsvshs-bevy [options]");
    println!("  --god-mode[=BOOL]   Disable enemy contact damage (default false)");
    println!("  --seed=0xHEX|N      RNG seed (default 0xC0FFEEF00D)");
    println!("  --duration=SEC      Exit after SEC wall-clock seconds (for trials)");
}
