//! VisionALVR logging.
//!
//! * General log `logs/VisionALVR.log`: appended by every component (host, shim inside each game, GUIs), one line per fact that
//!   matters later: start/stop, headset connect/disconnect, negotiated stream, game start/end, warnings and errors. Small.
//! * Debug session (only when debug is on): `logs/debug/<yyyymmdd-hhmmss>/` per host run with the verbose files: `host_events.jsonl`
//!   (every host event, `encoder_stats` every 2 s), `alvr.log` (server_core's own log), `gpu.csv` (NVML, 1 Hz), and one
//!   `shim_<exe>_<pid>.log` per game (the shim finds the folder through the IPC block).
//!
//! The host also installs the logger behind server_core (ALVR): its log lines go to `alvr.log`, its warnings/errors to the
//! general log, and its client statistics events (`StatisticsSummary` every second, `GraphStatistics` per frame: total / encode /
//! network / decode latency, packets lost, client fps) are captured for the status lines and the network benchmark.
use alvr_common::{log, parking_lot::Mutex};
use alvr_events::EventType;
use serde_json::{json, Value};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    sync::OnceLock,
};

struct State {
    logs_dir: Option<PathBuf>,
    general: Option<fs::File>,
    debug_dir: Option<PathBuf>,
    events: Option<fs::File>,
    alvr: Option<fs::File>,
    alvr_default: Option<PathBuf>,
}

static STATE: OnceLock<Mutex<State>> = OnceLock::new();

fn state() -> &'static Mutex<State> {
    STATE.get_or_init(|| Mutex::new(State { logs_dir: None, general: None, debug_dir: None, events: None, alvr: None, alvr_default: None }))
}

#[cfg(windows)]
#[repr(C)]
#[derive(Default)]
struct SystemTime {
    year: u16,
    month: u16,
    dow: u16,
    day: u16,
    hour: u16,
    minute: u16,
    second: u16,
    ms: u16,
}
#[cfg(windows)]
extern "system" {
    fn GetLocalTime(t: *mut SystemTime);
}

/// Local time "yyyy-mm-dd hh:mm:ss.mmm" (date = false: "hh:mm:ss.mmm"; compact = true: "yyyymmdd-hhmmss").
pub fn local_time(date: bool, compact: bool) -> String {
    #[cfg(windows)]
    {
        let mut t = SystemTime::default();
        unsafe { GetLocalTime(&mut t) };
        if compact {
            format!("{:04}{:02}{:02}-{:02}{:02}{:02}", t.year, t.month, t.day, t.hour, t.minute, t.second)
        } else if date {
            format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", t.year, t.month, t.day, t.hour, t.minute, t.second, t.ms)
        } else {
            format!("{:02}:{:02}:{:02}.{:03}", t.hour, t.minute, t.second, t.ms)
        }
    }
    #[cfg(not(windows))]
    {
        let s = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        let _ = (date, compact);
        format!("{s}")
    }
}

fn open_append(path: &Path) -> Option<fs::File> {
    fs::OpenOptions::new().create(true).append(true).open(path).ok()
}

/// `logs_dir`: the install's `logs/` (None in harness runs: no general log). `alvr_default`: where server_core's log goes when
/// debug is off (the harness reads `<config>/session_log.txt`).
pub fn init(logs_dir: Option<&Path>, alvr_default: Option<&Path>, debug: bool) {
    {
        let mut s = state().lock();
        if let Some(d) = logs_dir {
            fs::create_dir_all(d).ok();
            let g = d.join("VisionALVR.log");
            // keep the general log bounded: past 10 MB the old one is kept as .1
            if fs::metadata(&g).map(|m| m.len() > 10 << 20).unwrap_or(false) {
                fs::rename(&g, d.join("VisionALVR.log.1")).ok();
            }
            s.general = open_append(&g);
            s.logs_dir = Some(d.to_path_buf());
        }
        s.alvr_default = alvr_default.map(Path::to_path_buf);
        s.alvr = alvr_default.and_then(|p| fs::File::create(p).ok());
    }
    if debug {
        set_debug(true);
    }
    log::set_boxed_logger(Box::new(AlvrLogger)).ok();
    log::set_max_level(log::LevelFilter::Info);
    alvr_common::set_panic_hook();
}

/// One line in the general log: "time [host] LEVEL message".
pub fn general(level: &str, msg: &str) {
    let mut s = state().lock();
    if let Some(f) = s.general.as_mut() {
        writeln!(f, "{} [host] {level:<5} {msg}", local_time(true, false)).ok();
        f.flush().ok();
    }
}

