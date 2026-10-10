//! alvr_host: SteamVR-free ALVR streamer host (skeleton).
//!
//! Embeds `alvr_server_core` with an isolated config dir. Video comes from a `VideoSource`; the only source
//! today replays a pre-encoded Annex-B HEVC file (made by the nvenc/ test) so the whole ALVR protocol path
//! can be tested without a headset or a compositor. Frames are stamped with the latest tracking timestamp the
//! client sent (what ALVR's own driver does), so the client's timestamp-echo accounting works.
//!
//! Exit codes: 0 = pass (client connected and --min-frames sent), 1 = connected but fewer frames, 2 = no client
//! within --connect-timeout, 3 = bad arguments.
use alvr_common::{parking_lot::Mutex, ButtonType, BUTTON_INFO, HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID};
use alvr_packets::{ButtonValue, Haptics};
use alvr_server_core::{ServerCoreContext, ServerCoreEvent};
use alvr_session::CodecType;
mod bench_quality;
mod logs;
mod nvidia_profile;
mod vision;

use serde_json::json;
use std::{
    collections::VecDeque,
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

struct Args {
    config_dir: PathBuf,
    session: Option<PathBuf>,
    file: PathBuf,
    fps: f64,
    seconds: f64,
    connect_timeout: f64,
    min_frames: usize,
    exit_after_frames: Option<usize>,
    report: Option<PathBuf>,
    live: bool,
    width: Option<i32>,
    height: Option<i32>,
    noise: i32, // 0 smooth test pattern, 1 noisy scrolling pattern, 2 independent random frames (benchmarks)
    frames_csv: Option<PathBuf>,
    bench: Option<usize>,
    bench_sleep_ms: u64,
    gpu_keepalive: Option<i32>,
    shim: bool,
    fps_explicit: bool,
    daemon: bool,
    force_session: bool,
    split_encode: i64,
    /// hold each encoded app frame until the next display-clock tick before sending it (ALVR's SteamVR driver sends on
    /// its vsync grid; see the `send_on_vsync` handling in the main loop)
    send_on_vsync: bool,
    send_pacing_explicit: bool,
    /// 0: frames stamped with the sample the app rendered with; 1: newest sample + projection layers rotated to it; 2: 1 + the pose
    /// handed to the app is extrapolated to the expected stamp time
    stamp_mode: i32,
    stamp_explicit: bool,
    fresh_wait_ms: f64,
    /// display clock phase-locked to the headset's tracking packets (its real display rate) instead of ALVR's free-running grid
    pacing_tracking: bool,
    pacing_explicit: bool,
    pacing_guard_ms: f64,
    /// app render size relative to the headset's per-eye size (GPU load lever; the stream keeps its size)
    render_scale: f64,
    qp_map: Option<bool>,
    dump_qpmap: Option<PathBuf>,
    idle_rgb: Option<u32>,
    gpu_priority: Option<i64>,
    encode_profile: nvidia_profile::EncodeProfile,
    split_explicit: bool,
    profile_explicit: bool,
    install_dir: Option<PathBuf>,
    logs_dir: Option<PathBuf>,
    debug: Option<bool>,
    gui: bool,
    system_check: bool,
    gamma: Option<f32>,
    bench_network: Option<Vec<u32>>,
    bench_step_s: f64,
    noise_block: i64,
    noise_amp: i64,
    bench_mbps: Option<u64>,
    bench_quality: bool,
    bq_plan: String,
    bq_frames: i32,
    bq_warmup: i32,
    bq_metric_every: i32,
    bq_stride: i32,
    bq_out: Option<PathBuf>,
    bq_preview: Option<PathBuf>,
    bq_ref_scale: i32,
    bench_scene: Option<PathBuf>,
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args {
        config_dir: PathBuf::from("host_config"),
        session: None,
        file: PathBuf::new(),
        fps: 90.0,
        seconds: 30.0,
        connect_timeout: 60.0,
        min_frames: 0,
        exit_after_frames: None,
        report: None,
        live: false,
        width: None,
        height: None,
        noise: 0,
        frames_csv: None,
        bench: None,
        bench_sleep_ms: 0,
        gpu_keepalive: None,
        shim: false,
        fps_explicit: false,
        daemon: false,
        force_session: false,
        // forced 3-strip split-frame encoding: NVENC's auto mode only splits frames >= ~2000 lines high (measured on an RTX
        // 5090, tools/nvenc_split_probe), so the foveated 4224x1664 frame stayed on one engine (6.9 ms vs 2.5 ms). NVENC uses
        // min(3, engines) strips, so this is also right on 1- and 2-engine GPUs.
        split_encode: 3,
        send_on_vsync: false,
        send_pacing_explicit: false,
        stamp_mode: 2,
        stamp_explicit: false,
        fresh_wait_ms: 3.0,
        pacing_tracking: true,
        pacing_explicit: false,
        pacing_guard_ms: 2.0,
        render_scale: 1.0,
        qp_map: None,
        dump_qpmap: None,
        idle_rgb: None,
        gpu_priority: None,
        encode_profile: nvidia_profile::EncodeProfile::Session,
        split_explicit: false,
        profile_explicit: false,
        install_dir: None,
        logs_dir: None,
        debug: None,
        gui: false,
        system_check: false,
        gamma: None,
        bench_network: None,
        bench_step_s: 8.0,
        noise_block: 16,
        noise_amp: 48,
        bench_mbps: None,
        bench_quality: false,
        bq_plan: "full".into(),
        bq_frames: 90,
        bq_warmup: 20,
        bq_metric_every: 10,
        bq_stride: 1,
        bq_out: None,
        bq_preview: None,
        bq_ref_scale: 1,
        bench_scene: None,
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let mut v = || it.next().ok_or(format!("missing value for {k}"));
        match k.as_str() {
            "--config-dir" => a.config_dir = v()?.into(),
            "--session" => a.session = Some(v()?.into()),
            "--file" => a.file = v()?.into(),
            "--fps" => {
                a.fps = v()?.parse().map_err(|e| format!("{e}"))?;
                a.fps_explicit = true;
            }
            "--seconds" => a.seconds = v()?.parse().map_err(|e| format!("{e}"))?,
            "--connect-timeout" => a.connect_timeout = v()?.parse().map_err(|e| format!("{e}"))?,
            "--min-frames" => a.min_frames = v()?.parse().map_err(|e| format!("{e}"))?,
            "--exit-after-frames" => a.exit_after_frames = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--report" => a.report = Some(v()?.into()),
            "--live" => a.live = true,
            "--width" => a.width = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--height" => a.height = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--noise" => a.noise = 1,
            "--noise-temporal" => a.noise = 2,
            // VisionALVR portable install: config/ and logs/ inside this folder, settings from config/visionalvr.json
            "--install-dir" => a.install_dir = Some(v()?.into()),
            "--logs-dir" => a.logs_dir = Some(v()?.into()),
            "--debug" => a.debug = Some(true),
            "--gui" => a.gui = true, // started by VisionALVR.exe: control commands on stdin, end of stdin = quit
            "--system-check" => a.system_check = true,
            "--gamma" => a.gamma = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--benchmark-network" => {
                a.bench_network = Some(v()?.split(',').map(|x| x.trim().parse::<u32>().map_err(|e| format!("--benchmark-network: {e}"))).collect::<Result<_, _>>()?);
                a.live = true;
                a.noise = 2;
            }
            "--bench-step-s" => a.bench_step_s = v()?.parse().map_err(|e| format!("{e}"))?,
            "--noise-block" => a.noise_block = v()?.parse().map_err(|e| format!("{e}"))?,
            "--noise-amp" => a.noise_amp = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bench-mbps" => a.bench_mbps = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            // benchmark phase 2: local quality sweep on the benchmark scene (no headset; see bench_quality.rs)
            "--benchmark-quality" => a.bench_quality = true,
            "--bq-plan" => a.bq_plan = v()?,
            "--bq-frames" => a.bq_frames = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bq-warmup" => a.bq_warmup = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bq-metric-every" => a.bq_metric_every = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bq-stride" => a.bq_stride = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bq-out" => a.bq_out = Some(v()?.into()),
            "--bq-preview" => a.bq_preview = Some(v()?.into()),
            "--bq-ref-scale" => a.bq_ref_scale = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bench-scene" => a.bench_scene = Some(v()?.into()),
            "--encode-profile" => {
                a.encode_profile = nvidia_profile::EncodeProfile::parse(&v()?)?;
                a.profile_explicit = true;
            }
            "--split-encode" => {
                // NVENC split-frame encoding across several NVENC engines (single session, single bitstream)
                a.split_explicit = true;
                a.split_encode = parse_split(&v()?)?;
            }
            "--send-pacing" => {
                a.send_pacing_explicit = true;
                a.send_on_vsync = parse_send_pacing(&v()?)?;
            }
            "--stamp" => {
                a.stamp_explicit = true;
                a.stamp_mode = parse_stamp(&v()?)?;
            }
            "--fresh-wait-ms" => a.fresh_wait_ms = v()?.parse().map_err(|e| format!("{e}"))?,
            "--pacing" => {
                a.pacing_explicit = true;
                a.pacing_tracking = parse_pacing(&v()?)?;
            }
            "--pacing-guard-ms" => a.pacing_guard_ms = v()?.parse().map_err(|e| format!("{e}"))?,
            "--render-scale" => a.render_scale = v()?.parse().map_err(|e| format!("{e}"))?,
            "--qp-map" => {
                a.qp_map = Some(match v()?.as_str() {
                    "on" | "1" => true,
                    "off" | "0" => false,
                    o => return Err(format!("--qp-map on|off, got {o}")),
                })
            }
            "--dump-qpmap" => a.dump_qpmap = Some(v()?.into()),
            "--idle-rgb" => a.idle_rgb = Some(parse_rgb(&v()?)?),
            "--gpu-priority" => a.gpu_priority = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--force-session" => a.force_session = true,
            "--daemon" => a.daemon = true, // run until stopped: no connect/stream time limits (reconnects are handled)
            "--shim" => {
                a.shim = true;
                a.live = true;
            }
            "--gpu-keepalive" => a.gpu_keepalive = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--bench-sleep-ms" => a.bench_sleep_ms = v()?.parse().map_err(|e| format!("{e}"))?,
            "--bench" => a.bench = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--frames-csv" => a.frames_csv = Some(v()?.into()),
            other => return Err(format!("unknown argument {other}")),
        }
    }
    // environment equivalents (the installed logon task has a fixed command line)
    if a.idle_rgb.is_none() {
        if let Ok(v) = std::env::var("VISIONALVR_IDLE_RGB") {
            a.idle_rgb = Some(parse_rgb(&v)?);
        }
    }
    if a.gpu_priority.is_none() {
        if let Ok(v) = std::env::var("VISIONALVR_GPU_PRIORITY") {
            a.gpu_priority = Some(v.parse().map_err(|e| format!("VISIONALVR_GPU_PRIORITY: {e}"))?);
        }
    }
    if !a.profile_explicit {
        if let Ok(v) = std::env::var("VISIONALVR_ENCODE_PROFILE") {
            a.encode_profile = nvidia_profile::EncodeProfile::parse(&v)?;
            a.profile_explicit = true;
        }
    }
    if !a.live && a.file.as_os_str().is_empty() && !a.system_check && !a.bench_quality {
        return Err("--file <annexb.hevc> is required unless --live".into());
    }
    Ok(a)
}

/// The negotiated ALVR encoding gamma (persisted in `openvr_config` at connect). The client decodes with pow(v, gamma); the shim
/// sends pow(v, 1/gamma) (ALVR's SteamVR compositor does the same in FrameRender.fx). 1.0 if absent.
fn read_encoding_gamma(session_path: &std::path::Path) -> f32 {
    fs::read_to_string(session_path).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|s| s["openvr_config"]["encoding_gamma"].as_f64()).filter(|g| *g > 0.1 && *g < 10.0).unwrap_or(1.0) as f32
}

fn parse_split(v: &str) -> Result<i64, String> {
    Ok(match v {
        "auto" => 0,
        "auto-forced" => 1,
        "2" | "two" => 2,
        "3" | "three" => 3,
        "off" => 15,
        o => return Err(format!("--split-encode auto|auto-forced|2|3|off, got {o}")),
    })
}

fn parse_stamp(s: &str) -> Result<i32, String> {
    match s {
        "predict" => Ok(2),
        "newest" | "warp" => Ok(1),
        "match" | "rendered" => Ok(0),
        o => Err(format!("--stamp predict|newest|match, got {o}")),
    }
}

fn stamp_name(mode: i32) -> &'static str {
    match mode {
        2 => "predict",
        1 => "newest",
        _ => "match",
    }
}

fn parse_pacing(s: &str) -> Result<bool, String> {
    match s {
        "tracking" | "headset" => Ok(true),
        "grid" | "alvr" => Ok(false),
        o => Err(format!("--pacing tracking|grid, got {o}")),
    }
}

fn parse_send_pacing(s: &str) -> Result<bool, String> {
    match s {
        "vsync" | "tick" => Ok(true),
        "asap" | "off" => Ok(false),
        o => Err(format!("--send-pacing asap|vsync, got {o}")),
    }
}

/// Fills what the command line left open from `config/visionalvr.json`, then applies the encode profile's implications.
fn apply_settings(a: &mut Args, st: &vision::Settings) {
    if !a.send_pacing_explicit {
        if let Some(p) = st.send_pacing.as_deref().and_then(|p| parse_send_pacing(p).ok()) {
            a.send_on_vsync = p;
        }
    }
    if !a.stamp_explicit {
        if let Some(p) = st.stamp.as_deref().and_then(|p| parse_stamp(p).ok()) {
            a.stamp_mode = p;
        }
    }
    if let Some(w) = st.fresh_wait_ms {
        a.fresh_wait_ms = w.clamp(0.0, 10.0);
    }
    if !a.pacing_explicit {
        if let Some(p) = st.pacing.as_deref().and_then(|p| parse_pacing(p).ok()) {
            a.pacing_tracking = p;
        }
    }
    if let Some(g) = st.pacing_guard_ms {
        a.pacing_guard_ms = g.clamp(0.0, 6.0);
    }
    if let Some(r) = st.render_scale {
        a.render_scale = r.clamp(0.25, 2.0);
    }
    if a.idle_rgb.is_none() {
        a.idle_rgb = st.idle_rgb;
    }
    if a.gpu_priority.is_none() {
        a.gpu_priority = st.gpu_priority;
    }
    if a.qp_map.is_none() {
        a.qp_map = st.qp_map;
    }
    if !a.split_explicit {
        if let Some(m) = st.split_encode.as_deref().and_then(|m| parse_split(m).ok()) {
            a.split_encode = m;
            a.split_explicit = true;
        }
    }
    if !a.profile_explicit {
        if let Some(p) = st.encode_profile.as_deref().and_then(|p| nvidia_profile::EncodeProfile::parse(p).ok()) {
            a.encode_profile = p;
        }
    }
    if a.debug.is_none() {
        a.debug = st.debug;
    }
    if a.gamma.is_none() {
        a.gamma = st.gamma;
    }
    if a.encode_profile == nvidia_profile::EncodeProfile::FullSplit {
        // whole frame, straight to the encoder engines: no QP map, 3 strips (explicit --qp-map / --split-encode still win)
        if a.qp_map.is_none() {
            a.qp_map = Some(false);
        }
        if !a.split_explicit {
            a.split_encode = 3;
        }
    }
}

/// "r,g,b" (0-255 each) or "RRGGBB" -> 0xRRGGBB
fn parse_rgb(v: &str) -> Result<u32, String> {
    let parts: Vec<&str> = v.split(',').map(str::trim).collect();
    match parts.as_slice() {
        [r, g, b] => {
            let c = |x: &str| x.parse::<u8>().map(u32::from).map_err(|e| format!("--idle-rgb {v}: {e}"));
            Ok((c(r)? << 16) | (c(g)? << 8) | c(b)?)
        }
        [hex] if hex.len() == 6 => u32::from_str_radix(hex, 16).map_err(|e| format!("--idle-rgb {v}: {e}")),
        _ => Err(format!("--idle-rgb r,g,b or RRGGBB, got {v}")),
    }
}

