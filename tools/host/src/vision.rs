//! VisionALVR around the ALVR host: the portable install layout, `config/visionalvr.json`, the control channel from the GUI
//! (stdin), the system check, the GPU sampler for the debug log, and the headset's identity from ALVR's client list.
use serde_json::{json, Value};
use std::{
    fs,
    io::BufRead,
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering},
    thread,
    time::Duration,
};

pub const VERSION: &str = "0.2-alpha";

/// Portable install: everything sits in the folder the user unzipped (`alvr_host.exe` next to the runtime DLLs), with
/// `config/` (session.json, visionalvr.json) and `logs/` inside it.
pub struct Install {
    pub dir: PathBuf,
    pub config: PathBuf,
    pub logs: PathBuf,
}

impl Install {
    pub fn new(dir: &Path) -> Self {
        Self { dir: dir.to_path_buf(), config: dir.join("config"), logs: dir.join("logs") }
    }
}

/// `config/visionalvr.json`, written by configure.exe / VisionALVR.exe. Command-line flags win over it.
#[derive(Default, Debug)]
pub struct Settings {
    pub idle_rgb: Option<u32>,
    pub encode_profile: Option<String>,
    pub qp_map: Option<bool>,
    pub split_encode: Option<String>,
    /// "asap" | "vsync" | "phase" (default): when an encoded frame leaves the host (tools/host/src/pipeline.rs)
    pub send_pacing: Option<String>,
    pub gpu_priority: Option<i64>,
    /// shim running start margin in ms (0 = release the game at the display tick)
    pub running_start_ms: Option<f64>,
    /// compositor boundary offset after the display tick, ms
    pub boundary_offset_ms: Option<f64>,
    /// D3DKMT process scheduling class to ask for (5 realtime .. 2; 0 = leave)
    pub gpu_sched_class: Option<i64>,
    pub debug: Option<bool>,
    pub gamma: Option<f32>,
    /// brightness, contrast, saturation, sharpening (0 = neutral)
    pub color: [f32; 4],
}

pub fn load_settings(path: &Path) -> Settings {
    // A UTF-8 BOM (Windows PowerShell's Set-Content / older Notepad) made serde_json reject the whole file and silently
    // dropped every setting (seen 2026-10-10): strip it.
    let v: Value = fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(t.trim_start_matches('\u{feff}')).ok()).unwrap_or_default();
    let video = &v["video"];
    Settings {
        idle_rgb: video["idle_rgb"].as_str().and_then(|s| u32::from_str_radix(s.trim_start_matches('#'), 16).ok()),
        encode_profile: video["encode_profile"].as_str().map(str::to_string),
        qp_map: video["qp_map"].as_bool(),
        split_encode: video["split_encode"].as_str().map(str::to_string),
        send_pacing: video["send_pacing"].as_str().map(str::to_string),
        gpu_priority: video["gpu_priority"].as_i64(),
        running_start_ms: video["running_start_ms"].as_f64(),
        boundary_offset_ms: video["boundary_offset_ms"].as_f64(),
        gpu_sched_class: video["gpu_sched_class"].as_i64(),
        debug: v["debug"].as_bool(),
        gamma: v["display"]["gamma"].as_f64().map(|g| g as f32),
        color: ["brightness", "contrast", "saturation", "sharpening"].map(|k| v["display"][k].as_f64().unwrap_or(0.0) as f32),
    }
}

/// First start of a fresh install: `config/session.json` from the shipped `config/session.default.json`.
pub fn seed_session(config: &Path) -> bool {
    let s = config.join("session.json");
    let d = config.join("session.default.json");
    !s.exists() && d.exists() && fs::copy(&d, &s).is_ok()
}

/// The headset's address may change (DHCP). ALVR re-finds a trusted headset through its discovery broadcasts and records the
/// new `current_ip`; put it first in `manual_ips` so the next start dials it directly. Returns the changes made.
pub fn refresh_manual_ips(session: &Path) -> Vec<String> {
    let Some(mut s) = fs::read_to_string(session).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()) else { return vec![] };
    let mut changes = vec![];
    if let Some(clients) = s["client_connections"].as_object_mut() {
        for (host, c) in clients.iter_mut() {
            let Some(cur) = c["current_ip"].as_str().map(str::to_string) else { continue };
            let ips = c["manual_ips"].as_array().cloned().unwrap_or_default();
            if ips.first().and_then(Value::as_str) != Some(cur.as_str()) {
                let mut new = vec![json!(cur)];
                new.extend(ips.into_iter().filter(|i| i.as_str() != Some(cur.as_str())));
                c["manual_ips"] = json!(new);
                changes.push(format!("{host}: manual IP {cur} first"));
            }
        }
    }
    if !changes.is_empty() {
        fs::write(session, serde_json::to_string_pretty(&s).unwrap()).ok();
    }
    changes
}

/// (hostname, display name, ip) of the client ALVR is streaming to (connection_state "Streaming"), else the first trusted one.
pub fn headset(session: &Path, streaming_only: bool) -> Option<(String, String, String)> {
    let s: Value = serde_json::from_str(&fs::read_to_string(session).ok()?).ok()?;
    let clients = s["client_connections"].as_object()?;
    let pick = clients.iter().find(|(_, c)| c["connection_state"] == "Streaming")
        .or_else(|| if streaming_only { None } else { clients.iter().find(|(_, c)| c["trusted"] == true) })?;
    let (h, c) = pick;
    let ip = c["current_ip"].as_str().map(str::to_string)
        .or_else(|| c["manual_ips"].as_array().and_then(|a| a.first()).and_then(Value::as_str).map(str::to_string))
        .unwrap_or_default();
    Some((h.clone(), c["display_name"].as_str().unwrap_or(h).to_string(), ip))
}