/// Turns the debug session on (creates `logs/debug/<time>/` the first time) or off. Returns the session folder when on.
pub fn set_debug(on: bool) -> Option<PathBuf> {
    let mut s = state().lock();
    if !on {
        s.events = None;
        s.alvr = s.alvr_default.as_ref().and_then(|p| open_append(p));
        return None;
    }
    if s.debug_dir.is_none() {
        let base = s.logs_dir.clone().or_else(|| s.alvr_default.as_ref().and_then(|p| p.parent().map(|d| d.join("logs"))))?;
        let dir = base.join("debug").join(local_time(true, true));
        fs::create_dir_all(&dir).ok()?;
        s.debug_dir = Some(dir);
    }
    let dir = s.debug_dir.clone()?;
    s.events = open_append(&dir.join("host_events.jsonl"));
    s.alvr = open_append(&dir.join("alvr.log"));
    Some(dir)
}

pub fn debug_dir() -> Option<PathBuf> {
    let s = state().lock();
    s.events.as_ref().and(s.debug_dir.clone())
}

/// A host event (already a JSON line) into the debug session's `host_events.jsonl`.
pub fn event_line(line: &str) {
    let mut s = state().lock();
    if let Some(f) = s.events.as_mut() {
        writeln!(f, "{line}").ok();
    }
}

/// A line in the debug session folder's file `name` (e.g. gpu.csv), if debug is on.
pub fn debug_file_line(name: &str, line: &str) {
    let dir = debug_dir();
    if let Some(d) = dir {
        if let Some(mut f) = open_append(&d.join(name)) {
            writeln!(f, "{line}").ok();
        }
    }
}

// ---- server_core's logger ---------------------------------------------------------------------------------------------------

struct AlvrLogger;

impl log::Log for AlvrLogger {
    fn enabled(&self, meta: &log::Metadata) -> bool {
        meta.level() <= log::Level::Info && !meta.target().starts_with("mdns_sd")
    }

    fn log(&self, record: &log::Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let msg = format!("{}", record.args());
        if msg.starts_with('{') && msg.ends_with('}') {
            match serde_json::from_str::<EventType>(&msg) {
                Ok(EventType::GraphStatistics(g)) => {
                    client_stats::graph(&g); // per frame: aggregated, not written
                    return;
                }
                Ok(EventType::StatisticsSummary(sum)) => {
                    client_stats::summary(&sum);
                    write_alvr("STATS", &serde_json::to_string(&sum).unwrap_or_default());
                    return;
                }
                Ok(EventType::Tracking(_)) | Ok(EventType::Buttons(_)) => return, // only when ALVR's raw-event logging is on: too much
                Ok(e) => {
                    write_alvr("EVENT", &serde_json::to_string(&e).unwrap_or_default());
                    return;
                }
                Err(_) => {}
            }
        }
        // server_core logs this for every input sample of a button it has no mapping for (~400 lines/s from the AVP client):
        // keep the first line per button name
        if let Some(name) = msg.strip_prefix("Received button not mapped: ") {
            static SEEN: Mutex<Vec<String>> = Mutex::new(Vec::new());
            let mut seen = SEEN.lock();
            if seen.iter().any(|n| n == name) {
                return;
            }
            seen.push(name.to_string());
        }
        let level = match record.level() {
            log::Level::Error => "ERROR",
            log::Level::Warn => "WARN",
            log::Level::Info => "INFO",
            _ => "DEBUG",
        };
        write_alvr(level, &msg);
        if record.level() <= log::Level::Warn {
            general(level, &format!("alvr: {msg}"));
        }
    }

    fn flush(&self) {}
}

fn write_alvr(kind: &str, msg: &str) {
    let mut s = state().lock();
    if let Some(f) = s.alvr.as_mut() {
        writeln!(f, "{} [{kind}] {msg}", local_time(false, false)).ok();
    }
}

/// ALVR's client statistics, aggregated per window (status lines every 10 s, encoder_stats every 2 s, benchmark steps).
pub mod client_stats {
    use super::*;

    #[derive(Default, Clone)]
    pub struct Agg {
        n: u32,
        total: f64,
        game: f64,
        compositor: f64,
        encoder: f64,
        network: f64,
        decoder: f64,
        decoder_queue: f64,
        client_compositor: f64,
        vsync_queue: f64,
        client_frame_s: f64, // sum of 1 / client_fps over the samples with a sane value (client_n)
        client_n: u32,
        vsync_n: u32,        // samples with a sane vsync_queue (the client's unsigned subtraction can wrap to ~3e10 ms)
        since: Option<std::time::Instant>, // first sample of the window
        server_fps: f64,
        throughput_bps: f64,
        bitrate_bps: f64,
    }