/// Logs what NVENC was actually configured with (negotiated: preset, rate control, split-frame conditions and engine count,
/// capabilities, QP map) and optionally dumps the QP map. Returns the description for the final report.
fn describe_encoder(origin: Instant, enc: *mut std::ffi::c_void, dump: &Option<PathBuf>) -> serde_json::Value {
    let mut buf = vec![0 as std::ffi::c_char; 8192];
    let n = unsafe { nvh::nvh_describe(enc, buf.as_mut_ptr(), buf.len() as i32) };
    let mut v = json!(null);
    if n > 0 {
        let s = unsafe { std::ffi::CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().to_string();
        v = serde_json::from_str(&s).unwrap_or_else(|e| json!({ "raw": s, "parse_error": e.to_string() }));
    }
    event(origin, "nvenc_config", v.clone());
    if let Some(p) = dump {
        if let Ok(c) = std::ffi::CString::new(p.to_string_lossy().as_bytes()) {
            let ok = unsafe { nvh::nvh_qpmap_dump(c.as_ptr()) };
            event(origin, "qpmap_dump", json!({ "path": p, "written": ok }));
        }
    }
    v
}

/// One `encoder_stats` event per window: encode time, frame count, bitrate, slices per frame (>1 = split-frame encode in effect).
struct StatsWindow {
    start: Instant,
    first_idx: usize,
    bytes0: u64,
    samples0: usize,
    slices0: usize,
    matched0: u32,
    fallback0: u32,
    ticks0: u64,
}

/// display-clock ticks so far (mirror of the vsync thread's counter, for the stats windows)
static VSYNC_TICKS: AtomicU64 = AtomicU64::new(0);
/// Pacing diagnostics, reported per `encoder_stats` window. Ticks are nanoseconds since the host's `origin` instant.
static LAST_TICK_NS: AtomicU64 = AtomicU64::new(0);
static LAST_TICK_QPC: AtomicU64 = AtomicU64::new(0); // f64 bits, QPC seconds (shared clock with the shim's CSV)
static NEXT_TICK_NS: AtomicU64 = AtomicU64::new(0);
/// app frames sent with the same client timestamp as the previous frame (the headset treats them as already shown)
static SAME_TS_FRAMES: AtomicU64 = AtomicU64::new(0);
/// send instant relative to the display-clock tick (asap: render + encode time; vsync pacing: lateness after the tick)
static SEND_PHASE_MS: std::sync::Mutex<Vec<f64>> = std::sync::Mutex::new(Vec::new());
/// time an encoded frame waited for the tick (vsync pacing only; 0 = the frame was already late)
static HOLD_MS: std::sync::Mutex<Vec<f64>> = std::sync::Mutex::new(Vec::new());
/// spacing of the headset's tracking packets arriving at the host
static TRACKING_GAP_MS: std::sync::Mutex<Vec<f64>> = std::sync::Mutex::new(Vec::new());
static LAST_TRACKING_NS: AtomicU64 = AtomicU64::new(0);
static LAST_HEAD_POSE: std::sync::Mutex<Option<(alvr_common::glam::Quat, alvr_common::glam::Vec3)>> = std::sync::Mutex::new(None);
static HEAD_STEP_MDEG: std::sync::Mutex<Vec<f64>> = std::sync::Mutex::new(Vec::new());
static HEAD_STEP_MM: std::sync::Mutex<Vec<f64>> = std::sync::Mutex::new(Vec::new());
/// display-clock tick minus the latest tracking arrival (tracking pacing: should sit near the guard)
static TICK_AFTER_TRACKING_MS: std::sync::Mutex<Vec<f64>> = std::sync::Mutex::new(Vec::new());

/// Phase-locked loop on the headset's tracking packets: the visionOS client sends exactly one per display frame, so their
/// arrival grid is the headset's display clock (90, 96 or 100 Hz) plus network jitter. The display-clock tick is placed a guard
/// after the estimated arrival, so the app reads a fresh sample for every frame and runs at the headset's real rate.
#[derive(Default)]
struct TrackingPll {
    period_ns: f64,
    /// a point of the estimated arrival grid (kept within one period of the latest arrival)
    anchor: Option<Instant>,
    gaps: VecDeque<f64>,
    last: Option<Instant>,
    updates: u64,
}
static PLL: std::sync::Mutex<TrackingPll> = std::sync::Mutex::new(TrackingPll { period_ns: 0.0, anchor: None, gaps: VecDeque::new(), last: None, updates: 0 });

/// A tracking packet arrived now. Returns this arrival's residual vs the estimated grid (ms; negative = early).
fn pll_arrival(now: Instant) -> f64 {
    let Ok(mut pll) = PLL.lock() else { return 0.0 };
    let mut residual_ms = 0.0;
    if let Some(prev) = pll.last {
        let gap = now.duration_since(prev).as_secs_f64() * 1e9;
        if gap < 60e6 {
            pll.gaps.push_back(gap);
            if pll.gaps.len() > 64 {
                pll.gaps.pop_front();
            }
        } else {
            // a stall: start over (the anchor would otherwise pull the grid for seconds)
            pll.anchor = None;
            pll.gaps.clear();
        }
    }
    pll.last = Some(now);
    if pll.gaps.len() >= 16 {
        let mut v: Vec<f64> = pll.gaps.iter().copied().collect();
        v.sort_by(|a, b| a.partial_cmp(b).unwrap());
        // the median gap is the display period (a late packet makes one long gap and one short one)
        pll.period_ns = v[v.len() / 2].clamp(8e6, 14e6);
    } else if pll.period_ns == 0.0 {
        pll.period_ns = 1e9 / 90.0; // until 16 gaps are in: the headset rates in play are 90-100 Hz
    }
    let p = pll.period_ns;
    match pll.anchor {
        None => pll.anchor = Some(now),
        Some(a) => {
            let dt = if now >= a { now.duration_since(a).as_nanos() as f64 } else { -(a.duration_since(now).as_nanos() as f64) };
            let k = (dt / p).round();
            let r = dt - k * p; // this arrival vs the grid: negative = earlier than estimated
            // Network delay only ever makes packets late, so the grid hugs the early edge of the arrivals: pull quickly when a
            // packet comes early, drift slowly when it comes late.
            let adj = if r < 0.0 { 0.2 * r } else { 0.02 * r };
            let shift = k * p + adj;
            pll.anchor = Some(if shift >= 0.0 { a + Duration::from_nanos(shift as u64) } else { a - Duration::from_nanos((-shift) as u64) });
            pll.updates += 1;
            residual_ms = r / 1e6;
        }
    }
    residual_ms
}

/// Next display-clock tick from the tracking grid: (tick instant, period). None while unlocked or when tracking stopped.
fn pll_next_tick(now: Instant, guard_ns: f64) -> Option<(Instant, Duration)> {
    let pll = PLL.lock().ok()?;
    let a = pll.anchor?;
    if pll.updates < 8 || pll.gaps.len() < 16 || now.duration_since(pll.last?) > Duration::from_millis(250) {
        return None;
    }
    let p = pll.period_ns;
    let dt = if now >= a { now.duration_since(a).as_nanos() as f64 } else { -(a.duration_since(now).as_nanos() as f64) };
    // smallest grid point whose tick (grid + guard) is at least a quarter period ahead (never two ticks back to back)
    let mut k = ((dt + 0.25 * p - guard_ns) / p).floor() + 1.0;
    if k < 0.0 {
        k = 0.0;
    }
    let t = k * p + guard_ns;
    let tick = if t >= 0.0 { a + Duration::from_nanos(t as u64) } else { a - Duration::from_nanos((-t) as u64) };
    Some((tick, Duration::from_nanos(p as u64)))
}

fn pll_status() -> (bool, f64, f64) {
    let Ok(pll) = PLL.lock() else { return (false, 0.0, 0.0) };
    let since_last = pll.last.map(|l| l.elapsed().as_secs_f64() * 1000.0).unwrap_or(1e9);
    (pll.updates >= 8 && pll.gaps.len() >= 16 && since_last < 250.0, pll.period_ns / 1e6, since_last)
}

fn push_bounded(m: &std::sync::Mutex<Vec<f64>>, v: f64) {
    if let Ok(mut g) = m.lock() {
        if g.len() < 8192 {
            g.push(v);
        }
    }
}

/// p50 / p95 / max / count of a sample set (plus `over_<x>ms` counts), for the stats windows
fn pct_json(v: &mut Vec<f64>, over: &[(&str, f64)]) -> serde_json::Value {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q = |p: f64| v.get(((v.len() as f64) * p) as usize).copied().unwrap_or(0.0);
    let mut e = json!({ "p50": q(0.5), "p95": q(0.95), "max": v.last().copied().unwrap_or(0.0), "n": v.len() });
    for (name, thr) in over {
        e[*name] = json!(v.iter().filter(|x| **x > *thr).count());
    }
    e
}

/// Sleeps to just before `t`, then spins (a plain sleep is only ms-accurate).
fn sleep_spin_until(t: Instant) {
    loop {
        let left = t.saturating_duration_since(Instant::now());
        if left.is_zero() {
            break;
        }
        if left > Duration::from_micros(1500) {
            thread::sleep(left - Duration::from_micros(1000));
        } else {
            std::hint::spin_loop();
        }
    }
}

impl StatsWindow {
    fn new() -> Self {
        Self { start: Instant::now(), first_idx: 0, bytes0: 0, samples0: 0, slices0: 0, matched0: 0, fallback0: 0, ticks0: 0 }
    }
    fn tick(&mut self, origin: Instant, encode_ms: &[f64], bytes: u64, live: Option<&LiveCtx>, shim: Option<(u32, u32, u32)>, intake_ms: &[f64], ffr_wait_ms: &[f64]) {
        if self.start.elapsed() < Duration::from_secs(2) {
            return;
        }
        let secs = self.start.elapsed().as_secs_f64();
        let mut w = encode_ms[self.first_idx.min(encode_ms.len())..].to_vec();
        w.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let q = |p: f64| w.get(((w.len() as f64) * p) as usize).copied().unwrap_or(0.0);
        let (samples, slices) = live.map(|l| (l.slice_samples, l.slices)).unwrap_or((0, 0));
        let (submit_mode, matched, fallback) = shim.unwrap_or((0, 0, 0));
        let mut e = json!({
            "frames": w.len(), "fps": w.len() as f64 / secs, "encode_ms_avg": w.iter().sum::<f64>() / w.len().max(1) as f64,
            "encode_ms_p95": q(0.95), "encode_ms_max": w.last().copied().unwrap_or(0.0),
            "mbps": (bytes - self.bytes0) as f64 * 8.0 / secs / 1e6,
            "slices_per_packet": (slices - self.slices0) as f64 / (samples - self.samples0).max(1) as f64,
        });
        if ffr_wait_ms.len() >= w.len() && !w.is_empty() {
            // encode time split: queued foveation pass done on the GPU (waits behind the game's GPU work) vs. copy + NVENC
            let n = w.len();
            let mut fw = ffr_wait_ms[ffr_wait_ms.len() - n..].to_vec();
            let rest: Vec<f64> = encode_ms[encode_ms.len() - n..].iter().zip(&fw).map(|(e, f)| (e - f).max(0.0)).collect();
            let mut rs = rest.clone();
            fw.sort_by(|a, b| a.partial_cmp(b).unwrap());
            rs.sort_by(|a, b| a.partial_cmp(b).unwrap());
            let p = |v: &Vec<f64>, q: f64| v.get(((v.len() as f64) * q) as usize).copied().unwrap_or(0.0);
            // GPU time of ALVR's foveation pass (timestamp queries); the rest of the encode time is queueing + copy + NVENC
            e["ffr_gpu_ms"] = json!({ "avg": fw.iter().sum::<f64>() / n as f64, "p95": p(&fw, 0.95) });
            e["encode_minus_ffr_ms"] = json!({ "avg": rs.iter().sum::<f64>() / n as f64, "p95": p(&rs, 0.95) });
        }
        if !intake_ms.is_empty() {
            // spacing of the app's frames arriving at the host in this window: the app's real frame rate
            let mut v = intake_ms[intake_ms.len().saturating_sub(w.len())..].to_vec();
            v.sort_by(|a, b| a.partial_cmp(b).unwrap());
            let p = |q: f64| v.get(((v.len() as f64) * q) as usize).copied().unwrap_or(0.0);
            e["app_frame_interval_ms"] = json!({ "p50": p(0.5), "p95": p(0.95), "p99": p(0.99), "max": v.last().copied().unwrap_or(0.0),
                "over_16_7ms": v.iter().filter(|x| **x > 16.7).count() }); // > 1.5 periods at 90 Hz: a missed frame
        }
        if shim.is_some() {
            // how the shim chose frame timestamps in this window (pose match vs. last pose read), and its submit mode
            e["vsync_ticks"] = json!(VSYNC_TICKS.load(Ordering::Relaxed).wrapping_sub(self.ticks0));
            e["shim"] = json!({ "submit_mode": if submit_mode == 1 { "async" } else { "sync" },
                "ts_matched": matched.wrapping_sub(self.matched0), "ts_fallback": fallback.wrapping_sub(self.fallback0) });
        }
        if let Some(c) = logs::client_stats::take(0) {
            e["client"] = c; // ALVR's client statistics over the window: total / encode / network / decode latency, client fps
        }
        // pacing diagnostics (moving-head judder investigation): duplicate timestamps, tracking spacing, send phase
        e["same_ts_frames"] = json!(SAME_TS_FRAMES.swap(0, Ordering::Relaxed));
        if let Ok(mut g) = TRACKING_GAP_MS.lock() {
            if !g.is_empty() {
                let mut v = std::mem::take(&mut *g);
                e["tracking_gap_ms"] = pct_json(&mut v, &[("over_12ms", 12.0), ("over_15ms", 15.0)]);
            }
        }
        if let Ok(mut g) = SEND_PHASE_MS.lock() {
            if !g.is_empty() {
                let mut v = std::mem::take(&mut *g);
                e["send_after_tick_ms"] = pct_json(&mut v, &[("over_11ms", 11.0)]);
            }
        }
        if let Ok(mut g) = HOLD_MS.lock() {
            if !g.is_empty() {
                let mut v = std::mem::take(&mut *g);
                let late = v.iter().filter(|x| **x <= 0.0).count();
                e["send_hold_ms"] = pct_json(&mut v, &[]);
                e["send_hold_ms"]["late_frames"] = json!(late);
            }
        }
        for (name, m) in [("head_step_mdeg", &HEAD_STEP_MDEG), ("head_step_mm", &HEAD_STEP_MM)] {
            if let Ok(mut g) = m.lock() {
                if !g.is_empty() {
                    let mut v = std::mem::take(&mut *g);
                    // p05 ~ the still-head jitter floor, p50 the typical step
                    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
                    let q = |p: f64| v.get(((v.len() as f64) * p) as usize).copied().unwrap_or(0.0);
                    e[name] = json!({ "p05": q(0.05), "p25": q(0.25), "p50": q(0.5), "p95": q(0.95), "n": v.len() });
                }
            }
        }
        {
            let (locked, period_ms, since_last_ms) = pll_status();
            e["tracking_pll"] = json!({ "locked": locked, "period_ms": period_ms, "since_last_ms": since_last_ms });
            if let Ok(mut g) = TICK_AFTER_TRACKING_MS.lock() {
                if !g.is_empty() {
                    let mut v = std::mem::take(&mut *g);
                    e["tick_after_tracking_ms"] = pct_json(&mut v, &[]);
                }
            }
        }
        event(origin, "encoder_stats", e);
        *self = Self { start: Instant::now(), first_idx: encode_ms.len(), bytes0: bytes, samples0: samples, slices0: slices, matched0: matched, fallback0: fallback, ticks0: VSYNC_TICKS.load(Ordering::Relaxed) };
    }
}

fn event(origin: Instant, name: &str, extra: serde_json::Value) {
    let wall = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0);
    let line = json!({ "t_ms": origin.elapsed().as_secs_f64() * 1000.0, "unix_s": wall, "event": name, "data": extra }).to_string();
    println!("{line}");
    logs::event_line(&line);
}

