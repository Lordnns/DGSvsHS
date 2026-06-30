use std::collections::VecDeque;
use std::error::Error;
use std::net::{SocketAddr, UdpSocket};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use std::thread;

use gameplay::game::constants::{ARENA_RADIUS, SIM_DT};
use gameplay::game::types::{InputCmd, InputFlags, Snapshot};
use gameplay::game::sim::rng::DeterministicRng;
use gameplay::network::codec::{
    read_server_welcome, write_client_hello, write_input_batch, SnapshotDecoder,
    MSG_CLIENT_HELLO, MSG_SERVER_WELCOME, MSG_SNAPSHOT, MSG_INPUT, R,
};

const ALPN: &[u8] = b"dgsvshs/2";
const MAX_DATAGRAM_SIZE: usize = 1350;
const RECV_BUF: usize = 65535;

#[derive(Debug, Clone)]
struct BotStats {
    connected: bool,
    player_id: Option<u8>,
    client_tick: u32,
    last_acked_server_tick: u32,
    rtt_ms: f32,
    round: u16,
    enemies: u32,
    packets_sent: u64,
    packets_received: u64,
}

impl Default for BotStats {
    fn default() -> Self {
        Self {
            connected: false,
            player_id: None,
            client_tick: 0,
            last_acked_server_tick: 0,
            rtt_ms: 0.0,
            round: 0,
            enemies: 0,
            packets_sent: 0,
            packets_received: 0,
        }
    }
}

struct AutoPilot {
    orbit_radius: f32,
    orbit_angular_speed: f32,
    orbit_phase: f32,
    aim_angular_speed: f32,
    aim_phase: f32,
    fire_on_ticks: i32,
    fire_off_ticks: i32,
}

impl AutoPilot {
    fn new(bot_id: u8, seed: u64) -> Self {
        // Seed both state words with SplitMix64 as per DeterministicRng
        let mut rng = DeterministicRng::from_seed(seed ^ (0xA5A5A5A5 + bot_id as u64));

        let orbit_radius = rng.next_range(ARENA_RADIUS * 0.4, ARENA_RADIUS * 0.75);
        let orbit_angular_speed = rng.next_range(0.5, 1.2);
        let orbit_phase = rng.next_range(0.0, std::f32::consts::PI * 2.0);

        let aim_angular_speed = rng.next_range(2.0, 4.5);
        let aim_phase = rng.next_range(0.0, std::f32::consts::PI * 2.0);

        let fire_on_ticks = rng.next_range(30.0, 60.0).round() as i32;
        let fire_off_ticks = rng.next_range(6.0, 18.0).round() as i32;

        Self {
            orbit_radius,
            orbit_angular_speed,
            orbit_phase,
            aim_angular_speed,
            aim_phase,
            fire_on_ticks,
            fire_off_ticks,
        }
    }