    const ZERO: Agg = Agg {
        n: 0, total: 0.0, game: 0.0, compositor: 0.0, encoder: 0.0, network: 0.0, decoder: 0.0, decoder_queue: 0.0,
        client_compositor: 0.0, vsync_queue: 0.0, client_frame_s: 0.0, client_n: 0, vsync_n: 0, since: None, server_fps: 0.0, throughput_bps: 0.0, bitrate_bps: 0.0,
    };
    /// Windows: 0 = encoder_stats (2 s), 1 = status (10 s), 2 = benchmark step.
    static WINDOWS: std::sync::Mutex<[Agg; 3]> = std::sync::Mutex::new([ZERO, ZERO, ZERO]);
    static PACKETS_LOST_TOTAL: std::sync::Mutex<usize> = std::sync::Mutex::new(0);
    static LAST_SUMMARY: std::sync::Mutex<Option<Value>> = std::sync::Mutex::new(None);

    pub fn graph(g: &alvr_events::GraphStatistics) {
        for a in WINDOWS.lock().unwrap().iter_mut() {
            a.n += 1;
            a.total += g.total_pipeline_latency_s as f64;
            a.game += g.game_time_s as f64;
            a.compositor += g.server_compositor_s as f64;
            a.encoder += g.encoder_s as f64;
            a.network += g.network_s as f64;
            a.decoder += g.decoder_s as f64;
            a.decoder_queue += g.decoder_queue_s as f64;
            a.client_compositor += g.client_compositor_s as f64;
            a.since.get_or_insert_with(std::time::Instant::now);
            if (0.0..1.0).contains(&g.vsync_queue_s) {
                a.vsync_queue += g.vsync_queue_s as f64;
                a.vsync_n += 1;
            }
            // client_fps is per frame (1 / gap between two displayed frames): average the gaps, not the rates, which
            // overweighted short gaps; 0 and absurd values (1704 fps seen) are dropped
            if g.client_fps > 1.0 && g.client_fps < 500.0 {
                a.client_frame_s += 1.0 / g.client_fps as f64;
                a.client_n += 1;
            }
            a.server_fps += g.server_fps as f64;
            a.throughput_bps += g.throughput_bps as f64;
            a.bitrate_bps += g.bitrate_bps as f64;
        }
    }

    pub fn summary(s: &alvr_events::StatisticsSummary) {
        *PACKETS_LOST_TOTAL.lock().unwrap() = s.packets_lost_total;
        *LAST_SUMMARY.lock().unwrap() = serde_json::to_value(s).ok();
    }

    pub fn packets_lost_total() -> usize {
        *PACKETS_LOST_TOTAL.lock().unwrap()
    }

    pub fn last_summary() -> Option<Value> {
        LAST_SUMMARY.lock().unwrap().clone()
    }

    /// Averages of window `w` since its last take (milliseconds, fps, Mbps), then resets it. None if no client statistics arrived.
    pub fn take(w: usize) -> Option<Value> {
        let a = std::mem::take(&mut WINDOWS.lock().unwrap()[w]);
        if a.n == 0 {
            return None;
        }
        let n = a.n as f64;
        let ms = |x: f64| (x / n * 1000.0 * 100.0).round() / 100.0;
        let r1 = |x: f64| (x * 10.0).round() / 10.0;
        let client_fps = if a.client_n > 0 { r1(a.client_n as f64 / a.client_frame_s) } else { 0.0 };
        let vsync_queue_ms = if a.vsync_n > 0 { (a.vsync_queue / a.vsync_n as f64 * 1000.0 * 100.0).round() / 100.0 } else { 0.0 };
        // frames the headset reported as displayed, per second of window (frames it skipped never report)
        let secs = a.since.map(|t| t.elapsed().as_secs_f64()).unwrap_or(0.0);
        let displayed_fps = if secs > 0.5 { r1(n / secs) } else { client_fps };
        Some(json!({
            "samples": a.n,
            "total_latency_ms": ms(a.total), "game_ms": ms(a.game), "server_compositor_ms": ms(a.compositor),
            "encode_ms": ms(a.encoder), "network_ms": ms(a.network), "decode_ms": ms(a.decoder),
            "decoder_queue_ms": ms(a.decoder_queue), "client_compositor_ms": ms(a.client_compositor), "vsync_queue_ms": vsync_queue_ms,
            "client_fps": client_fps, "displayed_fps": displayed_fps, "server_fps": (a.server_fps / n * 10.0).round() / 10.0,
            "throughput_mbps": (a.throughput_bps / n / 1e6 * 10.0).round() / 10.0, "bitrate_mbps": (a.bitrate_bps / n / 1e6 * 10.0).round() / 10.0,
        }))
    }
}