// ---- HEVC Annex-B access-unit splitter ------------------------------------------------------------------

struct AccessUnit {
    data: Vec<u8>,
    is_idr: bool,
}

/// Position of the next Annex-B start code at or after `from` (a preceding zero byte counts as part of it: 4-byte code).
/// Skips 3 bytes whenever the third byte of the window rules out a start code there (packets are ~0.3-3 MB, scanned on the
/// encode thread before the frame goes to the network).
fn next_start(buf: &[u8], from: usize) -> Option<usize> {
    let mut i = from;
    while i + 3 <= buf.len() {
        let b2 = buf[i + 2];
        if b2 > 1 {
            i += 3;
        } else if b2 == 1 && buf[i] == 0 && buf[i + 1] == 0 {
            return Some(if i > 0 && buf[i - 1] == 0 { i - 1 } else { i });
        } else {
            i += 1;
        }
    }
    None
}

/// NAL unit type of the NAL whose start code begins at `s`.
fn nal_type(buf: &[u8], s: usize) -> Option<u8> {
    let sc = if buf[s..].starts_with(&[0, 0, 0]) { 4 } else { 3 };
    buf.get(s + sc).map(|h| (h >> 1) & 0x3f)
}

fn nal_ranges(buf: &[u8]) -> Vec<(usize, usize)> {
    // (start of start-code, end) for every NAL
    let mut starts = vec![];
    let mut i = 0;
    while let Some(s) = next_start(buf, i) {
        starts.push(s);
        i = s + 3;
    }
    (0..starts.len()).map(|k| (starts[k], *starts.get(k + 1).unwrap_or(&buf.len()))).collect()
}

/// VCL NAL units (slices) in an access unit.
fn count_vcl(buf: &[u8]) -> usize {
    let (mut n, mut i) = (0, 0);
    while let Some(s) = next_start(buf, i) {
        n += nal_type(buf, s).is_some_and(|t| t < 32) as usize;
        i = s + 3;
    }
    n
}

fn split_access_units(buf: &[u8]) -> Vec<AccessUnit> {
    let mut aus: Vec<AccessUnit> = vec![];
    let mut cur: Vec<u8> = vec![];
    let (mut cur_has_vcl, mut cur_idr) = (false, false);
    for (s, e) in nal_ranges(buf) {
        let sc = if buf[s] == 0 && buf[s + 1] == 0 && buf[s + 2] == 0 { 4 } else { 3 };
        let hdr = buf[s + sc];
        let nal_type = (hdr >> 1) & 0x3f;
        let is_vcl = nal_type < 32;
        let first_slice = is_vcl && buf.get(s + sc + 2).is_some_and(|b| b & 0x80 != 0);
        let starts_au = matches!(nal_type, 32..=35 | 39) || first_slice;
        if starts_au && cur_has_vcl {
            aus.push(AccessUnit { data: std::mem::take(&mut cur), is_idr: cur_idr });
            cur_has_vcl = false;
            cur_idr = false;
        }
        if is_vcl {
            cur_has_vcl = true;
            cur_idr |= matches!(nal_type, 19 | 20);
        }
        cur.extend_from_slice(&buf[s..e]);
    }
    if cur_has_vcl {
        aus.push(AccessUnit { data: cur, is_idr: cur_idr });
    }
    aus
}

/// Mirrors ALVR NalParsing.cpp: if the packet starts with VPS, the first 3 NALs (VPS, SPS, PPS) are the
/// decoder config and only the remainder is sent as the frame.
fn split_config(au: &[u8]) -> (Option<&[u8]>, &[u8]) {
    // only the first 4 NALs matter: no full scan of the frame
    let Some(mut s) = next_start(au, 0) else { return (None, au) };
    if nal_type(au, s) != Some(32) {
        return (None, au);
    }
    for _ in 0..3 {
        match next_start(au, s + 3) {
            Some(n) => s = n,
            None => return (None, au), // fewer than 4 NALs
        }
    }
    (Some(&au[..s]), &au[s..])
}

// ---- live NVENC (C++ wrapper around the unmodified ALVR encoder, see nvenc/hostlib) ---------------------------

mod nvh {
    use std::ffi::{c_char, c_void};
    pub type PacketCb = extern "C" fn(user: *mut c_void, data: *const u8, len: i32, ts_ns: u64, is_idr: i32);
    pub type ParamsCb = extern "C" fn(user: *mut c_void, bitrate_bps: *mut u64, framerate: *mut f32) -> i32;
    extern "C" {
        pub fn nvh_set(key: *const c_char, value: i64) -> i32;
        pub fn nvh_create(w: i32, h: i32, noise: i32, p: PacketCb, q: ParamsCb, user: *mut c_void, err: *mut c_char, errlen: i32) -> *mut c_void;
        pub fn nvh_encode(h: *mut c_void, idx: u32, ts_ns: u64, force_idr: i32, err: *mut c_char, errlen: i32) -> f64;
        pub fn nvh_destroy(h: *mut c_void);
        pub fn nvh_keepalive(h: *mut c_void, on: i32, duty_pct: i32) -> i32;
        pub fn nvh_set_f(key: *const c_char, value: f64) -> i32;
        pub fn nvh_encoded_size(w: *mut i32, h: *mut i32);
        pub fn nvh_encoder_engines(h: *mut c_void) -> i32;
        pub fn nvh_describe(h: *mut c_void, buf: *mut std::ffi::c_char, len: i32) -> i32;
        pub fn nvh_qpmap_dump(path: *const std::ffi::c_char) -> i32;
        pub fn nvh_ipc_last_compose_ms(ipc: *mut c_void) -> f32;
        pub fn nvh_last_ffr_gpu_ms(h: *mut c_void) -> f32;
        pub fn nvh_ipc_open() -> *mut c_void;
        pub fn nvh_ipc_config(ipc: *mut c_void, nvh: *mut c_void, rate_hz: f64, eye_w: i32, eye_h: i32, fov8: *const f32, off6: *const f32, bits: i32, encoding_gamma: f32);
        pub fn nvh_last_ffr_wait_ms(h: *mut c_void) -> f32;
        pub fn nvh_ipc_set_connected(ipc: *mut c_void, connected: i32);
        pub fn nvh_ipc_publish_head(ipc: *mut c_void, ts: u64, q: *const f32, p: *const f32, lv: *const f32, av: *const f32);
        pub fn nvh_ipc_publish_hand(ipc: *mut c_void, side: i32, valid: i32, q: *const f32, p: *const f32, lv: *const f32, av: *const f32);
        pub fn nvh_ipc_vsync(ipc: *mut c_void, interval_s: f64);
        pub fn nvh_ipc_publish_input(ipc: *mut c_void, side: i32, clicks: u32, touches: u32, trigger: f32, squeeze: f32, sx: f32, sy: f32);
        pub fn nvh_ipc_poll_haptic(ipc: *mut c_void, side: i32, seq: *mut u32, freq: *mut f32, amp: *mut f32);
        pub fn nvh_ipc_wait_frame(ipc: *mut c_void, timeout_ms: i32, counter: *mut u64, slot: *mut u32, ts: *mut u64, submit_time_s: *mut f64) -> i32;
        pub fn nvh_qpc_seconds() -> f64;
        pub fn nvh_encode_green(nvh: *mut c_void, frame_no: u32, ts_ns: u64, force_idr: i32, err: *mut c_char, errlen: i32) -> f64;
        pub fn nvh_ipc_shim_alive(ipc: *mut c_void) -> i32;
        pub fn nvh_encode_shared(nvh: *mut c_void, ipc: *mut c_void, slot: u32, frame_no: u32, ts_ns: u64, force_idr: i32, err: *mut c_char, errlen: i32) -> f64;
        pub fn nvh_ipc_close(ipc: *mut c_void);
        pub fn nvh_release_shared(nvh: *mut c_void);
        pub fn nvh_ipc_set_user(ipc: *mut c_void, user_gamma: f32, debug_on: i32, debug_dir: *const u16);
        pub fn nvh_ipc_app_exe(ipc: *mut c_void, buf: *mut c_char, len: i32) -> i32;
        pub fn nvh_ipc_set_color(ipc: *mut c_void, brightness: f32, contrast: f32, saturation: f32, sharpening: f32);
        pub fn nvh_ipc_set_pacing(ipc: *mut c_void, stamp_mode: i32, fresh_wait_ms: f32, render_scale: f32);
        pub fn nvh_gpu_info(buf: *mut c_char, len: i32) -> i32;
        pub fn nvh_gpu_sample(buf: *mut c_char, len: i32) -> i32;
        pub fn nvh_ipc_shim_stats(ipc: *mut c_void, submit_mode: *mut u32, ts_matched: *mut u32, ts_fallback: *mut u32);
    }
}

/// Per-hand input state in the Touch layout (see ovrshim/ipc.h).
#[derive(Default, Clone, Copy)]
struct HandInputState {
    clicks: u32,
    touches: u32,
    trigger: f32,
    squeeze: f32,
    stick_x: f32,
    stick_y: f32,
}

impl HandInputState {
    fn apply(&mut self, input: &str, value: &ButtonValue) {
        let on = match value {
            ButtonValue::Binary(b) => *b,
            ButtonValue::Scalar(v) => *v > 0.5,
        };
        let v = match value {
            ButtonValue::Scalar(v) => *v,
            ButtonValue::Binary(b) => *b as u32 as f32,
        };
        let set = |mask: &mut u32, bit: u32| {
            if on {
                *mask |= bit
            } else {
                *mask &= !bit
            }
        };
        match input {
            "a/click" | "x/click" => set(&mut self.clicks, 1),
            "b/click" | "y/click" => set(&mut self.clicks, 2),
            "thumbstick/click" => set(&mut self.clicks, 4),
            "menu/click" => set(&mut self.clicks, 8),
            "system/click" => set(&mut self.clicks, 16),
            "a/touch" | "x/touch" => set(&mut self.touches, 1),
            "b/touch" | "y/touch" => set(&mut self.touches, 2),
            "thumbstick/touch" => set(&mut self.touches, 4),
            "trigger/touch" => set(&mut self.touches, 8),
            "thumbrest/touch" => set(&mut self.touches, 16),
            "trigger/value" => self.trigger = v,
            "squeeze/value" => self.squeeze = v,
            "thumbstick/x" => self.stick_x = v,
            "thumbstick/y" => self.stick_y = v,
            _ => {}
        }
    }
}

struct LiveCtx {
    ctx: Arc<ServerCoreContext>,
    shared: Arc<Shared>,
    packet_bytes: usize,
    packet_idr: bool,
    packets: usize,
    slices: usize,           // VCL NAL units in the sampled packets (slices per frame > 1 means NVENC split the frame across engines)
    slice_samples: usize,    // packets whose slices were counted (every 8th: it is only a statistic)
    report_in_callback: bool, // false in shim mode: composed/present are reported with real timings before encoding
    cb_ns: u128,     // time spent inside on_packet (server_core hand-off)
    params_ns: u128, // time spent inside on_params
    bitrate_override: u64, // network benchmark: this bitrate instead of server_core's (0 = server_core decides)
    bytes_total: u64,
    bitrate_sent: u64,
    fps: f32,
    /// vsync send pacing: on_packet queues the encoded frame in `pending` while this is set; the main loop sends it at the tick
    hold_now: bool,
    pending: Vec<(Duration, Vec<u8>, bool, Option<Vec<u8>>)>,
}

/// Sends the packets `on_packet` queued while `hold_now` was set (in order, config NALs first).
fn flush_pending(live: &mut LiveCtx) {
    for (ts, frame, is_idr, config) in live.pending.drain(..) {
        if let Some(c) = config {
            live.ctx.set_video_config_nals(c, CodecType::Hevc);
        }
        live.ctx.send_video_nal(ts, frame, is_idr);
    }
}

extern "C" fn on_packet(user: *mut std::ffi::c_void, data: *const u8, len: i32, ts_ns: u64, is_idr: i32) {
    let t0 = Instant::now();
    let live = unsafe { &mut *(user as *mut LiveCtx) };
    let buf = unsafe { std::slice::from_raw_parts(data, len as usize) };
    let ts = Duration::from_nanos(ts_ns);
    let (config, frame) = split_config(buf);
    if config.is_some() {
        live.shared.config_sent.fetch_add(1, Ordering::Relaxed);
    }
    if live.hold_now {
        live.pending.push((ts, frame.to_vec(), is_idr != 0, config.map(|c| c.to_vec())));
    } else {
        if let Some(c) = config {
            live.ctx.set_video_config_nals(c.to_vec(), CodecType::Hevc);
        }
        live.ctx.send_video_nal(ts, frame.to_vec(), is_idr != 0);
    }
    if live.report_in_callback {
        live.ctx.report_composed(ts, Duration::from_millis(1));
        live.ctx.report_present(ts, Duration::from_millis(1));
    }
    if live.packets % 8 == 0 {
        live.slices += count_vcl(buf);
        live.slice_samples += 1;
    }
    live.packet_bytes += frame.len();
    live.bytes_total += buf.len() as u64;
    live.packet_idr |= is_idr != 0;
    live.packets += 1;
    live.cb_ns += t0.elapsed().as_nanos();
}

extern "C" fn on_params(user: *mut std::ffi::c_void, bitrate: *mut u64, fps: *mut f32) -> i32 {
    let t0 = Instant::now();
    let live = unsafe { &mut *(user as *mut LiveCtx) };
    if live.bitrate_override != 0 {
        if live.bitrate_override == live.bitrate_sent {
            return 0;
        }
        live.bitrate_sent = live.bitrate_override;
        unsafe {
            *bitrate = live.bitrate_override;
            *fps = live.fps;
        }
        return 1;
    }
    let r = live.ctx.get_dynamic_encoder_params();
    live.params_ns += t0.elapsed().as_nanos();
    match r {
        Some(p) => unsafe {
            *bitrate = p.bitrate_bps as u64;
            *fps = p.framerate;
            1
        },
        None => 0,
    }
}

/// Pushes the encoder configuration to the NVENC code: the encoder parameters are the hardcoded NVIDIA profile, the stream
/// parameters (refresh rate, eye resolution, foveation) come from the session's `openvr_config` as before.
fn configure_encoder(session_path: &std::path::Path, split_mode: i64, qp_map: bool, host_keys: &[(&std::ffi::CStr, i64)]) -> (i32, i32, f64) {
    let s: serde_json::Value = serde_json::from_str(&fs::read_to_string(session_path).expect("read session")).expect("parse session");
    let c = &s["openvr_config"];
    unsafe {
        nvh::nvh_set(c"nvenc_split_encode_mode".as_ptr(), split_mode);
        nvh::nvh_set(c"nvenc_qp_map".as_ptr(), qp_map as i64);
        nvh::nvh_set(c"qp_map_center_delta".as_ptr(), nvidia_profile::QP_MAP_CENTER_DELTA);
        nvh::nvh_set(c"qp_map_edge_delta".as_ptr(), nvidia_profile::QP_MAP_EDGE_DELTA);
        nvh::nvh_set_f(c"qp_map_transition".as_ptr(), nvidia_profile::QP_MAP_TRANSITION);
        for (k, v) in host_keys {
            nvh::nvh_set(k.as_ptr(), *v);
        }
    }
    for (k, n) in nvidia_profile::NVH_KEYS {
        let ck = std::ffi::CString::new(*k).unwrap();
        unsafe { nvh::nvh_set(ck.as_ptr(), *n) };
    }
    let fset = &s["session_settings"]["video"]["foveated_encoding"]["content"];
    for (k, sk) in [
        ("foveation_center_size_x", "center_size_x"), ("foveation_center_size_y", "center_size_y"),
        ("foveation_center_shift_x", "center_shift_x"), ("foveation_center_shift_y", "center_shift_y"),
        ("foveation_edge_ratio_x", "edge_ratio_x"), ("foveation_edge_ratio_y", "edge_ratio_y"),
    ] {
        // ALVR writes zeros to openvr_config when foveation is off (and its FFR code divides by them): then use the geometry from the
        // session's foveation settings (still the user's values; the QP map follows them even without the warp)
        if let Some(v) = c[k].as_f64().filter(|v| *v > 0.0).or_else(|| fset[sk].as_f64().filter(|v| *v > 0.0)) {
            let ck = std::ffi::CString::new(k).unwrap();
            unsafe { nvh::nvh_set_f(ck.as_ptr(), v) };
        }
    }
    for k in ["refresh_rate", "enable_foveated_encoding", "eye_resolution_width", "eye_resolution_height"] {
        let v = &c[k];
        if let Some(n) = v.as_i64().or_else(|| v.as_bool().map(|b| b as i64)) {
            let ck = std::ffi::CString::new(k).unwrap();
            unsafe { nvh::nvh_set(ck.as_ptr(), n) };
        }
    }
    let w = c["eye_resolution_width"].as_i64().unwrap_or(1920) as i32 * 2;
    let h = c["eye_resolution_height"].as_i64().unwrap_or(1080) as i32;
    (w, h, c["refresh_rate"].as_f64().unwrap_or(90.0))
}