/// The negotiated stream in one line for the general log (ALVR persists it in `openvr_config` before it reports the connection).
pub fn stream_summary(session: &Path, encoded: (i32, i32)) -> String {
    let s: Value = fs::read_to_string(session).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
    let c = &s["openvr_config"];
    let bitrate = &s["session_settings"]["video"]["bitrate"]["mode"];
    let br = match bitrate["variant"].as_str() {
        Some("ConstantMbps") => format!("{} Mbps constant", bitrate["ConstantMbps"]),
        Some(v) => format!("bitrate {v}"),
        None => "bitrate ?".into(),
    };
    format!(
        "stream: {}x{} per eye @ {} Hz, {}, {}-bit, {}, encoded {}x{}, {br}, encoding gamma {}",
        c["eye_resolution_width"], c["eye_resolution_height"], c["refresh_rate"],
        match c["codec"].as_i64() { Some(0) => "H.264", Some(1) => "HEVC", Some(2) => "AV1", _ => "?" },
        if c["use_10bit_encoder"] == true { 10 } else { 8 },
        if c["enable_foveated_encoding"] == true { "foveated" } else { "full frame" },
        encoded.0, encoded.1, c["encoding_gamma"],
    )
}

// ---- control channel from the GUI (stdin lines) -------------------------------------------------------------------------

pub static QUIT: AtomicBool = AtomicBool::new(false);
/// user display gamma (f32 bits); 0 = unchanged
pub static GAMMA_REQ: AtomicU32 = AtomicU32::new(0);
/// -1 nothing pending, 0 debug off, 1 debug on
pub static DEBUG_REQ: AtomicI32 = AtomicI32::new(-1);
/// colour correction (brightness, contrast, saturation, sharpening as f32 bits) and its "pending" flag
pub static COLOR_REQ: [AtomicU32; 4] = [AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0)];
pub static COLOR_PENDING: AtomicBool = AtomicBool::new(false);

/// Commands (one per line): `quit`, `gamma <0.5..2.0>`, `debug on|off`, `color <brightness> <contrast> <saturation> <sharpening>`.
/// End of input (the GUI went away) = quit.
pub fn spawn_stdin_control() {
    thread::spawn(|| {
        let stdin = std::io::stdin();
        for line in stdin.lock().lines() {
            let Ok(line) = line else { break };
            let mut w = line.split_whitespace();
            match (w.next(), w.next()) {
                (Some("quit"), _) => break,
                (Some("gamma"), Some(g)) => {
                    if let Ok(g) = g.parse::<f32>() {
                        GAMMA_REQ.store(g.clamp(0.5, 2.0).to_bits(), Ordering::SeqCst);
                    }
                }
                (Some("debug"), Some(v)) => DEBUG_REQ.store((v == "on" || v == "1") as i32, Ordering::SeqCst),
                (Some("color"), Some(first)) => {
                    let vals: Vec<f32> = std::iter::once(first).chain(w).filter_map(|x| x.parse().ok()).collect();
                    if vals.len() == 4 {
                        for (k, v) in vals.iter().enumerate() {
                            COLOR_REQ[k].store(v.to_bits(), Ordering::SeqCst);
                        }
                        COLOR_PENDING.store(true, Ordering::SeqCst);
                    }
                }
                _ => {}
            }
        }
        QUIT.store(true, Ordering::SeqCst);
    });
}

// ---- system check -----------------------------------------------------------------------------------------------------------

/// NVML architecture numbers: Ada (RTX 40) = 8, Hopper = 9, Blackwell (RTX 50) = 10.
pub const MIN_ARCH: i64 = 8;

pub fn system_check(gpu_info: Value) -> Value {
    let arch = gpu_info["arch"].as_i64().unwrap_or(-1);
    let name = gpu_info["name"].as_str().unwrap_or("").to_string();
    // NVML missing or an unknown (newer) architecture: fall back on the name
    let by_name = ["RTX 40", "RTX 50", "RTX 60", "Ada", "Blackwell"].iter().any(|k| name.contains(k));
    let ok = arch >= MIN_ARCH || (arch < 0 && by_name);
    let override_ = std::env::var("VISIONALVR_ALLOW_ANY_GPU").as_deref() == Ok("1");
    json!({
        "version": VERSION,
        "alvr_protocol": alvr_common::protocol_id(),
        // what a compatible headset puts in its discovery broadcast (bytes 16..24, little endian), as a string (u64)
        "alvr_protocol_id": alvr_common::protocol_id_u64().to_string(),
        "gpu": gpu_info,
        "gpu_supported": ok,
        "gpu_override": override_,
        "ok": ok || override_,
        "reason": if ok { "".to_string() } else { format!("needs an NVIDIA GeForce RTX 40 series or newer (Ada / Blackwell); found '{name}'") },
    })
}

// ---- GPU sampler (debug sessions) -------------------------------------------------------------------------------------------

pub fn spawn_gpu_sampler(sample: fn() -> Option<String>) {
    thread::spawn(move || {
        let mut header_for: Option<PathBuf> = None;
        loop {
            thread::sleep(Duration::from_secs(1));
            let Some(dir) = crate::logs::debug_dir() else { continue };
            if header_for.as_ref() != Some(&dir) {
                crate::logs::debug_file_line("gpu.csv", "time,gpu_pct,mem_pct,enc_pct,dec_pct,graphics_mhz,memory_mhz,video_mhz,power_w,temp_c,pstate,vram_used_mb,clock_event_reasons");
                header_for = Some(dir);
            }
            if let Some(row) = sample() {
                crate::logs::debug_file_line("gpu.csv", &format!("{},{row}", crate::logs::local_time(false, false)));
            }
        }
    });
}