    fn sample(&mut self, client_tick: u32, local_pos_x: f32, local_pos_y: f32) -> InputCmd {
        let t = client_tick as f32 * SIM_DT;

        // Move: walk toward the next point on the orbit circle.
        let orbit_angle = self.orbit_phase + self.orbit_angular_speed * t;
        let target_x = orbit_angle.cos() * self.orbit_radius;
        let target_y = orbit_angle.sin() * self.orbit_radius;

        let to_target_x = target_x - local_pos_x;
        let to_target_y = target_y - local_pos_y;
        let sqr_mag = to_target_x * to_target_x + to_target_y * to_target_y;

        let (move_x, move_y) = if sqr_mag > 0.04 {
            let mag = sqr_mag.sqrt();
            (to_target_x / mag, to_target_y / mag)
        } else {
            (0.0, 0.0)
        };

        // Aim: pure tick-driven rotation.
        let aim_angle = self.aim_phase + self.aim_angular_speed * t;
        let aim_x = aim_angle.cos();
        let aim_y = aim_angle.sin();

        // Fire: fixed duty cycle in ticks.
        let period = self.fire_on_ticks + self.fire_off_ticks;
        let phase = if period > 0 {
            (client_tick % period as u32) as i32
        } else {
            0
        };
        let fire_held = phase < self.fire_on_ticks;
        let flags = if fire_held {
            InputFlags::FIRE
        } else {
            InputFlags::NONE
        };

        InputCmd {
            tick: client_tick,
            last_acked_server_tick: 0,
            move_x,
            move_y,
            aim_x,
            aim_y,
            flags,
        }
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().collect();
    let (server_host, num_bots, seed, duration) = parse_args(&args)?;

    let server_addr: SocketAddr = server_host.parse().map_err(|_| "invalid server address format (expected IP:PORT)")?;

    println!("Starting stress client targeting {}", server_addr);
    println!("Spawn configuration: {} bots, seed = 0x{:X}", num_bots, seed);
    if let Some(d) = duration {
        println!("Duration cap: {} seconds", d);
    }

    let shutdown = Arc::new(AtomicBool::new(false));
    let mut handles = Vec::new();
    let mut stats_list = Vec::new();

    for i in 0..num_bots {
        let bot_id = i as u8;
        let bot_stats = Arc::new(Mutex::new(BotStats::default()));
        stats_list.push(bot_stats.clone());

        let shutdown_clone = shutdown.clone();
        let handle = thread::spawn(move || {
            if let Err(e) = run_bot(server_addr, bot_id, seed, bot_stats, shutdown_clone) {
                eprintln!("[Bot {}] thread error: {:?}", bot_id, e);
            }
        });
        handles.push(handle);
    }

    // Monitor loop
    let start_time = Instant::now();
    loop {
        thread::sleep(Duration::from_secs(1));

        let elapsed = start_time.elapsed();
        if let Some(d) = duration {
            if elapsed >= Duration::from_secs_f32(d) {
                println!("\nDuration limit reached ({}s). Shutting down...", d);
                shutdown.store(true, Ordering::SeqCst);
                break;
            }
        }

        // Print stats table
        print!("{}[2J{}[H", 27 as char, 27 as char); // Clear screen
        println!("=== DGSvsHS Bevy Server Stress Client - Elapsed: {:.1}s ===", elapsed.as_secs_f32());
        println!("-----------------------------------------------------------------------------------------");
        println!("Bot ID | Player ID | Status     | Tick     | Acked    | RTT (ms) | Round | Enemies | TX / RX");
        println!("-----------------------------------------------------------------------------------------");
        for (i, stat_mutex) in stats_list.iter().enumerate() {
            if let Ok(s) = stat_mutex.lock() {
                let status_str = if s.connected { "Connected" } else { "Connecting" };
                let player_id_str = match s.player_id {
                    Some(pid) => pid.to_string(),
                    None => "-".to_string(),
                };
                println!(
                    " {:<5} | {:<9} | {:<10} | {:<8} | {:<8} | {:<8.1} | {:<5} | {:<7} | {} / {}",
                    i,
                    player_id_str,
                    status_str,
                    s.client_tick,
                    s.last_acked_server_tick,
                    s.rtt_ms,
                    s.round,
                    s.enemies,
                    s.packets_sent,
                    s.packets_received
                );
            }
        }
        println!("-----------------------------------------------------------------------------------------");
        println!("Press Ctrl+C to terminate client manually.");
    }

    for handle in handles {
        let _ = handle.join();
    }

    println!("All bot connections shut down successfully.");
    Ok(())
}

fn parse_args(args: &[String]) -> Result<(String, usize, u64, Option<f32>), Box<dyn Error>> {
    let mut server = "127.0.0.1:4433".to_string();
    let mut num_bots = 1;
    let mut seed = 0xC0FFEEF00D;
    let mut duration = None;

    let mut i = 1;
    while i < args.len() {
        let arg = &args[i];
        if arg.starts_with("--") {
            let clean = &arg[2..];
            let (key, val) = match clean.find('=') {
                Some(idx) => (&clean[..idx], Some(&clean[idx + 1..])),
                None => (clean, None),
            };

            match key {
                "server" | "s" => {
                    if let Some(v) = val {
                        server = v.to_string();
                    } else if i + 1 < args.len() {
                        i += 1;
                        server = args[i].clone();
                    }
                }
                "num-bots" | "n" => {
                    let v_str = if let Some(v) = val {
                        v
                    } else if i + 1 < args.len() {
                        i += 1;
                        &args[i]
                    } else {
                        return Err("missing value for --num-bots".into());
                    };
                    num_bots = v_str.parse::<usize>()?;
                    if num_bots < 1 || num_bots > 4 {
                        return Err("number of bots must be between 1 and 4".into());
                    }
                }
                "seed" => {
                    let v_str = if let Some(v) = val {
                        v
                    } else if i + 1 < args.len() {
                        i += 1;
                        &args[i]
                    } else {
                        return Err("missing value for --seed".into());
                    };
                    seed = if let Some(hex) = v_str.strip_prefix("0x").or_else(|| v_str.strip_prefix("0X")) {
                        u64::from_str_radix(hex, 16)?
                    } else {
                        v_str.parse::<u64>()?
                    };
                }
                "duration" | "d" => {
                    let v_str = if let Some(v) = val {
                        v
                    } else if i + 1 < args.len() {
                        i += 1;
                        &args[i]
                    } else {
                        return Err("missing value for --duration".into());
                    };
                    duration = Some(v_str.parse::<f32>()?);
                }
                "help" | "h" | "?" => {
                    println!("DGSvsHS Bevy stress client");
                    println!("Usage: stress_client [options]");
                    println!("  --server=IP:PORT / -s   Bevy Server QUIC endpoint (default 127.0.0.1:4433)");
                    println!("  --num-bots=N     / -n   Number of bots to run concurrent 1..4 (default 1)");
                    println!("  --seed=HEX/N            RNG seed (default 0xC0FFEEF00D)");
                    println!("  --duration=SEC   / -d   Run for SEC seconds, then exit gracefully");
                    std::process::exit(0);
                }
                _ => return Err(format!("unknown option --{}", key).into()),
            }
        } else {
            return Err(format!("unexpected argument: {}", arg).into());
        }
        i += 1;
    }

    Ok((server, num_bots, seed, duration))
}

fn run_bot(
    server_addr: SocketAddr,
    bot_id: u8,
    seed: u64,
    stats: Arc<Mutex<BotStats>>,
    shutdown: Arc<AtomicBool>,
) -> Result<(), Box<dyn Error>> {
    let mut config = quiche::Config::new(quiche::PROTOCOL_VERSION)?;
    config.set_application_protos(&[ALPN])?;
    config.verify_peer(false);
    config.set_max_idle_timeout(10_000);
    config.set_max_recv_udp_payload_size(MAX_DATAGRAM_SIZE);
    config.set_max_send_udp_payload_size(MAX_DATAGRAM_SIZE);
    config.set_initial_max_data(256 * 1024);
    config.set_initial_max_stream_data_bidi_local(64 * 1024);
    config.set_initial_max_stream_data_bidi_remote(64 * 1024);
    config.set_initial_max_stream_data_uni(64 * 1024);
    config.set_initial_max_streams_bidi(4);
    config.set_initial_max_streams_uni(4);
    config.enable_dgram(true, 256, 256);
    config.set_disable_active_migration(true);

    let socket = UdpSocket::bind("0.0.0.0:0")?;
    socket.set_nonblocking(true)?;
    let local_addr = socket.local_addr()?;

    let scid_bytes = mint_scid_bytes();
    let scid = quiche::ConnectionId::from_ref(&scid_bytes);
    let mut conn = quiche::connect(Some("dgsvshs"), &scid, local_addr, server_addr, &mut config)?;

    let mut buf = vec![0u8; RECV_BUF];
    let mut out = vec![0u8; MAX_DATAGRAM_SIZE];

    let mut hello_sent = false;
    let mut player_id = None;
    let mut client_tick = 0;
    let mut last_acked_server_tick = 0;
    let mut player_pos = (0.0f32, 0.0f32);
    let mut welcome_buf = Vec::new();
    let mut input_history = VecDeque::<InputCmd>::new();
    let mut autopilot = AutoPilot::new(bot_id, seed);
    let mut last_input_time: Option<Instant> = None;
    let mut snapshot_decoder = SnapshotDecoder::new();

    // Pump initial connection packets
    pump_outgoing(&mut conn, &socket, &mut out, &stats)?;

    while !shutdown.load(Ordering::SeqCst) && !conn.is_closed() {
        // Read from socket
        loop {
            match socket.recv_from(&mut buf) {
                Ok((n, from)) => {
                    let recv_info = quiche::RecvInfo { from, to: local_addr };
                    if let Ok(_) = conn.recv(&mut buf[..n], recv_info) {
                        if let Ok(mut s) = stats.lock() {
                            s.packets_received += 1;
                        }
                    }
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(e) => return Err(e.into()),
            }
        }

        if conn.is_established() {
            if let Ok(mut s) = stats.lock() {
                s.connected = true;
            }

            if !hello_sent {
                let mut body = Vec::new();
                write_client_hello(&mut body, 0);
                let mut payload = Vec::with_capacity(4 + 1 + body.len());
                payload.extend_from_slice(&(body.len() as u32).to_le_bytes());
                payload.push(MSG_CLIENT_HELLO);
                payload.extend_from_slice(&body);
                conn.stream_send(0, &payload, true)?;
                hello_sent = true;
            }
        }

        // Process readable streams
        let readable: Vec<u64> = conn.readable().collect();
        for sid in readable {
            let mut sbuf = [0u8; 256];
            loop {
                let (n, fin) = match conn.stream_recv(sid, &mut sbuf) {
                    Ok(v) => v,
                    Err(quiche::Error::Done) => break,
                    Err(e) => {
                        eprintln!("[Bot {}] stream_recv sid {} error: {:?}", bot_id, sid, e);
                        break;
                    }
                };
                if n > 0 {
                    welcome_buf.extend_from_slice(&sbuf[..n]);
                }
                if fin {
                    break;
                }
            }

            // Parse stream framed message: [length: u32 LE][msg_type: u8][payload]
            if welcome_buf.len() >= 5 {
                let len = u32::from_le_bytes([welcome_buf[0], welcome_buf[1], welcome_buf[2], welcome_buf[3]]) as usize;
                if welcome_buf.len() >= 4 + 1 + len {
                    let msg_type = welcome_buf[4];
                    let payload_slice = &welcome_buf[5..5 + len];
                    if msg_type == MSG_SERVER_WELCOME {
                        let mut r = R::new(payload_slice);
                        if let Ok(sw) = read_server_welcome(&mut r) {
                            player_id = Some(sw.player_id);
                            client_tick = sw.server_tick;
                            last_input_time = Some(Instant::now());
                            if let Ok(mut s) = stats.lock() {
                                s.player_id = Some(sw.player_id);
                                s.client_tick = sw.server_tick;
                            }
                        }
                    }
                }
            }
            welcome_buf.clear();
        }

        // Process incoming datagrams (snapshots)
        while let Ok(n) = conn.dgram_recv(&mut buf) {
            if n == 0 {
                continue;
            }
            let msg_type = buf[0];
            if msg_type == MSG_SNAPSHOT {
                let mut r = R::new(&buf[1..n]);
                let mut snap = Snapshot::with_capacity();
                if let Ok(ok) = snapshot_decoder.decode(&mut r, &mut snap) {
                    if ok {
                        last_acked_server_tick = snap.tick;
                        if let Some(pid) = player_id {
                            if let Some(player) = snap.players.iter().find(|p| p.id == pid) {
                                player_pos = (player.pos_x, player.pos_y);
                            }
                        }

                        if let Ok(mut s) = stats.lock() {
                            s.last_acked_server_tick = snap.tick;
                            s.round = snap.round;
                            s.enemies = snap.enemy_total_in_world;
                        }
                    }
                }
            }
        }

        // Handle precise 16ms input ticks
        if let Some(last_time) = last_input_time {
            let elapsed = last_time.elapsed();
            let ticks_to_send = (elapsed.as_millis() / 16) as u32;
            if ticks_to_send > 0 {
                for _ in 0..ticks_to_send {
                    client_tick += 1;
                    let mut cmd = autopilot.sample(client_tick, player_pos.0, player_pos.1);
                    cmd.last_acked_server_tick = last_acked_server_tick;

                    input_history.push_front(cmd);
                    if input_history.len() > 4 {
                        input_history.pop_back();
                    }

                    let mut payload = vec![MSG_INPUT];
                    let slice: Vec<InputCmd> = input_history.iter().copied().collect();
                    write_input_batch(&mut payload, &slice);

                    let _ = conn.dgram_send(&payload);
                }
                last_input_time = Some(last_time + Duration::from_millis(16 * ticks_to_send as u64));

                let rtt = conn.path_stats().next().map(|p| p.rtt).unwrap_or(Duration::from_secs(0));
                if let Ok(mut s) = stats.lock() {
                    s.client_tick = client_tick;
                    s.rtt_ms = rtt.as_secs_f32() * 1000.0;
                }
            }
        }

        // Pump outgoing
        pump_outgoing(&mut conn, &socket, &mut out, &stats)?;

        // Sleep to avoid CPU pegging, wait for timeout or next tick
        let now = Instant::now();
        let sleep_dur = if let Some(last_time) = last_input_time {
            let next_input_time = last_time + Duration::from_millis(16);
            next_input_time.saturating_duration_since(now)
        } else {
            Duration::from_millis(10)
        };
        let sleep_dur = sleep_dur.min(conn.timeout().unwrap_or(Duration::from_millis(10)));
        let sleep_dur = sleep_dur.min(Duration::from_millis(2));
        thread::sleep(sleep_dur);
    }

    // Graceful close
    let _ = conn.close(true, 0, b"shutdown");
    let _ = pump_outgoing(&mut conn, &socket, &mut out, &stats);

    Ok(())
}

fn pump_outgoing(
    conn: &mut quiche::Connection,
    socket: &UdpSocket,
    out: &mut [u8],
    stats: &Arc<Mutex<BotStats>>,
) -> Result<(), Box<dyn Error>> {
    loop {
        match conn.send(out) {
            Ok((n, send_info)) => {
                socket.send_to(&out[..n], send_info.to)?;
                if let Ok(mut s) = stats.lock() {
                    s.packets_sent += 1;
                }
            }
            Err(quiche::Error::Done) => return Ok(()),
            Err(e) => return Err(e.into()),
        }
    }
}

fn mint_scid_bytes() -> [u8; quiche::MAX_CONN_ID_LEN] {
    let mut bytes = [0u8; quiche::MAX_CONN_ID_LEN];
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0);
    bytes[..8].copy_from_slice(&nanos.to_be_bytes());
    bytes
}