/// Exit code for a setup problem the user has to fix (missing or broken config); restarting the streamer cannot help.
const EXIT_SETUP: i32 = 7;

/// Reports a setup problem in plain words (stderr, the general log, a `setup_error` event the GUIs show) and exits.
fn setup_fail(origin: Instant, message: &str) -> ! {
    eprintln!("{message}");
    logs::general("ERROR", message);
    event(origin, "setup_error", json!({ "message": message }));
    std::process::exit(EXIT_SETUP);
}

/// The session ALVR, the encoder and the benchmarks read: present and valid JSON, else a message that says what to do.
fn check_session(path: &std::path::Path, installed: bool) -> Result<(), String> {
    let text = match fs::read_to_string(path) {
        Ok(t) => t,
        Err(_) if installed => {
            return Err("config\\session.json is missing and there is no config\\session.default.json to create it from: this \
                        VisionALVR folder is incomplete. Unzip the release again (you can keep your config\\ folder)."
                .into())
        }
        Err(e) => return Err(format!("cannot read the session {}: {e} (pass --session <file> or --install-dir <VisionALVR folder>)", path.display())),
    };
    match serde_json::from_str::<serde_json::Value>(&text) {
        Ok(v) if v.is_object() => Ok(()),
        Ok(_) => Err(format!("{} is not an ALVR session (expected a JSON object)", path.display())),
        Err(e) => Err(format!(
            "{} is not valid JSON ({e}). Fix the edit, or delete the file to start again from config\\session.default.json \
             (your headset pairing is stored in it: pair again with configure.exe).",
            path.display()
        )),
    }
}

/// Vision Pro views measured in the pilot (visionalvr_views.json): used until the headset has sent its own.
const AVP_DEFAULT_FOV: [f32; 8] = [1.7823, 1.0186, 1.2169, 1.0186, 1.0249, 1.7995, 1.2253, 1.0249];
const AVP_DEFAULT_IPD: f32 = 0.0623;

/// `--benchmark-quality`: benchmark phase 2 (bench_quality.rs) from the session's stream settings, the headset's last views and the
/// bitrate (`--bench-mbps`, else the session's constant bitrate). Results: `bq_*` events, `--bq-out` JSON, a general log line.
fn run_quality_bench(origin: Instant, args: &Args, layout: &alvr_filesystem::Layout, install: Option<&vision::Install>) -> i32 {
    let session = layout.session();
    let host_keys = [(c"idle_rgb", 0x00FF00i64), (c"gpu_thread_priority", 0)];
    let qp_on = args.qp_map.unwrap_or(nvidia_profile::QP_MAP_ENABLED);
    configure_encoder(&session, args.split_encode, qp_on, &host_keys);
    let s: serde_json::Value = fs::read_to_string(&session).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(json!({}));
    let c = &s["openvr_config"];
    let fset = &s["session_settings"]["video"]["foveated_encoding"];
    let g = |k: &str, sk: &str, d: f64| c[k].as_f64().filter(|v| *v > 0.0).or_else(|| fset["content"][sk].as_f64().filter(|v| *v > 0.0)).unwrap_or(d) as f32;
    let (fov, ipd) = match load_views(&args.config_dir.join("visionalvr_views.json")) {
        Some((f, o)) if f.iter().all(|x| x.is_finite() && *x > 0.0) => (f, (o[3] - o[0]).abs().max(0.04)),
        _ => (AVP_DEFAULT_FOV, AVP_DEFAULT_IPD),
    };
    let base = bench_quality::Base {
        eye_w: args.width.map(|w| w / 2).or(c["eye_resolution_width"].as_i64().map(|v| v as i32)).filter(|v| *v > 0).unwrap_or(3552),
        eye_h: args.height.or(c["eye_resolution_height"].as_i64().map(|v| v as i32)).filter(|v| *v > 0).unwrap_or(3200),
        fps: if args.fps_explicit { args.fps } else { c["refresh_rate"].as_f64().filter(|v| *v > 0.0).unwrap_or(90.0) },
        foveated: c["enable_foveated_encoding"].as_bool().or(fset["enabled"].as_bool()).unwrap_or(true),
        geom: [
            g("foveation_center_size_x", "center_size_x", 0.45), g("foveation_center_size_y", "center_size_y", 0.4),
            g("foveation_center_shift_x", "center_shift_x", 0.4), g("foveation_center_shift_y", "center_shift_y", 0.1),
            g("foveation_edge_ratio_x", "edge_ratio_x", 4.0), g("foveation_edge_ratio_y", "edge_ratio_y", 5.0),
        ],
        preset: nvidia_profile::NVH_KEYS.iter().find(|(k, _)| *k == "nvenc_quality_preset").map(|(_, v)| *v as i32).unwrap_or(3),
        aq: nvidia_profile::NVH_KEYS.iter().find(|(k, _)| *k == "nvenc_adaptive_quantization_mode").map(|(_, v)| *v as i32).unwrap_or(1),
        split: args.split_encode as i32,
        qp_map: qp_on,
        bitrate_mbps: args.bench_mbps.map(|m| m as f64).or(s["session_settings"]["video"]["bitrate"]["mode"]["ConstantMbps"].as_f64()).unwrap_or(250.0),
        fov,
        ipd,
        engines: 0,
    };
    let exe_dir = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf()));
    let scene = args.bench_scene.clone().unwrap_or_else(|| {
        install.map(|i| i.dir.clone()).or(exe_dir).unwrap_or_default().join("bench").join("littlest_tokyo.vab")
    });
    let opt = bench_quality::Options {
        warmup: args.bq_warmup,
        frames: args.bq_frames,
        metric_every: args.bq_metric_every,
        metric_stride: args.bq_stride,
        out: args.bq_out.clone().or_else(|| install.map(|i| i.config.join("benchmark_quality.json"))),
        preview: args.bq_preview.clone(),
        plan: args.bq_plan.clone(),
        ref_scale: args.bq_ref_scale,
        keepalive: args.gpu_keepalive.unwrap_or(60),
    };
    bench_quality::run(origin, &scene, &base, &opt, &|o, n, d| event(o, n, d))
}

// ---- main ------------------------------------------------------------------------------------------------

/// Stream parameters ALVR writes into the session's `openvr_config` when it negotiates with a client (it persists them at connect,
/// before the headset sends its views). Everything the encoder and the shim's frame ring are built from.
#[derive(Clone, PartialEq, Debug)]
struct StreamCfg {
    eye_w: i64,
    eye_h: i64,
    fps: f64,
    foveated: bool,
    geom: [f64; 6],
}

fn read_stream_cfg(session_path: &std::path::Path) -> StreamCfg {
    let s: serde_json::Value = serde_json::from_str(&fs::read_to_string(session_path).expect("read session")).expect("parse session");
    let c = &s["openvr_config"];
    let g = |k: &str| c[k].as_f64().unwrap_or(0.0);
    StreamCfg {
        eye_w: c["eye_resolution_width"].as_i64().unwrap_or(0),
        eye_h: c["eye_resolution_height"].as_i64().unwrap_or(0),
        fps: g("refresh_rate"),
        foveated: c["enable_foveated_encoding"].as_bool().unwrap_or(false),
        // the geometry only matters (and ALVR only keeps it) while foveation is on
        geom: if c["enable_foveated_encoding"].as_bool().unwrap_or(false) {
            [
                g("foveation_center_size_x"), g("foveation_center_size_y"), g("foveation_center_shift_x"),
                g("foveation_center_shift_y"), g("foveation_edge_ratio_x"), g("foveation_edge_ratio_y"),
            ]
        } else {
            [0.0; 6]
        },
    }
}

#[cfg(windows)]
extern "system" {
    fn GetCurrentThread() -> *mut std::ffi::c_void;
    fn SetThreadPriority(thread: *mut std::ffi::c_void, priority: i32) -> i32;
}

/// n / p50 / p95 / p99 / max (ms) of a list of intervals, plus how many exceed 1.5x the nominal period.
fn interval_stats(v: &[f64], period_ms: f64) -> serde_json::Value {
    let mut s = v.to_vec();
    s.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q = |p: f64| s.get(((s.len() as f64) * p) as usize).copied().unwrap_or(0.0);
    json!({
        "n": s.len(), "min": s.first().copied().unwrap_or(0.0), "p01": q(0.01), "p50": s.get(s.len() / 2).copied().unwrap_or(0.0), "p95": q(0.95), "p99": q(0.99),
        "under_0p5_period": s.iter().filter(|x| **x < 0.5 * period_ms).count(),
        "max": s.last().copied().unwrap_or(0.0), "over_1p5_period": s.iter().filter(|x| **x > 1.5 * period_ms).count(),
    })
}

/// Highest bitrate step that held the frame rate without packet loss and without the network latency climbing (the link is
/// saturating), minus 10% headroom.
fn bench_recommendation(results: &[serde_json::Value], fps: f64) -> serde_json::Value {
    let f = |r: &serde_json::Value, k: &str| r[k].as_f64().unwrap_or(f64::NAN);
    let min_net = results.iter().map(|r| f(r, "network_ms")).filter(|x| x.is_finite()).fold(f64::MAX, f64::min);
    let mut best: Option<f64> = None;
    let judged: Vec<serde_json::Value> = results.iter().map(|r| {
        let (t, a) = (f(r, "target_mbps"), f(r, "actual_mbps"));
        let mut why = vec![];
        if !(f(r, "client_fps") >= 0.95 * fps) { why.push("headset fps dropped"); }
        if !(f(r, "packets_lost_per_s") <= 1.0) { why.push("packet loss"); }
        if !(f(r, "network_ms") <= min_net + 4.0) { why.push("network latency rising"); }
        if !(a >= 0.8 * t) { why.push("encoder could not reach the bitrate"); }
        if why.is_empty() && best.map_or(true, |b| t > b) {
            best = Some(t);
        }
        json!({ "target_mbps": t, "ok": why.is_empty(), "why": why })
    }).collect();
    json!({
        "steps": judged,
        "best_ok_mbps": best,
        "recommended_mbps": best.map(|b| (b * 0.9 / 10.0).round() * 10.0),
        "note": if best.is_none() { "no step passed: check the Wi-Fi link (5/6 GHz, close to the router) and try lower bitrates" } else { "" },
    })
}

/// The user's display gamma and the debug session folder, for the shim.
fn push_user(ipc: *mut std::ffi::c_void, gamma: f32, debug: bool) {
    #[cfg(windows)]
    let dir: Vec<u16> = {
        use std::os::windows::ffi::OsStrExt;
        logs::debug_dir().map(|d| d.as_os_str().encode_wide().chain(std::iter::once(0)).collect()).unwrap_or_else(|| vec![0])
    };
    #[cfg(not(windows))]
    let dir: Vec<u16> = vec![0];
    unsafe { nvh::nvh_ipc_set_user(ipc, gamma, debug as i32, dir.as_ptr()) };
}

/// Per-frame histories in --daemon mode: past this many entries the oldest half is dropped. Returns how many were dropped.
const HISTORY_CAP: usize = 1 << 18;
fn bound_history<T>(v: &mut Vec<T>) -> usize {
    if v.len() > HISTORY_CAP {
        let n = v.len() / 2;
        v.drain(..n);
        n
    } else {
        0
    }
}

/// An encode failed. In --daemon mode (the installed host) a transient failure, e.g. a shim ring that was just recreated by a new
/// app, must not end the process: drop the frame, reopen the shared slots, restart from an IDR. Gives up after 30 in a row.
/// Returns false when the caller should stop.
fn tolerate_encode_error(origin: Instant, args: &Args, shared: &Shared, enc: *mut std::ffi::c_void, msg: &str, total: &mut usize, consecutive: &mut usize) -> bool {
    *total += 1;
    *consecutive += 1;
    event(origin, "encode_error", json!({ "message": msg, "consecutive": *consecutive, "total": *total }));
    if *consecutive == 1 || *consecutive >= 30 {
        logs::general("ERROR", &format!("encode failed ({} in a row): {msg}", *consecutive));
    }
    if !args.daemon || *consecutive >= 30 {
        return false;
    }
    unsafe { nvh::nvh_release_shared(enc) };
    shared.restart_from_idr.store(true, Ordering::SeqCst);
    true
}

/// The headset's last views (FoV tangents, eye offsets), kept across runs so an OpenXR app can start before this connection's
/// ViewsConfig arrives (the visionOS client sends it after its first decoded frame).
fn load_views(path: &std::path::Path) -> Option<([f32; 8], [f32; 6])> {
    let v: serde_json::Value = serde_json::from_str(&fs::read_to_string(path).ok()?).ok()?;
    let arr = |k: &str| -> Option<Vec<f32>> { v[k].as_array()?.iter().map(|x| x.as_f64().map(|f| f as f32)).collect() };
    Some((arr("fov_tan")?.try_into().ok()?, arr("eye_offsets")?.try_into().ok()?))
}

fn save_views(path: &std::path::Path, v: &([f32; 8], [f32; 6])) {
    fs::write(path, json!({ "fov_tan": v.0, "eye_offsets": v.1 }).to_string()).ok();
}

/// `session_settings.audio.game_audio` as ALVR will use it (server_core captures and streams it by itself).
fn game_audio_config(session_path: &std::path::Path) -> serde_json::Value {
    let s: serde_json::Value = fs::read_to_string(session_path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
    let g = &s["session_settings"]["audio"]["game_audio"];
    json!({
        "enabled": g["enabled"].as_bool(),
        "device": if g["content"]["device"]["set"].as_bool() == Some(true) { g["content"]["device"]["content"].clone() } else { json!("windows default output") },
        "mute_when_streaming": g["content"]["mute_when_streaming"].as_bool(),
        "windows_default_id": default_playback_device_id(),
    })
}

#[cfg(windows)]
fn default_playback_device_id() -> Option<String> {
    alvr_audio::AudioDevice::new_output(None).and_then(|d| alvr_audio::get_windows_device_id(&d)).ok()
}
#[cfg(not(windows))]
fn default_playback_device_id() -> Option<String> {
    None
}

#[derive(Default)]
struct Shared {
    /// views (fov tangents, eye offsets) from the latest ViewsConfig event, applied by the main loop (which owns the encoder)
    pending_views: Mutex<Option<([f32; 8], [f32; 6])>>,
    /// ClientConnected seen, not yet handled by the main loop (which checks the encoder against the negotiated stream)
    connect_pending: AtomicBool,
    /// the encoder matches the current connection's negotiated stream: frames may be sent. Deliberately NOT tied to the
    /// headset's ViewsConfig: the stock visionOS client sends its views only after it has decoded a first frame
    stream_ready: AtomicBool,
    audio_device_events: AtomicUsize,
    /// (hostname, name, ip) of the connected headset
    headset: Mutex<Option<(String, String, String)>>,
    latest_sample_ts: Mutex<Option<Duration>>,
    client_connected: AtomicBool,
    restart_from_idr: AtomicBool,
    tracking_events: AtomicUsize,
    idr_requests: AtomicUsize,
    frames_sent: AtomicUsize,
    bytes_sent: AtomicU64,
    config_sent: AtomicUsize,
}

fn main() {
    let mut args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(3);
        }
    };
    let origin = Instant::now();
    logs::set_clock(origin, unsafe { nvh::nvh_qpc_seconds() });
    let gpu_info: serde_json::Value = {
        let mut b = vec![0 as std::ffi::c_char; 1024];
        unsafe { nvh::nvh_gpu_info(b.as_mut_ptr(), b.len() as i32) };
        serde_json::from_str(&unsafe { std::ffi::CStr::from_ptr(b.as_ptr()) }.to_string_lossy()).unwrap_or(json!({}))
    };
    if args.system_check {
        let v = vision::system_check(gpu_info);
        println!("{}", json!({ "event": "system_check", "data": v }));
        std::process::exit(if v["ok"] == true { 0 } else { 6 });
    }
    let install = args.install_dir.as_deref().map(vision::Install::new);
    let mut seeded = false;
    if let Some(i) = &install {
        args.config_dir = i.config.clone();
        fs::create_dir_all(&i.config).ok();
        // an explicit --session (seeded below) wins over the shipped default
        seeded = args.session.is_none() && vision::seed_session(&i.config);
        let st = vision::load_settings(&i.config.join("visionalvr.json"));
        for (k, v) in st.color.iter().enumerate() {
            vision::COLOR_REQ[k].store(v.to_bits(), Ordering::SeqCst);
        }
        vision::COLOR_PENDING.store(true, Ordering::SeqCst);
        apply_settings(&mut args, &st);
    } else {
        apply_settings(&mut args, &vision::Settings::default());
    }
    fs::create_dir_all(&args.config_dir).expect("create config dir");
    let layout = alvr_filesystem::Layout::new(&args.config_dir);
    let logs_dir = install.as_ref().map(|i| i.logs.clone()).or(args.logs_dir.clone());
    let alvr_log = match &install {
        Some(i) => i.logs.join("alvr.log"),
        None => layout.session_log(),
    };
    fs::create_dir_all(alvr_log.parent().unwrap_or(&args.config_dir)).ok();
    logs::init(logs_dir.as_deref(), Some(&alvr_log), args.debug == Some(true));
    let mut debug_on = args.debug == Some(true);
    let mut user_gamma = args.gamma.unwrap_or(1.0).clamp(0.5, 2.0);
    event(origin, "host_start", json!({ "version": vision::VERSION, "alvr": env!("CARGO_PKG_VERSION"), "args": std::env::args().skip(1).collect::<Vec<_>>(),
        "install_dir": args.install_dir, "debug_dir": logs::debug_dir(), "gpu": gpu_info }));
    logs::general("INFO", &format!(
        "VisionALVR {} host start ({}) | GPU {} ({}, driver {}) | profile {:?} | debug {}{}",
        vision::VERSION, args.install_dir.as_ref().map(|d| d.display().to_string()).unwrap_or_else(|| args.config_dir.display().to_string()),
        gpu_info["name"].as_str().unwrap_or("?"), gpu_info["arch_name"].as_str().unwrap_or("?"), gpu_info["driver"].as_str().unwrap_or("?"),
        args.encode_profile, if debug_on { "on: " } else { "off" },
        logs::debug_dir().map(|d| d.display().to_string()).unwrap_or_default(),
    ));
    if seeded {
        logs::general("INFO", "first start: config/session.json created from config/session.default.json");
    }
    if args.gui {
        vision::spawn_stdin_control();
    }
    vision::spawn_gpu_sampler(|| {
        let mut b = vec![0 as std::ffi::c_char; 256];
        (unsafe { nvh::nvh_gpu_sample(b.as_mut_ptr(), b.len() as i32) } > 0)
            .then(|| unsafe { std::ffi::CStr::from_ptr(b.as_ptr()) }.to_string_lossy().to_string())
    });
    if let Some(s) = &args.session {
        // The session also holds ALVR's trusted-client list and the user's edits: only seed it, never overwrite.
        let dest = args.config_dir.join("session.json");
        if !dest.exists() || args.force_session {
            if let Err(e) = fs::copy(s, &dest) {
                setup_fail(origin, &format!("cannot copy the session {} to {}: {e}", s.display(), dest.display()));
            }
        }
    }
    if args.live || args.bench_quality {
        if let Err(m) = check_session(&args.config_dir.join("session.json"), install.is_some()) {
            setup_fail(origin, &m);
        }
    }
    if args.live {
        // encoder parameters are fixed (NVIDIA profile); log what differed from the session so nothing is overridden silently
        match nvidia_profile::apply_to_session(&args.config_dir.join("session.json")) {
            Ok(changed) => event(origin, "nvidia_profile", json!({ "overrides": changed })),
            Err(e) => {
                eprintln!("cannot apply NVIDIA profile to session.json: {e}");
                std::process::exit(3);
            }
        }
        // benchmarks try profiles without changing the user's session (they set the encoder's foveation directly)
        let profile_to_session = if args.bench.is_some() || args.bench_network.is_some() { nvidia_profile::EncodeProfile::Session } else { args.encode_profile };
        match nvidia_profile::apply_encode_profile(&args.config_dir.join("session.json"), profile_to_session) {
            Ok(changed) => event(origin, "encode_profile", json!({
                "profile": format!("{:?}", args.encode_profile), "session_changes": changed,
                "qp_map": args.qp_map, "split_encode_mode": args.split_encode,
            })),
            Err(e) => {
                eprintln!("cannot apply the encode profile to session.json: {e}");
                std::process::exit(3);
            }
        }
    }
    if args.bench_quality {
        std::process::exit(run_quality_bench(origin, &args, &layout, install.as_ref()));
    }
    let (aus, idr_count) = if args.live {
        (vec![], 0)
    } else {
        let stream = fs::read(&args.file).expect("read --file");
        let aus = split_access_units(&stream);
        let idr_count = aus.iter().filter(|a| a.is_idr).count();
        event(origin, "source", json!({ "file": args.file, "bytes": stream.len(), "access_units": aus.len(), "idr": idr_count }));
        if aus.is_empty() || !aus[0].is_idr {
            eprintln!("source must start with an IDR access unit");
            std::process::exit(3);
        }
        (aus, idr_count)
    };

    if install.is_some() {
        for c in vision::refresh_manual_ips(&layout.session()) {
            logs::general("INFO", &format!("headset address updated from ALVR's last connection: {c}"));
        }
        match vision::headset(&layout.session(), false) {
            Some((h, name, ip)) => logs::general("INFO", &format!("paired headset: {name} ({h}) at {ip}")),
            None => logs::general("WARN", "no paired headset in config/session.json: run configure.exe"),
        }
    }
    alvr_server_core::initialize_environment(layout.clone()); // (logging: logs::init above replaces server_core's init_logging)

    let (ctx, events) = ServerCoreContext::new();
    let ctx = Arc::new(ctx);
    let shared = Arc::new(Shared::default());

    // live NVENC encoder (settings from the session's openvr_config, like ALVR's C++ Settings.cpp)
    let mut live: Option<Box<LiveCtx>> = None;
    let mut eye_size = (0i32, 0i32); // per-eye render size the app uses (the shim composes this; the encoder may see less)
    let mut enc_handle: *mut std::ffi::c_void = std::ptr::null_mut();
    let mut enc_size = (0, 0);
    let qp_on = args.qp_map.unwrap_or_else(|| match std::env::var("VISIONALVR_QP_MAP").as_deref() {
        Ok("0") | Ok("off") => false,
        Ok("1") | Ok("on") => true,
        _ => nvidia_profile::QP_MAP_ENABLED,
    });
    let mut nvenc_cfg = json!(null);
    let mut cur_cfg = read_stream_cfg(&layout.session());
    let host_keys = [
        (c"idle_rgb", args.idle_rgb.unwrap_or(0x00FF00) as i64),
        (c"gpu_thread_priority", args.gpu_priority.unwrap_or(0)),
        (c"noise_block", args.noise_block),
        (c"noise_amp", args.noise_amp),
    ];
    let audio_cfg = game_audio_config(&layout.session());
    event(origin, "game_audio", audio_cfg.clone());
    let mut reconfigs = 0usize;
    if args.live {
        let (w, h, session_fps) = configure_encoder(&layout.session(), args.split_encode, qp_on, &host_keys);
        eye_size = (w / 2, h);
        if !args.fps_explicit {
            args.fps = session_fps; // the session's refresh rate (what the client is asked for), not a hard-coded 90
        }
        let benchmarking = args.bench.is_some() || args.bench_network.is_some();
        if benchmarking && args.encode_profile != nvidia_profile::EncodeProfile::Session {
            // no headset negotiated anything yet: the profile decides whether the encoder sees ALVR's foveated layout
            unsafe { nvh::nvh_set(c"enable_foveated_encoding".as_ptr(), (args.encode_profile == nvidia_profile::EncodeProfile::Foveated) as i64) };
        }
        let (w, h) = if args.shim || (benchmarking && args.width.is_none()) {
            // the encoded frame is ALVR's foveated layout when foveation is negotiated, else the full side-by-side frame
            let (mut ew, mut eh) = (0i32, 0i32);
            unsafe { nvh::nvh_encoded_size(&mut ew, &mut eh) };
            (ew, eh)
        } else {
            (args.width.unwrap_or(w), args.height.unwrap_or(h))
        };
        enc_size = (w, h);
        let mut l = Box::new(LiveCtx { ctx: Arc::clone(&ctx), shared: Arc::clone(&shared), packet_bytes: 0, packet_idr: false, packets: 0, slices: 0, slice_samples: 0, report_in_callback: !args.shim, cb_ns: 0, params_ns: 0, bitrate_override: 0, bytes_total: 0, bitrate_sent: 0, fps: args.fps as f32, hold_now: false, pending: vec![] });
        let mut err = [0 as std::ffi::c_char; 256];
        enc_handle = unsafe {
            nvh::nvh_create(w, h, args.noise, on_packet, on_params, &mut *l as *mut LiveCtx as *mut _, err.as_mut_ptr(), 256)
        };
        if enc_handle.is_null() {
            let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy().to_string();
            eprintln!("NVENC init failed: {msg}");
            std::process::exit(4);
        }
        // network benchmark: no game keeps the GPU clocked, and a lightly loaded GPU downclocks NVENC (~3x slower on the laptop)
        let keepalive = args.gpu_keepalive.or(if args.bench_network.is_some() { Some(90) } else { None });
        if let Some(duty) = keepalive {
            unsafe { nvh::nvh_keepalive(enc_handle, 1, duty) };
        }
        event(origin, "encoder", json!({ "width": w, "height": h, "noise": args.noise, "gpu_keepalive_duty_pct": args.gpu_keepalive }));
        nvenc_cfg = describe_encoder(origin, enc_handle, &args.dump_qpmap);
        live = Some(l);
    }
    let mut ipc: *mut std::ffi::c_void = std::ptr::null_mut();
    if args.shim {
        ipc = unsafe { nvh::nvh_ipc_open() };
        if ipc.is_null() {
            eprintln!("cannot open the OVRShim IPC objects");
            std::process::exit(4);
        }
        event(origin, "ipc", json!({ "shim_alive": unsafe { nvh::nvh_ipc_shim_alive(ipc) } }));
    }
    let ipc_addr = ipc as usize;
    if let (Some(n), Some(l)) = (args.bench, live.as_mut()) {
        // encoder-only benchmark: back-to-back frames, nothing connected, at the session's bitrate (or --bench-mbps)
        let mbps = args.bench_mbps.unwrap_or_else(|| {
            fs::read_to_string(layout.session()).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
                .and_then(|s| s["session_settings"]["video"]["bitrate"]["mode"]["ConstantMbps"].as_u64()).unwrap_or(250)
        });
        l.bitrate_override = mbps * 1_000_000;
        let mut v = vec![];
        for i in 0..n {
            let mut err = [0 as std::ffi::c_char; 256];
            let ms = unsafe { nvh::nvh_encode(enc_handle, i as u32, i as u64 * 11_000_000, (i == 0) as i32, err.as_mut_ptr(), 256) };
            if ms < 0.0 {
                println!("{}", json!({ "event": "bench_error", "frame": i, "message": unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy() }));
                std::process::exit(5);
            }
            v.push(ms);
            if args.bench_sleep_ms > 0 {
                thread::sleep(Duration::from_millis(args.bench_sleep_ms));
            }
        }
        let avg = v.iter().sum::<f64>() / v.len() as f64;
        let mut sorted = v[v.len().min(10)..].to_vec(); // the first frames include the encoder's warm-up
        sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
        let q = |p: f64| sorted.get(((sorted.len() as f64) * p) as usize).copied().unwrap_or(0.0);
        let (p50, p95) = (q(0.5), q(0.95));
        let budget = 1000.0 / args.fps;
        let data = json!({ "frames": n, "avg_ms": avg, "p50_ms": p50, "p95_ms": p95, "min_ms": v.iter().cloned().fold(f64::MAX, f64::min),
            "max_ms": v.iter().cloned().fold(0.0, f64::max), "width": enc_size.0, "height": enc_size.1, "profile": format!("{:?}", args.encode_profile),
            "split_encode_mode": args.split_encode, "nvenc_engines": unsafe { nvh::nvh_encoder_engines(enc_handle) },
            "send_pacing": if args.send_on_vsync { "vsync" } else { "asap" },
            "stamp": stamp_name(args.stamp_mode), "fresh_wait_ms": args.fresh_wait_ms,
            "pacing": if args.pacing_tracking { "tracking" } else { "grid" }, "pacing_guard_ms": args.pacing_guard_ms,
            "slices_per_frame": live.as_ref().map(|l| l.slices as f64 / l.slice_samples.max(1) as f64),
            "fps": args.fps, "budget_ms": budget, "fits": p95 < budget * 0.9, "noise": args.noise, "noise_block": args.noise_block, "noise_amp": args.noise_amp,
            "target_mbps": mbps, "produced_mbps_at_fps": live.as_ref().map(|l| l.bytes_total as f64 * 8.0 / n.max(1) as f64 * args.fps / 1e6) });
        println!("{}", json!({ "event": "bench", "data": data }));
        logs::general("INFO", &format!("encoder benchmark {:?} {}x{}: p50 {p50:.1} ms, p95 {p95:.1} ms ({} at {} Hz)", args.encode_profile, enc_size.0, enc_size.1,
            if p95 < budget * 0.9 { "fits" } else { "does NOT fit" }, args.fps));
        std::process::exit(0);
    }
    let mut encode_ms: Vec<f64> = vec![];
    let mut compose_ms: Vec<f64> = vec![];
    let mut ffr_ms: Vec<f64> = vec![];
    let mut ffr_wait_ms: Vec<f64> = vec![];
    let mut frame_rows: Vec<String> = vec![]; // header added when written
    ctx.start_connection();
    event(origin, "listening", json!({ "config_dir": args.config_dir }));

    // event thread
    {
        let (ctx, shared) = (Arc::clone(&ctx), Arc::clone(&shared));
        let session_path = layout.session();
        thread::spawn(move || {
            let ipc = ipc_addr as *mut std::ffi::c_void;
            let mut hands = [HandInputState::default(); 2];
            let mut buttons_seen = 0usize;
            let mut invalid_views = 0usize;
            for ev in events {
                match ev {
                    ServerCoreEvent::ClientConnected => {
                        shared.connect_pending.store(true, Ordering::SeqCst);
                        shared.client_connected.store(true, Ordering::SeqCst);
                        shared.restart_from_idr.store(true, Ordering::SeqCst);
                        let hs = vision::headset(&session_path, true);
                        if let Some((h, name, ip)) = &hs {
                            logs::general("INFO", &format!("headset connected: {name} ({h}) at {ip}"));
                        }
                        *shared.headset.lock() = hs.clone();
                        event(origin, "client_connected", json!(hs.map(|(h, n, ip)| json!({ "hostname": h, "name": n, "ip": ip }))));
                    }
                    ServerCoreEvent::ClientDisconnected => {
                        shared.client_connected.store(false, Ordering::SeqCst);
                        shared.stream_ready.store(false, Ordering::SeqCst);
                        *shared.latest_sample_ts.lock() = None;
                        if !ipc.is_null() {
                            unsafe { nvh::nvh_ipc_set_connected(ipc, 0) };
                        }
                        logs::general("INFO", "headset disconnected");
                        event(origin, "client_disconnected", json!(null));
                    }
                    ServerCoreEvent::Tracking { sample_timestamp } => {
                        let n = shared.tracking_events.fetch_add(1, Ordering::Relaxed);
                        let now_ns = origin.elapsed().as_nanos() as u64;
                        let pll_residual_ms = pll_arrival(Instant::now());
                        if let Some(m) = ctx.get_device_motion(*HEAD_ID, sample_timestamp) {
                            // sample-to-sample head step (orientation in millidegrees, position in mm): the jitter of the pose
                            // stream the frames are placed by (grows with the headset's prediction horizon)
                            if let Ok(mut prev) = LAST_HEAD_POSE.lock() {
                                if let Some((pq, pp)) = *prev {
                                    let d = (m.pose.orientation * pq.inverse()).normalize();
                                    let ang = 2.0 * d.w.clamp(-1.0, 1.0).acos() as f64;
                                    let ang = if ang > std::f64::consts::PI { 2.0 * std::f64::consts::PI - ang } else { ang };
                                    push_bounded(&HEAD_STEP_MDEG, ang.to_degrees() * 1000.0);
                                    push_bounded(&HEAD_STEP_MM, (m.pose.position - pp).length() as f64 * 1000.0);
                                    logs::csv("tracking.csv", "t_arrival,ts_ns,step_mdeg,step_mm,pll_residual_ms",
                                        &format!("{:.6},{},{:.1},{:.3},{:.3}", logs::qpc_now(), sample_timestamp.as_nanos(), ang.to_degrees() * 1000.0,
                                            (m.pose.position - pp).length() as f64 * 1000.0, pll_residual_ms));
                                }
                                *prev = Some((m.pose.orientation, m.pose.position));
                            }
                        }
                        let prev_ns = LAST_TRACKING_NS.swap(now_ns, Ordering::Relaxed);
                        if prev_ns != 0 {
                            push_bounded(&TRACKING_GAP_MS, now_ns.saturating_sub(prev_ns) as f64 / 1e6);
                        }
                        *shared.latest_sample_ts.lock() = Some(sample_timestamp);
                        if !ipc.is_null() {
                            // server_core hands out angular velocity in the device's own frame (what OpenVR wants); OVR/OpenXR report
                            // it in tracking space (VDXR passes it to the app as a base-space velocity): rotate it back
                            let world_av = |m: &alvr_common::DeviceMotion| (m.pose.orientation * m.angular_velocity).to_array();
                            if let Some(m) = ctx.get_device_motion(*HEAD_ID, sample_timestamp) {
                                // Head: pose only, zero velocity, exactly like ALVR's SteamVR driver (Hmd::OnPoseUpdated). The
                                // visionOS client sends a finite-difference velocity of two ARKit poses (noisy); extrapolating
                                // the head with it over ~25 ms made the view bob, and the headset reprojects against the
                                // un-extrapolated pose anyway.
                                let (q, p) = (m.pose.orientation.to_array(), m.pose.position.to_array());
                                let (lv, av) = ([0f32; 3], [0f32; 3]);
                                unsafe { nvh::nvh_ipc_publish_head(ipc, sample_timestamp.as_nanos() as u64, q.as_ptr(), p.as_ptr(), lv.as_ptr(), av.as_ptr()) };
                            }
                            for (side, id) in [(0, *HAND_LEFT_ID), (1, *HAND_RIGHT_ID)] {
                                match ctx.get_device_motion(id, sample_timestamp) {
                                    Some(m) => {
                                        let (q, p) = (m.pose.orientation.to_array(), m.pose.position.to_array());
                                        let (lv, av) = (m.linear_velocity.to_array(), world_av(&m));
                                        unsafe { nvh::nvh_ipc_publish_hand(ipc, side, 1, q.as_ptr(), p.as_ptr(), lv.as_ptr(), av.as_ptr()) };
                                    }
                                    None => unsafe { nvh::nvh_ipc_publish_hand(ipc, side, 0, std::ptr::null(), std::ptr::null(), std::ptr::null(), std::ptr::null()) },
                                }
                            }
                        }
                        if n % 270 == 0 {
                            let head = ctx.get_device_motion(*HEAD_ID, sample_timestamp);
                            event(origin, "tracking", json!({
                                "n": n, "sample_ts_ns": sample_timestamp.as_nanos() as u64,
                                "head_orientation": head.map(|m| m.pose.orientation.to_array()),
                            }));
                        }
                    }
                    ServerCoreEvent::Buttons(entries) => {
                        // after ALVR's own mapping: paths are in the emulated (Quest/Touch) profile
                        for e in &entries {
                            if let Some(info) = BUTTON_INFO.get(&e.path_id) {
                                let side = if info.device_id == *HAND_LEFT_ID { 0 } else { 1 };
                                if let Some(input) = info.path.split("/input/").nth(1) {
                                    let _ = ButtonType::Binary; // (type documented by the path suffix)
                                    hands[side].apply(input, &e.value);
                                }
                            }
                        }
                        buttons_seen += entries.len();
                        if !ipc.is_null() {
                            for side in 0..2 {
                                let h = hands[side];
                                unsafe { nvh::nvh_ipc_publish_input(ipc, side as i32, h.clicks, h.touches, h.trigger, h.squeeze, h.stick_x, h.stick_y) };
                            }
                        }
                        if buttons_seen % 200 < entries.len() {
                            event(origin, "buttons", json!({ "seen": buttons_seen }));
                        }
                    }
                    ServerCoreEvent::RequestIDR => {
                        shared.idr_requests.fetch_add(1, Ordering::Relaxed);
                        shared.restart_from_idr.store(true, Ordering::SeqCst);
                    }
                    ServerCoreEvent::ViewsConfig(cfg) if !ipc.is_null() => {
                        // ALVR FOV is in radians (left/down negative); OVR wants positive tangents.
                        let t = |f: &alvr_common::Fov| [(-f.left).tan(), f.right.tan(), f.up.tan(), (-f.down).tan()];
                        let (l, r) = (t(&cfg.fov[0]), t(&cfg.fov[1]));
                        let fov8 = [l[0], l[1], l[2], l[3], r[0], r[1], r[2], r[3]];
                        if !fov8.iter().all(|x| x.is_finite() && *x > 0.0) {
                            // the visionOS client sends placeholder views (NaN FoV) about 10x/s until the user enters its
                            // immersive space: not a FoV, keep the last good one
                            let n = invalid_views;
                            invalid_views += 1;
                            if n == 0 || n.is_power_of_two() {
                                event(origin, "views_config_ignored", json!({ "count": n + 1, "reason": "non-finite or non-positive FoV" }));
                            }
                            continue;
                        }
                        let (a, b) = (cfg.local_view_transforms[0].position.to_array(), cfg.local_view_transforms[1].position.to_array());
                        let off6 = [a[0], a[1], a[2], b[0], b[1], b[2]];
                        // the main loop applies it: it may have to rebuild the encoder for the negotiated stream parameters first
                        *shared.pending_views.lock() = Some((fov8, off6));
                        event(origin, "views_config", json!({ "fov_tan": fov8, "eye_offsets": off6 }));
                    }
                    ServerCoreEvent::ViewsConfig(cfg) => event(origin, "views_config", json!({
                        "ipd_m": cfg.local_view_transforms[1].position.x - cfg.local_view_transforms[0].position.x,
                        "fov": cfg.fov.iter().map(|f| [f.left, f.right, f.up, f.down]).collect::<Vec<_>>(),
                    })),
                    ServerCoreEvent::SetOpenvrProperty { prop, .. }
                        if matches!(prop.key, alvr_session::OpenvrPropKey::AudioDefaultPlaybackDeviceIdString) =>
                    {
                        // server_core's game-audio thread names the playback device it loop-back captures (and the default device
                        // again when it stops). SteamVR would make that device the Windows default; nothing does here, so games
                        // must already play to it: warn if it is not the default (a configured `game_audio.device`).
                        let default_id = default_playback_device_id();
                        let n = shared.audio_device_events.fetch_add(1, Ordering::Relaxed);
                        event(origin, "audio_device", json!({
                            "n": n, "device_id": prop.value, "windows_default_id": default_id,
                            "is_windows_default": default_id.as_deref() == Some(prop.value.as_str()),
                        }));
                    }
                    ServerCoreEvent::ShutdownPending | ServerCoreEvent::RestartPending => {
                        event(origin, "shutdown_pending", json!(null))
                    }
                    _ => {}
                }
            }
        });
    }

    // Display clock: its own thread, so a long encode can never delay a vsync tick (the OpenXR app waits on it). Ticks follow ALVR's
    // pacing grid (`duration_until_next_vsync`); the interval to the next tick is published so the shim can predict display times.
    let vsync_ticks = Arc::new(AtomicU64::new(0));
    let vsync_period_ns = Arc::new(AtomicU64::new((1e9 / args.fps) as u64));
    let vsync_stop = Arc::new(AtomicBool::new(false));
    let vsync_thread = if args.shim {
        let (ctx, ticks, period_ns, stop) = (Arc::clone(&ctx), Arc::clone(&vsync_ticks), Arc::clone(&vsync_period_ns), Arc::clone(&vsync_stop));
        let daemon = args.daemon;
        let (pacing_tracking, guard_ns) = (args.pacing_tracking, args.pacing_guard_ms * 1e6);
        Some(thread::spawn(move || {
            #[cfg(windows)]
            unsafe {
                SetThreadPriority(GetCurrentThread(), 2); // THREAD_PRIORITY_HIGHEST
            }
            let ipc = ipc_addr as *mut std::ffi::c_void;
            let mut intervals: Vec<f64> = vec![];
            let mut last: Option<Instant> = None;
            let mut next = Instant::now();
            while !stop.load(Ordering::Relaxed) {
                // sleep to just before the tick, spin the rest (a plain sleep is only ms-accurate)
                loop {
                    let left = next.saturating_duration_since(Instant::now());
                    if left.is_zero() || stop.load(Ordering::Relaxed) {
                        break;
                    }
                    if left > Duration::from_micros(1500) {
                        thread::sleep(left - Duration::from_micros(1000));
                    } else {
                        std::hint::spin_loop();
                    }
                }
                let now = Instant::now();
                let nominal = Duration::from_nanos(period_ns.load(Ordering::Relaxed));
                // ALVR measures "until the next grid point" from its own, slightly later, Instant::now(); waking at now + interval
                // therefore lands a few microseconds BEFORE the grid point, and ALVR then answers "a few microseconds". Clamping
                // that to 0.5 ms produced a second tick 0.5 ms later on ~15-20% of ticks (~107 Hz instead of 90: apps ran at
                // 97-111 fps in the first headset test). A remainder under half a period means "this grid point": aim at the next.
                let (interval, period) = match if pacing_tracking { pll_next_tick(now, guard_ns) } else { None } {
                    // headset-locked grid: the tick sits `guard` after the estimated arrival of the headset's tracking packet
                    Some((tick, period)) => (tick.saturating_duration_since(now), period),
                    None => {
                        let mut interval = ctx.duration_until_next_vsync().unwrap_or(nominal);
                        if interval < nominal / 2 {
                            interval += nominal;
                        }
                        (interval, nominal)
                    }
                };
                if pacing_tracking {
                    if let Ok(pll) = PLL.lock() {
                        if let Some(l) = pll.last {
                            push_bounded(&TICK_AFTER_TRACKING_MS, now.saturating_duration_since(l).as_secs_f64() * 1000.0);
                        }
                    }
                }
                let nominal = period; // the tick spacing this grid runs at (the headset's period when locked)
                LAST_TICK_NS.store(now.duration_since(origin).as_nanos() as u64, Ordering::Relaxed);
                NEXT_TICK_NS.store((now + interval).duration_since(origin).as_nanos() as u64, Ordering::Relaxed);
                LAST_TICK_QPC.store(logs::qpc_at(now).to_bits(), Ordering::Relaxed);
                unsafe { nvh::nvh_ipc_vsync(ipc, interval.as_secs_f64()) };
                ticks.fetch_add(1, Ordering::Relaxed);
                VSYNC_TICKS.fetch_add(1, Ordering::Relaxed);
                if let Some(l) = last {
                    let ms = now.duration_since(l).as_secs_f64() * 1000.0;
                    if ms > 1.8 * nominal.as_secs_f64() * 1000.0 {
                        event(origin, "vsync_late", json!({ "interval_ms": ms, "nominal_ms": nominal.as_secs_f64() * 1000.0 }));
                    }
                    intervals.push(ms);
                    if daemon {
                        bound_history(&mut intervals);
                    }
                }
                last = Some(now);
                next = now + interval;
            }
            intervals
        }))
    } else {
        None
    };
    let mut last_green_tick = 0u64;
    let mut last_sent_ts = Duration::ZERO;
    let mut stats_win = StatsWindow::new();
    let mut intake_ms: Vec<f64> = vec![];
    let mut last_intake: Option<Instant> = None;

    // sender (the "compositor + encoder" stand-in)
    let period = Duration::from_secs_f64(1.0 / args.fps);
    let mut idx = 0usize;
    let mut next = Instant::now();
    let mut connected_at: Option<Instant> = None;
    let mut dropped_no_ts = 0usize;
    #[derive(Clone, Copy)]
    struct HapticState {
        seq: u32,
        freq: f32,
        amp: f32,
        last_change: Instant,
        last_send: Instant,
    }
    let mut haptic_state = [HapticState { seq: 0, freq: 0.0, amp: 0.0, last_change: Instant::now() - Duration::from_secs(10), last_send: Instant::now() - Duration::from_secs(10) }; 2];
    let mut haptics_sent = 0usize;
    let mut green_active = false;
    let mut green_frames = 0usize;
    let min_idr_interval = Duration::from_millis(100);
    let mut last_idr_restart = Instant::now() - min_idr_interval;
    let mut idr_restarts_honoured = 0usize;
    let mut sent_ts: Vec<u64> = vec![];
    let views_path = args.config_dir.join("visionalvr_views.json");
    let mut last_views = load_views(&views_path);
    let mut views_published = false;
    let mut deferred_reconfig = false;
    let (mut encode_errors, mut consecutive_errors) = (0usize, 0usize);
    if !ipc.is_null() {
        push_user(ipc, user_gamma, debug_on);
        unsafe { nvh::nvh_ipc_set_pacing(ipc, args.stamp_mode, args.fresh_wait_ms as f32, args.render_scale as f32) };
        event(origin, "pacing_config", json!({ "stamp": stamp_name(args.stamp_mode), "fresh_wait_ms": args.fresh_wait_ms, "render_scale": args.render_scale,
            "pacing": if args.pacing_tracking { "tracking" } else { "grid" }, "pacing_guard_ms": args.pacing_guard_ms,
            "send_pacing": if args.send_on_vsync { "vsync" } else { "asap" } }));
        logs::general("INFO", &format!("frame stamping: {} (fresh wait {:.1} ms); display clock: {} (guard {:.1} ms); send pacing: {}",
            match args.stamp_mode { 2 => "newest sample + rotation + pose extrapolation", 1 => "newest sample + rotation", _ => "rendered sample (pose match)" }, args.fresh_wait_ms,
            if args.pacing_tracking { "locked to headset tracking" } else { "ALVR grid" }, args.pacing_guard_ms,
            if args.send_on_vsync { "vsync" } else { "asap" }));
    }
    struct GameRun {
        exe: String,
        start: Instant,
        frames0: usize,
        submitted0: u32,
    }
    let mut game: Option<GameRun> = None;
    let shim_submitted = |ipc: *mut std::ffi::c_void| -> u32 {
        let (mut m, mut a, mut b) = (0u32, 0u32, 0u32);
        if !ipc.is_null() {
            unsafe { nvh::nvh_ipc_shim_stats(ipc, &mut m, &mut a, &mut b) };
        }
        a.wrapping_add(b)
    };
    let mut status_at = Instant::now();
    struct BenchState {
        i: usize,
        start: Instant,
        measuring: bool,
        mstart: Instant,
        bytes0: u64,
        lost0: usize,
        frames0: usize,
        results: Vec<serde_json::Value>,
    }
    let mut bench: Option<BenchState> = None;
    let (mut st_frames, mut st_bytes, mut st_submitted, mut st_lost) = (0usize, 0u64, 0u32, 0usize);
    loop {
        if vision::QUIT.load(Ordering::SeqCst) {
            logs::general("INFO", "stop requested (VisionALVR.exe closed or sent quit)");
            break;
        }
        if !ipc.is_null() {
            // live settings from VisionALVR.exe
            if vision::COLOR_PENDING.swap(false, Ordering::SeqCst) {
                let c = [0, 1, 2, 3].map(|k| f32::from_bits(vision::COLOR_REQ[k].load(Ordering::SeqCst)));
                unsafe { nvh::nvh_ipc_set_color(ipc, c[0], c[1], c[2], c[3]) };
                event(origin, "color", json!({ "brightness": c[0], "contrast": c[1], "saturation": c[2], "sharpening": c[3] }));
            }
            let g = vision::GAMMA_REQ.swap(0, Ordering::SeqCst);
            let d = vision::DEBUG_REQ.swap(-1, Ordering::SeqCst);
            if g != 0 || d >= 0 {
                if g != 0 {
                    user_gamma = f32::from_bits(g);
                }
                if d >= 0 && (d == 1) != debug_on {
                    debug_on = d == 1;
                    logs::set_debug(debug_on);
                    logs::general("INFO", &format!("debug logging {}", logs::debug_dir().map(|p| format!("on: {}", p.display())).unwrap_or_else(|| "off".into())));
                }
                push_user(ipc, user_gamma, debug_on);
                event(origin, "user_settings", json!({ "gamma": user_gamma, "debug": debug_on, "debug_dir": logs::debug_dir() }));
            }
            // game start / end, for the general log and the GUI
            let alive = unsafe { nvh::nvh_ipc_shim_alive(ipc) } != 0;
            if alive && game.is_none() {
                let mut b = [0 as std::ffi::c_char; 256];
                unsafe { nvh::nvh_ipc_app_exe(ipc, b.as_mut_ptr(), b.len() as i32) };
                let exe = unsafe { std::ffi::CStr::from_ptr(b.as_ptr()) }.to_string_lossy().to_string();
                let exe = if exe.is_empty() { "(unknown app)".to_string() } else { exe };
                logs::general("INFO", &format!("game started: {exe}"));
                event(origin, "game_started", json!({ "exe": exe }));
                game = Some(GameRun { exe, start: Instant::now(), frames0: shared.frames_sent.load(Ordering::Relaxed), submitted0: shim_submitted(ipc) });
            } else if !alive && game.is_some() {
                let g = game.take().unwrap();
                let secs = g.start.elapsed().as_secs_f64();
                let frames = shared.frames_sent.load(Ordering::Relaxed) - g.frames0;
                let submitted = shim_submitted(ipc).wrapping_sub(g.submitted0);
                logs::general("INFO", &format!(
                    "game ended: {} after {}m{:02}s, {} frames rendered ({:.1} fps), {} streamed ({:.1} fps)",
                    g.exe, secs as u64 / 60, secs as u64 % 60, submitted, submitted as f64 / secs.max(1e-3), frames, frames as f64 / secs.max(1e-3)));
                event(origin, "game_ended", json!({ "exe": g.exe, "seconds": secs, "frames_rendered": submitted, "frames_streamed": frames }));
            }
        }
        if status_at.elapsed() >= Duration::from_secs(10) {
            // every 10 s for VisionALVR.exe's status lines (the logs have the 2 s detail)
            let secs = status_at.elapsed().as_secs_f64();
            let (frames, bytes, submitted, lost) = (shared.frames_sent.load(Ordering::Relaxed), shared.bytes_sent.load(Ordering::Relaxed),
                shim_submitted(ipc), logs::client_stats::packets_lost_total());
            let rendered = submitted.wrapping_sub(st_submitted) as usize;
            let streamed = frames - st_frames;
            event(origin, "status", json!({
                "connected": shared.client_connected.load(Ordering::SeqCst),
                "headset": shared.headset.lock().clone().map(|(h, n, ip)| json!({ "hostname": h, "name": n, "ip": ip })),
                "game": game.as_ref().map(|g| g.exe.clone()),
                "app_fps": rendered as f64 / secs, "stream_fps": streamed as f64 / secs,
                "frames_not_streamed": rendered.saturating_sub(streamed),
                "mbps": (bytes - st_bytes) as f64 * 8.0 / secs / 1e6,
                "packets_lost": lost.saturating_sub(st_lost),
                "client": logs::client_stats::take(1),
                "gamma": user_gamma, "debug": debug_on,
            }));
            (st_frames, st_bytes, st_submitted, st_lost) = (frames, bytes, submitted, lost);
            status_at = Instant::now();
        }
        if args.shim {
            // Connect: ALVR persisted the negotiated stream (openvr_config) before it reported ClientConnected, so the encoder is
            // checked against it right here, before any frame. Frames do NOT wait for the headset's ViewsConfig: the stock
            // visionOS client sends its views only after it has decoded a first frame (when it is already in its immersive
            // space), so waiting for them would deadlock. Views only feed the shim (FoV/IPD for the OpenXR app).
            let connect = shared.connect_pending.swap(false, Ordering::SeqCst);
            if connect {
                views_published = false;
            }
            if connect || (deferred_reconfig && unsafe { nvh::nvh_ipc_shim_alive(ipc) } == 0) {
                // Rebuild the encoder if the negotiated stream (resolution, refresh rate, foveation) differs from what it was built
                // from (a stale `openvr_config` cache). Not while an app runs: its swapchain/ring sizes are fixed; then it is
                // retried as soon as the app has exited.
                let want = read_stream_cfg(&layout.session());
                if want == cur_cfg {
                    deferred_reconfig = false;
                } else if unsafe { nvh::nvh_ipc_shim_alive(ipc) } != 0 {
                    if !deferred_reconfig {
                        logs::general("WARN", "the headset negotiated a different stream while a game runs: the encoder is rebuilt when the game exits (restart the game)");
                        event(origin, "reconfigure_deferred", json!({ "running": format!("{cur_cfg:?}"), "negotiated": format!("{want:?}") }));
                    }
                    deferred_reconfig = true;
                } else {
                    unsafe { nvh::nvh_destroy(enc_handle) };
                    let (w, h, session_fps) = configure_encoder(&layout.session(), args.split_encode, qp_on, &host_keys);
                    eye_size = (w / 2, h);
                    if !args.fps_explicit {
                        args.fps = session_fps;
                    }
                    let (mut ew, mut eh) = (0i32, 0i32);
                    unsafe { nvh::nvh_encoded_size(&mut ew, &mut eh) };
                    let user = &mut **live.as_mut().unwrap() as *mut LiveCtx as *mut std::ffi::c_void;
                    let mut err = [0 as std::ffi::c_char; 256];
                    enc_handle = unsafe { nvh::nvh_create(ew, eh, args.noise, on_packet, on_params, user, err.as_mut_ptr(), 256) };
                    if enc_handle.is_null() {
                        eprintln!("NVENC re-init failed: {}", unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy());
                        std::process::exit(4);
                    }
                    if let Some(duty) = args.gpu_keepalive {
                        unsafe { nvh::nvh_keepalive(enc_handle, 1, duty) };
                    }
                    nvenc_cfg = describe_encoder(origin, enc_handle, &args.dump_qpmap);
                    logs::general("INFO", &format!("encoder rebuilt for the negotiated stream: {ew}x{eh} (was {}x{})", enc_size.0, enc_size.1));
                    event(origin, "reconfigured", json!({
                        "from": format!("{cur_cfg:?}"), "to": format!("{want:?}"),
                        "encoder": [ew, eh], "was": [enc_size.0, enc_size.1], "was_deferred": deferred_reconfig,
                    }));
                    enc_size = (ew, eh);
                    vsync_period_ns.store((1e9 / args.fps) as u64, Ordering::Relaxed);
                    cur_cfg = want;
                    reconfigs += 1;
                    idx = 0;
                    deferred_reconfig = false;
                    views_published = false; // new eye size for the shim
                }
                if connect {
                    logs::general("INFO", &vision::stream_summary(&layout.session(), enc_size));
                    shared.stream_ready.store(true, Ordering::SeqCst);
                }
            }
            if let Some(v) = shared.pending_views.lock().take() {
                last_views = Some(v);
                save_views(&views_path, &v);
                views_published = false;
            }
            if !views_published && shared.stream_ready.load(Ordering::SeqCst) {
                // the shim presents the device to an OpenXR app only once it has the FoV: this connection's views, or the ones
                // the same headset sent last time (they arrive again right after the first decoded frame)
                if let Some((fov8, off6)) = last_views {
                    unsafe {
                        nvh::nvh_ipc_config(ipc, enc_handle, args.fps, eye_size.0, eye_size.1, fov8.as_ptr(), off6.as_ptr(), 10, read_encoding_gamma(&layout.session()));
                        nvh::nvh_ipc_set_pacing(ipc, args.stamp_mode, args.fresh_wait_ms as f32, args.render_scale as f32);
                        nvh::nvh_ipc_set_connected(ipc, 1); // after the config: the shim waits for this flag
                    }
                    views_published = true;
                }
            }
        }
        let connected = shared.client_connected.load(Ordering::SeqCst);
        if connected && connected_at.is_none() {
            connected_at = Some(Instant::now());
        }
        if !args.daemon {
            if !connected && connected_at.is_none() && origin.elapsed().as_secs_f64() > args.connect_timeout {
                break;
            }
            if let Some(t) = connected_at {
                if t.elapsed().as_secs_f64() > args.seconds {
                    break;
                }
            }
        }
        if let Some(n) = args.exit_after_frames {
            if shared.frames_sent.load(Ordering::Relaxed) >= n {
                break;
            }
        }

        if args.shim {
            // Display clock + frame intake. The shim (inside the OpenXR app) waits on our vsync and posts composed
            // side-by-side frames; each one is encoded as soon as it arrives.
            // App vibration (ovr_SetControllerVibration) -> ALVR haptic pulses for the headset's controllers.
            for side in 0..2usize {
                let (mut seq, mut freq, mut amp) = (0u32, 0f32, 0f32);
                unsafe { nvh::nvh_ipc_poll_haptic(ipc, side as i32, &mut seq, &mut freq, &mut amp) };
                let st = &mut haptic_state[side];
                if seq != st.seq {
                    st.seq = seq;
                    st.last_change = Instant::now();
                    st.freq = freq;
                    st.amp = amp;
                }
                let active = st.amp > 0.0 && st.last_change.elapsed() < Duration::from_millis(150);
                if active && connected && st.last_send.elapsed() >= Duration::from_millis(30) {
                    st.last_send = Instant::now();
                    haptics_sent += 1;
                    ctx.send_haptics(Haptics {
                        device_id: if side == 0 { *HAND_LEFT_ID } else { *HAND_RIGHT_ID },
                        duration: Duration::from_millis(40),
                        frequency: if st.freq > 1.0 { st.freq } else if st.freq > 0.0 { st.freq * 320.0 } else { 160.0 }, // VDXR passes Hz; OVR docs say 0..1
                        amplitude: st.amp,
                    });
                }
            }
            if !connected || !shared.stream_ready.load(Ordering::SeqCst) {
                thread::sleep(Duration::from_millis(2));
                continue;
            }
            let wait_ms = 4; // frames wake the wait; the timeout only bounds the haptics/views polling latency
            let app_alive = unsafe { nvh::nvh_ipc_shim_alive(ipc) } != 0;
            if app_alive {
                green_active = false;
            }
            let (mut counter, mut slot, mut cts, mut submit_s) = (0u64, 0u32, 0u64, 0f64);
            if unsafe { nvh::nvh_ipc_wait_frame(ipc, wait_ms, &mut counter, &mut slot, &mut cts, &mut submit_s) } == 0 {
                // No app frame. With no OpenXR app running, keep the headset fed with pure green (it chroma-keys green, so
                // the user sees through). If an app is alive but stalled, send nothing: the headset re-presents its last frame.
                let tick = vsync_ticks.load(Ordering::Relaxed);
                if !app_alive && tick.wrapping_sub(last_green_tick) >= (args.fps / 30.0).round().max(1.0) as u64 {
                    last_green_tick = tick;
                    last_intake = None;
                    if !green_active {
                        unsafe { nvh::nvh_release_shared(enc_handle) }; // the app's ring is gone: free its VRAM
                    }
                    green_active = true;
                    // Green frames carry no pose, so stamp them clearly in the past: the first app frame is stamped with the pose it
                    // was rendered with, which is a few ms older than the freshest tracking sample, and timestamps must not go backwards.
                    let ts = shared.latest_sample_ts.lock().map(|t| t.saturating_sub(Duration::from_millis(50)))
                        .unwrap_or_else(|| connected_at.map(|t| t.elapsed()).unwrap_or_default())
                        .max(last_sent_ts); // ...but never behind the last frame sent (an app that just ended)
                    last_sent_ts = ts;
                    if shared.restart_from_idr.load(Ordering::SeqCst) && last_idr_restart.elapsed() >= min_idr_interval {
                        shared.restart_from_idr.store(false, Ordering::SeqCst);
                        last_idr_restart = Instant::now();
                        idr_restarts_honoured += 1;
                        idx = 0;
                    }
                    let l = live.as_mut().unwrap();
                    let force_idr = idx == 0;
                    idx += 1;
                    l.packet_bytes = 0;
                    l.packet_idr = false;
                    ctx.report_present(ts, Duration::ZERO);
                    ctx.report_composed(ts, Duration::ZERO);
                    let mut err = [0 as std::ffi::c_char; 256];
                    let ms = unsafe { nvh::nvh_encode_green(enc_handle, 0, ts.as_nanos() as u64, force_idr as i32, err.as_mut_ptr(), 256) };
                    if ms < 0.0 {
                        let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy().to_string();
                        if !tolerate_encode_error(origin, &args, &shared, enc_handle, &msg, &mut encode_errors, &mut consecutive_errors) {
                            eprintln!("green encode failed: {msg}");
                            break;
                        }
                        continue;
                    }
                    consecutive_errors = 0;
                    green_frames += 1;
                    frame_rows.push(format!("-1,{},{},{},{ms:.2}", ts.as_nanos(), l.packet_bytes, l.packet_idr as u8));
                    shared.bytes_sent.fetch_add(l.packet_bytes as u64, Ordering::Relaxed);
                }
                continue;
            }
            let intake = Instant::now();
            if let Some(l) = last_intake {
                intake_ms.push(intake.duration_since(l).as_secs_f64() * 1000.0);
            }
            last_intake = Some(intake);
            let ts = if cts != 0 {
                Duration::from_nanos(cts)
            } else {
                dropped_no_ts += 1;
                shared.latest_sample_ts.lock().unwrap_or_default()
            };
            if shared.restart_from_idr.load(Ordering::SeqCst) && last_idr_restart.elapsed() >= min_idr_interval {
                shared.restart_from_idr.store(false, Ordering::SeqCst);
                last_idr_restart = Instant::now();
                idr_restarts_honoured += 1;
                idx = 0;
            }
            if ts != Duration::ZERO && ts == last_sent_ts {
                SAME_TS_FRAMES.fetch_add(1, Ordering::Relaxed);
            }
            last_sent_ts = last_sent_ts.max(ts);
            let l = live.as_mut().unwrap();
            let force_idr = idx == 0;
            idx += 1;
            l.packet_bytes = 0;
            l.packet_idr = false;
            l.pending.clear();
            l.hold_now = args.send_on_vsync;
            let frame_no = shared.frames_sent.load(Ordering::Relaxed);
            // Real timings for ALVR's statistics: the app presented (submitted) the frame at submit_s, the shim finished
            // composing it then, and the encoder starts now.
            let since_submit = Duration::from_secs_f64((unsafe { nvh::nvh_qpc_seconds() } - submit_s).max(0.0));
            ctx.report_present(ts, since_submit);
            ctx.report_composed(ts, since_submit);
            let mut err = [0 as std::ffi::c_char; 256];
            let ms = unsafe { nvh::nvh_encode_shared(enc_handle, ipc, slot, counter as u32, ts.as_nanos() as u64, force_idr as i32, err.as_mut_ptr(), 256) };
            l.hold_now = false;
            if ms < 0.0 {
                l.pending.clear();
                let msg = unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy().to_string();
                if !tolerate_encode_error(origin, &args, &shared, enc_handle, &msg, &mut encode_errors, &mut consecutive_errors) {
                    eprintln!("encode failed: {msg}");
                    break;
                }
                continue;
            }
            if args.send_on_vsync {
                // Send on the display clock, like ALVR's SteamVR driver (the compositor presents at vsync and the driver sleeps
                // until the next grid point after each Present): the headset then sees a regular cadence instead of
                // tick + render time + encode time. A frame that is already past the tick goes out at once.
                let next = origin + Duration::from_nanos(NEXT_TICK_NS.load(Ordering::Relaxed));
                let t0 = Instant::now();
                if next > t0 {
                    sleep_spin_until(next);
                    push_bounded(&HOLD_MS, next.duration_since(t0).as_secs_f64() * 1000.0);
                } else {
                    push_bounded(&HOLD_MS, 0.0);
                }
                flush_pending(l);
                push_bounded(&SEND_PHASE_MS, Instant::now().saturating_duration_since(next).as_secs_f64() * 1000.0);
            } else {
                let last_tick = origin + Duration::from_nanos(LAST_TICK_NS.load(Ordering::Relaxed));
                push_bounded(&SEND_PHASE_MS, Instant::now().saturating_duration_since(last_tick).as_secs_f64() * 1000.0);
            }
            {
                // the host side of the per-frame timeline (debug mode), same clock as the shim's frames CSV
                let hold_ms = HOLD_MS.lock().ok().and_then(|h| h.last().copied()).filter(|_| args.send_on_vsync).unwrap_or(0.0);
                logs::csv("host_frames.csv", "frame,ts_ns,t_tick,t_intake,t_submit,encode_ms,t_send,hold_ms,bytes,idr",
                    &format!("{counter},{},{:.6},{:.6},{:.6},{ms:.2},{:.6},{hold_ms:.2},{},{}", ts.as_nanos(),
                        f64::from_bits(LAST_TICK_QPC.load(Ordering::Relaxed)), logs::qpc_at(intake), submit_s, logs::qpc_now(),
                        l.packet_bytes, l.packet_idr as u8));
            }
            consecutive_errors = 0;
            encode_ms.push(ms);
            ffr_wait_ms.push(unsafe { nvh::nvh_last_ffr_gpu_ms(enc_handle) } as f64);
            let shim_stats = {
                let (mut m, mut a, mut b) = (0u32, 0u32, 0u32);
                unsafe { nvh::nvh_ipc_shim_stats(ipc, &mut m, &mut a, &mut b) };
                (m, a, b)
            };
            stats_win.tick(origin, &encode_ms, shared.bytes_sent.load(Ordering::Relaxed) + l.packet_bytes as u64, Some(&**l), Some(shim_stats), &intake_ms, &ffr_wait_ms);
            compose_ms.push(unsafe { nvh::nvh_ipc_last_compose_ms(ipc) } as f64);
            ffr_ms.push(unsafe { nvh::nvh_last_ffr_gpu_ms(enc_handle) } as f64);
            // frame column = the shim's frame counter (what the app submitted), so the harness can match app frames
            frame_rows.push(format!("{counter},{},{},{},{ms:.2}", ts.as_nanos(), l.packet_bytes, l.packet_idr as u8));
            sent_ts.push(ts.as_nanos() as u64);
            if args.daemon {
                // the logon-task host runs for days: keep the per-frame histories bounded
                stats_win.first_idx = stats_win.first_idx.saturating_sub(bound_history(&mut encode_ms));
                bound_history(&mut compose_ms);
                bound_history(&mut ffr_ms);
                bound_history(&mut ffr_wait_ms);
                bound_history(&mut frame_rows);
                bound_history(&mut sent_ts);
                bound_history(&mut intake_ms);
            }
            shared.frames_sent.fetch_add(1, Ordering::Relaxed);
            shared.bytes_sent.fetch_add(l.packet_bytes as u64, Ordering::Relaxed);
            let _ = frame_no;
            continue;
        }

        next += period;
        thread::sleep(next.saturating_duration_since(Instant::now()));
        if !connected {
            continue;
        }
        if let (Some(steps), Some(l)) = (&args.bench_network, live.as_mut()) {
            // Network benchmark: random frames (nothing for the encoder to reuse, so each frame costs the full bitrate) at each
            // bitrate step; 2 s to settle, then `bench_step_s` measured with ALVR's own client statistics.
            let now = Instant::now();
            let b = bench.get_or_insert_with(|| {
                l.bitrate_override = steps[0] as u64 * 1_000_000;
                event(origin, "bench_step_start", json!({ "step": 0, "target_mbps": steps[0] }));
                BenchState { i: 0, start: now, measuring: false, mstart: now, bytes0: 0, lost0: 0, frames0: 0, results: vec![] }
            });
            if !b.measuring && now.duration_since(b.start) >= Duration::from_secs(2) {
                logs::client_stats::take(2);
                b.measuring = true;
                b.mstart = now;
                b.bytes0 = shared.bytes_sent.load(Ordering::Relaxed);
                b.lost0 = logs::client_stats::packets_lost_total();
                b.frames0 = shared.frames_sent.load(Ordering::Relaxed);
            }
            if b.measuring && now.duration_since(b.mstart).as_secs_f64() >= args.bench_step_s {
                let secs = now.duration_since(b.mstart).as_secs_f64();
                let c = logs::client_stats::take(2).unwrap_or(json!({}));
                let r = json!({
                    "target_mbps": steps[b.i],
                    "actual_mbps": (shared.bytes_sent.load(Ordering::Relaxed) - b.bytes0) as f64 * 8.0 / secs / 1e6,
                    "sent_fps": (shared.frames_sent.load(Ordering::Relaxed) - b.frames0) as f64 / secs,
                    "packets_lost_per_s": (logs::client_stats::packets_lost_total().saturating_sub(b.lost0)) as f64 / secs,
                    "client_fps": c["client_fps"], "total_latency_ms": c["total_latency_ms"], "encode_ms": c["encode_ms"],
                    "network_ms": c["network_ms"], "decode_ms": c["decode_ms"], "decoder_queue_ms": c["decoder_queue_ms"],
                });
                event(origin, "bench_step", r.clone());
                b.results.push(r);
                b.i += 1;
                if b.i < steps.len() {
                    l.bitrate_override = steps[b.i] as u64 * 1_000_000;
                    b.start = now;
                    b.measuring = false;
                    event(origin, "bench_step_start", json!({ "step": b.i, "target_mbps": steps[b.i] }));
                } else {
                    let res = bench_recommendation(&b.results, args.fps);
                    logs::general("INFO", &format!("network benchmark: {}", res));
                    event(origin, "bench_result", res);
                    break;
                }
            }
        }
        // Like ALVR's own driver, keep sending even before tracking arrives (this also opens the return path
        // through firewalls that only admit UDP replies to flows the server started). Until the first real
        // tracking packet, stamp frames with time since connect.
        let ts = match *shared.latest_sample_ts.lock() {
            Some(ts) => ts,
            None => {
                dropped_no_ts += 1;
                connected_at.map(|t| t.elapsed()).unwrap_or_default()
            }
        };
        if shared.restart_from_idr.load(Ordering::SeqCst) && last_idr_restart.elapsed() >= min_idr_interval {
            shared.restart_from_idr.store(false, Ordering::SeqCst);
            last_idr_restart = Instant::now();
            idr_restarts_honoured += 1;
            idx = 0;
        }
        if let Some(l) = live.as_mut() {
            // idx was reset to 0 by an honoured IDR request (or first frame after connect)
            let force_idr = idx == 0;
            let frame_no = shared.frames_sent.load(Ordering::Relaxed);
            idx += 1;
            l.packet_bytes = 0;
            l.packet_idr = false;
            let mut err = [0 as std::ffi::c_char; 256];
            let ms = unsafe { nvh::nvh_encode(enc_handle, frame_no as u32, ts.as_nanos() as u64, force_idr as i32, err.as_mut_ptr(), 256) };
            if ms < 0.0 {
                eprintln!("encode failed: {}", unsafe { std::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy());
                break;
            }
            encode_ms.push(ms);
            stats_win.tick(origin, &encode_ms, shared.bytes_sent.load(Ordering::Relaxed) + l.packet_bytes as u64, Some(&**l), None, &[], &[]);
            frame_rows.push(format!("{frame_no},{},{},{},{ms:.2}", ts.as_nanos(), l.packet_bytes, l.packet_idr as u8));
            sent_ts.push(ts.as_nanos() as u64);
            shared.frames_sent.fetch_add(1, Ordering::Relaxed);
            shared.bytes_sent.fetch_add(l.packet_bytes as u64, Ordering::Relaxed);
            continue;
        }
        let au = &aus[idx % aus.len()];
        idx += 1;
        let (config, frame) = split_config(&au.data);
        if let Some(c) = config {
            ctx.set_video_config_nals(c.to_vec(), CodecType::Hevc);
            shared.config_sent.fetch_add(1, Ordering::Relaxed);
        }
        ctx.send_video_nal(ts, frame.to_vec(), au.is_idr);
        ctx.report_composed(ts, Duration::from_millis(1));
        ctx.report_present(ts, Duration::from_millis(1));
        sent_ts.push(ts.as_nanos() as u64);
        shared.frames_sent.fetch_add(1, Ordering::Relaxed);
        shared.bytes_sent.fetch_add(frame.len() as u64, Ordering::Relaxed);
    }

    if let Some(g) = game.take() {
        // the host stops while a game still runs (VisionALVR.exe closed mid-game): its summary still belongs in the log
        let secs = g.start.elapsed().as_secs_f64();
        let frames = shared.frames_sent.load(Ordering::Relaxed) - g.frames0;
        let submitted = shim_submitted(ipc).wrapping_sub(g.submitted0);
        logs::general("INFO", &format!(
            "game ended: {} after {}m{:02}s, {} frames rendered ({:.1} fps), {} streamed ({:.1} fps) (streamer stopping)",
            g.exe, secs as u64 / 60, secs as u64 % 60, submitted, submitted as f64 / secs.max(1e-3), frames, frames as f64 / secs.max(1e-3)));
        event(origin, "game_ended", json!({ "exe": g.exe, "seconds": secs, "frames_rendered": submitted, "frames_streamed": frames, "host_stopping": true }));
    }
    let frames = shared.frames_sent.load(Ordering::Relaxed);
    let connected = connected_at.is_some();
    let distinct_ts = {
        let mut v = sent_ts.clone();
        v.dedup();
        v.len()
    };
    vsync_stop.store(true, Ordering::Relaxed);
    let vsync_intervals = vsync_thread.map(|t| t.join().unwrap_or_default()).unwrap_or_default();
    let vsync_intervals_json = interval_stats(&vsync_intervals, 1000.0 / args.fps);
    let encoder_json = if args.live {
        let mut v = encode_ms.clone();
        v.sort_by(|a, b| a.partial_cmp(b).unwrap());
        json!({
            "width": enc_size.0, "height": enc_size.1,
            "shim_compose_ms_avg": compose_ms.iter().sum::<f64>() / compose_ms.len().max(1) as f64,
            "ffr_gpu_ms_avg": ffr_ms.iter().sum::<f64>() / ffr_ms.len().max(1) as f64,
            "nvenc_engines": unsafe { nvh::nvh_encoder_engines(enc_handle) },
            "split_encode_mode": args.split_encode,
            "slices_per_frame": live.as_ref().map(|l| l.slices as f64 / l.slice_samples.max(1) as f64),
            "callback_ms_avg": live.as_ref().map(|l| l.cb_ns as f64 / 1e6 / l.packets.max(1) as f64),
            "params_ms_avg": live.as_ref().map(|l| l.params_ns as f64 / 1e6 / encode_ms.len().max(1) as f64),
            "encode_ms": {
                "n": v.len(),
                "avg": v.iter().sum::<f64>() / v.len().max(1) as f64,
                "p50": v.get(v.len() / 2).copied().unwrap_or(0.0),
                "p95": v.get(((v.len() as f64) * 0.95) as usize).copied().unwrap_or(0.0),
                "max": v.last().copied().unwrap_or(0.0),
            },
        })
    } else {
        json!(null)
    };
    let pass = connected && frames >= args.min_frames;
    let report = json!({
        "pass": pass,
        "connected": connected,
        "frames_sent": frames,
        "bytes_sent": shared.bytes_sent.load(Ordering::Relaxed),
        "distinct_timestamps_sent": distinct_ts,
        "config_nal_sends": shared.config_sent.load(Ordering::Relaxed),
        "idr_requests": shared.idr_requests.load(Ordering::Relaxed),
        "idr_restarts_honoured": idr_restarts_honoured,
        "tracking_events": shared.tracking_events.load(Ordering::Relaxed),
        "frames_before_first_tracking": dropped_no_ts,
        "source_access_units": aus.len(),
        "source_idr": idr_count,
        "fps_target": args.fps,
        "refresh_hz": args.fps,
        "live": args.live,
        "haptics_sent": haptics_sent,
        "green_frames_sent": green_frames,
        "green_active_at_exit": green_active,
        "reconfigurations": reconfigs,
        "reconfigure_still_deferred": deferred_reconfig,
        "encode_errors": encode_errors,
        "audio": json!({ "config": audio_cfg, "device_events": shared.audio_device_events.load(Ordering::Relaxed) }),
        "shim": if ipc.is_null() { json!(null) } else {
            let (mut m, mut a, mut b) = (0u32, 0u32, 0u32);
            unsafe { nvh::nvh_ipc_shim_stats(ipc, &mut m, &mut a, &mut b) };
            json!({ "submit_mode": if m == 1 { "async" } else { "sync" }, "ts_matched": a, "ts_fallback": b })
        },
        "nvenc_config": nvenc_cfg,
        "qp_map": qp_on,
        "pacing": json!({ "vsync_ticks": vsync_ticks.load(Ordering::Relaxed), "vsync_interval_ms": vsync_intervals_json, "frame_intake_ms": interval_stats(&intake_ms, 1000.0 / args.fps) }),
        "encoder": encoder_json,
    });
    if let Some(p) = &args.frames_csv {
        fs::write(p, format!("frame,ts_ns,bytes,idr,encode_ms\n{}", frame_rows.join("\n"))).ok();
    }
    if !ipc.is_null() {
        unsafe { nvh::nvh_ipc_set_connected(ipc, 0) };
    }
    if !enc_handle.is_null() {
        unsafe { nvh::nvh_destroy(enc_handle) };
    }
    if !ipc.is_null() {
        unsafe { nvh::nvh_ipc_close(ipc) };
    }
    drop(live);
    if let Some(p) = &args.report {
        if let Err(e) = fs::write(p, serde_json::to_string_pretty(&report).unwrap()) {
            eprintln!("\ncannot write report {}: {e}", p.display());
        }
    }
    println!("{}", json!({ "event": "report", "data": report }));
    // dropping ServerCoreContext joins its threads (ALVR un-mutes the PC speakers here); do it explicitly before exit
    drop(ctx);
    logs::general("INFO", &format!("host stop ({} frames streamed{})", frames, if encode_errors > 0 { format!(", {encode_errors} encode errors") } else { String::new() }));
    std::process::exit(if !connected { 2 } else if pass { 0 } else { 1 });
}
