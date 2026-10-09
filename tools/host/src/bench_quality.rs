//! Benchmark phase 2 (docs/BENCHMARK_DESIGN.md): the local quality sweep, no headset needed (`--benchmark-quality`).
//! Each configuration (resolution, foveation, NVENC preset, spatial AQ, split encode) renders the benchmark scene through ALVR's
//! foveation pass and NVENC (nvenc/hostlib/bench.cpp) and gets: encode time, produced bitrate, slices per frame, and the quality the
//! headset would show (foveation-weighted PSNR in display space against a reference render). The search is pruned:
//!   a. preset curve P1..P5 at the current settings;
//!   b. resolution {0.8, 1.0, 1.2} x foveation {off, mild, strong} with the two best presets;
//!   c. at the three leaders: spatial AQ toggled, split encode forced.
//! Configurations whose encode p95 exceeds 85% of the frame time are not real time (kept in the results, never proposed).
//! The proposals use the latency known here (encode); phase 3 adds network + decode per frame size.
use serde_json::{json, Value};
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::Path;
use std::time::Instant;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Config {
    pub eye_w: i32,
    pub eye_h: i32,
    pub ref_w: i32,
    pub ref_h: i32,
    pub disp_w: i32,
    pub disp_h: i32,
    pub foveated: i32,
    pub fov_cs: [f32; 2],
    pub fov_sh: [f32; 2],
    pub fov_er: [f32; 2],
    pub preset: i32,
    pub aq: i32,
    pub split: i32,
    pub qp_map: i32,
    pub bitrate_mbps: f32,
    pub fps: f32,
    pub warmup: i32,
    pub frames: i32,
    pub metric_every: i32,
    pub metric_stride: i32,
    pub measure_quality: i32,
    pub t0: f64,
    pub fov: [f32; 8],
    pub ipd: f32,
}

#[repr(C)]
pub struct Result {
    pub enc_w: i32,
    pub enc_h: i32,
    pub recon: i32,
    pub recon_format: [c_char; 16],
    pub luma601: i32,
    pub y_gain: f32,
    pub y_offset: f32,
    pub encode_avg_ms: f64,
    pub encode_p50_ms: f64,
    pub encode_p95_ms: f64,
    pub encode_max_ms: f64,
    pub produced_mbps: f64,
    pub slices_per_frame: f64,
    pub metric_frames: i32,
    pub fw_psnr: f64,
    pub center_psnr: f64,
    pub periphery_psnr: f64,
    pub codec_psnr: f64,
    pub nvenc_engines: i32,
    pub note: [c_char; 256],
    pub error: [c_char; 256],
}

extern "C" {
    fn nvh_bench_open(scene: *const c_char, err: *mut c_char, errlen: i32) -> *mut c_void;
    fn nvh_bench_gpu(b: *mut c_void, buf: *mut c_char, len: i32) -> i32;
    fn nvh_bench_preview(b: *mut c_void, eye_w: i32, eye_h: i32, t: f64, fov8: *const f32, ipd: f32, rgba: *mut u8, len: i32) -> i32;
    fn nvh_bench_run(b: *mut c_void, cfg: *const Config, result: *mut Result) -> i32;
    fn nvh_bench_close(b: *mut c_void);
    fn nvh_gpu_sample(buf: *mut c_char, len: i32) -> i32;
    fn nvh_keepalive_start(duty_pct: i32) -> *mut c_void;
    fn nvh_keepalive_stop(k: *mut c_void);
}

/// NVML samples (5 Hz) while the sweep runs: encode times are only comparable at the same clocks (a laptop GPU moves its video
/// clock with temperature and power limits; an idle-ish GPU drops its P-state).
struct ClockLog {
    rows: std::sync::Arc<std::sync::Mutex<Vec<(f64, Vec<String>)>>>,
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
    t0: Instant,
}

impl ClockLog {
    fn start() -> Self {
        let rows = std::sync::Arc::new(std::sync::Mutex::new(vec![]));
        let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let t0 = Instant::now();
        let (r, st) = (rows.clone(), stop.clone());
        std::thread::spawn(move || {
            while !st.load(std::sync::atomic::Ordering::SeqCst) {
                let mut b = [0 as c_char; 256];
                if unsafe { nvh_gpu_sample(b.as_mut_ptr(), 256) } > 0 {
                    let f: Vec<String> = cstr(&b).split(',').map(str::to_string).collect();
                    r.lock().unwrap().push((t0.elapsed().as_secs_f64(), f));
                }
                std::thread::sleep(std::time::Duration::from_millis(200));
            }
        });
        Self { rows, stop, t0 }
    }
    /// median graphics / video clock, P-states, max temperature and power between two times (s since start)
    fn window(&self, a: f64, b: f64) -> Value {
        let rows = self.rows.lock().unwrap();
        let w: Vec<&Vec<String>> = rows.iter().filter(|(t, _)| *t >= a && *t <= b).map(|(_, f)| f).collect();
        if w.is_empty() {
            return Value::Null;
        }
        let med = |i: usize| {
            let mut v: Vec<f64> = w.iter().filter_map(|f| f.get(i)?.parse().ok()).collect();
            v.sort_by(|x, y| x.partial_cmp(y).unwrap());
            v.get(v.len() / 2).copied().unwrap_or(0.0)
        };
        let max = |i: usize| w.iter().filter_map(|f| f.get(i)?.parse::<f64>().ok()).fold(0.0, f64::max);
        let mut ps: Vec<String> = w.iter().filter_map(|f| f.get(9).cloned()).collect();
        ps.dedup();
        json!({ "graphics_mhz": med(4), "video_mhz": med(6), "encoder_pct": med(2), "power_w": max(7), "temp_c": max(8), "pstates": ps, "samples": w.len() })
    }
    fn now(&self) -> f64 {
        self.t0.elapsed().as_secs_f64()
    }
}

impl Drop for ClockLog {
    fn drop(&mut self) {
        self.stop.store(true, std::sync::atomic::Ordering::SeqCst);
    }
}

fn cstr(b: &[c_char]) -> String {
    unsafe { CStr::from_ptr(b.as_ptr()) }.to_string_lossy().to_string()
}

/// What the sweep starts from: the session's stream (or the headset's last views) and the link budget.
pub struct Base {
    pub eye_w: i32,
    pub eye_h: i32,
    pub fps: f64,
    pub foveated: bool,
    /// center size x/y, center shift x/y, edge ratio x/y
    pub geom: [f32; 6],
    pub preset: i32,
    pub aq: i32,
    pub split: i32,
    pub qp_map: bool,
    pub bitrate_mbps: f64,
    pub fov: [f32; 8],
    pub ipd: f32,
    pub engines: i32,
}

pub struct Options {
    pub warmup: i32,
    pub frames: i32,
    pub metric_every: i32,
    pub metric_stride: i32,
    pub out: Option<std::path::PathBuf>,
    pub preview: Option<std::path::PathBuf>,
    /// "full" (a+b+c) or "quick" (current settings + presets only)
    pub plan: String,
    /// reference render = display grid x this (1: what the game renders at the headset's size, so the score is what streaming
    /// loses; 2: a 2x supersampled render, an absolute reference that also scores the game's own anti-aliasing)
    pub ref_scale: i32,
    /// GPU keep-alive duty (%, 0 = off): a lightly loaded GPU drops to P2/P3 and NVENC slows down ~1.5x, as without a game
    pub keepalive: i32,
}

#[derive(Clone, Copy, PartialEq)]
struct Knobs {
    scale: f32,
    fov_level: u8, // 0 off, 1 mild, 2 strong (or the session's geometry when it is on)
    preset: i32,
    aq: i32,
    split: i32,
}

/// Foveation levels: off, mild (edge ratio 2.5 / 3), strong (the session's geometry if it has one, else ALVR's 4 / 5). The center
/// region and shift stay the session's.
fn fov_geom(base: &Base, level: u8) -> (bool, [f32; 6]) {
    let g = base.geom;
    let (cs, sh) = ([g[0], g[1]], [g[2], g[3]]);
    match level {
        0 => (false, g),
        1 => (true, [cs[0], cs[1], sh[0], sh[1], 2.5, 3.0]),
        _ => (true, if base.foveated { g } else { [cs[0], cs[1], sh[0], sh[1], 4.0, 5.0] }),
    }
}

fn round32(v: f32) -> i32 {
    ((v / 32.0).round() as i32).max(1) * 32
}

fn label(k: &Knobs) -> String {
    format!(
        "{}% {} P{} AQ {} split {}",
        (k.scale * 100.0).round(),
        ["no foveation", "mild foveation", "strong foveation"][k.fov_level as usize],
        k.preset,
        if k.aq == 1 { "on" } else { "off" },
        match k.split { 15 => "off".to_string(), 0 => "auto".to_string(), 1 => "forced".to_string(), n => format!("{n}") }
    )
}

pub fn run(origin: Instant, scene: &Path, base: &Base, opt: &Options, event: &dyn Fn(Instant, &str, Value)) -> i32 {
    let t_start = Instant::now();
    let mut err = [0 as c_char; 256];
    let cpath = CString::new(scene.to_string_lossy().as_bytes()).unwrap();
    let b = unsafe { nvh_bench_open(cpath.as_ptr(), err.as_mut_ptr(), 256) };
    if b.is_null() {
        let e = cstr(&err);
        event(origin, "bq_error", json!({ "message": format!("benchmark scene: {e}"), "scene": scene }));
        crate::logs::general("ERROR", &format!("quality benchmark: cannot load the scene {}: {e}", scene.display()));
        return 5;
    }
    let mut gpu = [0 as c_char; 128];
    unsafe { nvh_bench_gpu(b, gpu.as_mut_ptr(), 128) };
    let budget_ms = 1000.0 / base.fps;
    // quality is measured on the display grid (the session's eye size, what the client resamples to)
    let (disp_w, disp_h) = (base.eye_w, base.eye_h);
    let rs = opt.ref_scale.clamp(1, 2);
    let (ref_w, ref_h) = (disp_w * rs, disp_h * rs);
    event(origin, "bq_start", json!({
        "gpu": cstr(&gpu), "scene": scene, "eye": [base.eye_w, base.eye_h], "display_eye": [disp_w, disp_h], "reference_eye": [ref_w, ref_h], "fps": base.fps,
        "bitrate_mbps": base.bitrate_mbps, "fov_tan": base.fov, "ipd": base.ipd, "nvenc_engines": base.engines, "plan": opt.plan,
        "frames": opt.frames, "warmup": opt.warmup, "metric_every": opt.metric_every, "keepalive_pct": opt.keepalive,
    }));
    if let Some(p) = &opt.preview {
        // a quarter-size frame of what is encoded (check the render)
        let (w, h) = (round32(base.eye_w as f32 / 4.0), round32(base.eye_h as f32 / 4.0));
        let mut px = vec![0u8; (w * 2 * h * 4) as usize];
        let n = unsafe { nvh_bench_preview(b, w, h, 2.0, base.fov.as_ptr(), base.ipd, px.as_mut_ptr(), px.len() as i32) };
        if n > 0 {
            let ok = write_png(p, (w * 2) as u32, h as u32, &px).is_ok();
            event(origin, "bq_preview", json!({ "path": p, "size": [w * 2, h], "written": ok }));
        }
    }

    let clocks = ClockLog::start();
    let keep = if opt.keepalive > 0 { unsafe { nvh_keepalive_start(opt.keepalive) } } else { std::ptr::null_mut() };
    if !keep.is_null() {
        std::thread::sleep(std::time::Duration::from_millis(2500)); // the driver raises the clocks over ~1-2 s of load
    }
    let mut results: Vec<(Knobs, Value)> = vec![];
    let mut measure = |k: Knobs, results: &mut Vec<(Knobs, Value)>| -> Option<Value> {
        if let Some((_, v)) = results.iter().find(|(x, _)| *x == k) {
            return Some(v.clone());
        }
        let (foveated, g) = fov_geom(base, k.fov_level);
        let cfg = Config {
            eye_w: round32(base.eye_w as f32 * k.scale),
            eye_h: round32(base.eye_h as f32 * k.scale),
            ref_w,
            ref_h,
            disp_w,
            disp_h,
            foveated: foveated as i32,
            fov_cs: [g[0], g[1]],
            fov_sh: [g[2], g[3]],
            fov_er: [g[4], g[5]],
            preset: k.preset,
            aq: k.aq,
            split: k.split,
            qp_map: (base.qp_map && foveated) as i32,
            bitrate_mbps: base.bitrate_mbps as f32,
            fps: base.fps as f32,
            warmup: opt.warmup,
            frames: opt.frames,
            metric_every: opt.metric_every,
            metric_stride: opt.metric_stride,
            measure_quality: 1,
            t0: 2.0,
            fov: base.fov,
            ipd: base.ipd,
        };
        let mut r: Result = unsafe { std::mem::zeroed() };
        let t = Instant::now();
        let c0 = clocks.now();
        let ok = unsafe { nvh_bench_run(b, &cfg, &mut r) } != 0;
        let gpu_clocks = clocks.window(c0, clocks.now());
        let v = json!({
            "label": label(&k), "scale": k.scale, "eye": [cfg.eye_w, cfg.eye_h], "foveation": k.fov_level, "foveated": foveated,
            "geometry": g, "preset": k.preset, "aq": k.aq, "split": k.split, "qp_map": cfg.qp_map,
            "ok": ok, "error": cstr(&r.error), "note": cstr(&r.note),
            "encoded": [r.enc_w, r.enc_h], "encoded_mpix": r.enc_w as f64 * r.enc_h as f64 / 1e6,
            "encode_ms": { "avg": r.encode_avg_ms, "p50": r.encode_p50_ms, "p95": r.encode_p95_ms, "max": r.encode_max_ms },
            "realtime": ok && r.encode_p95_ms <= budget_ms * 0.85,
            "produced_mbps": r.produced_mbps, "slices_per_frame": r.slices_per_frame,
            "recon": r.recon, "recon_format": cstr(&r.recon_format), "luma": if r.luma601 != 0 { "bt601" } else { "bt709" },
            "y_fit": [r.y_gain, r.y_offset], "metric_frames": r.metric_frames, "nvenc_engines": r.nvenc_engines,
            "psnr": { "weighted": r.fw_psnr, "center": r.center_psnr, "periphery": r.periphery_psnr, "codec": r.codec_psnr },
            "seconds": t.elapsed().as_secs_f64(), "gpu": gpu_clocks,
        });
        event(origin, "bq_result", v.clone());
        results.push((k, v.clone()));
        Some(v)
    };
    let q = |v: &Value| v["psnr"]["weighted"].as_f64().unwrap_or(0.0);
    let p95 = |v: &Value| v["encode_ms"]["p95"].as_f64().unwrap_or(f64::MAX);
    let valid = |v: &Value| v["realtime"] == true && v["recon"] == 1 && v["metric_frames"].as_i64().unwrap_or(0) > 0;

    let cur_level = if base.foveated { 2 } else { 0 };
    let today = Knobs { scale: 1.0, fov_level: cur_level, preset: base.preset, aq: base.aq, split: base.split };
    // a. preset curve
    for p in 1..=5 {
        measure(Knobs { preset: p, ..today }, &mut results);
    }
    if opt.plan != "quick" {
        // the two best presets: the best quality that is real time, and the fastest within 0.3 dB of it
        let curve: Vec<(i32, Value)> = results.iter().map(|(k, v)| (k.preset, v.clone())).collect();
        let rt: Vec<&(i32, Value)> = curve.iter().filter(|(_, v)| valid(v)).collect();
        let mut presets: Vec<i32> = vec![];
        if let Some(best) = rt.iter().max_by(|a, b| q(&a.1).partial_cmp(&q(&b.1)).unwrap()) {
            presets.push(best.0);
            if let Some(fast) = rt.iter().filter(|(_, v)| q(v) >= q(&best.1) - 0.3).min_by(|a, b| p95(&a.1).partial_cmp(&p95(&b.1)).unwrap()) {
                if fast.0 != best.0 {
                    presets.push(fast.0);
                }
            }
        }
        if presets.len() < 2 {
            for p in [base.preset, 1, 3] {
                if !presets.contains(&p) && presets.len() < 2 {
                    presets.push(p);
                }
            }
        }
        // b. resolution x foveation
        for &p in &presets {
            for scale in [0.8f32, 1.0, 1.2] {
                for level in [0u8, 1, 2] {
                    measure(Knobs { scale, fov_level: level, preset: p, ..today }, &mut results);
                }
            }
        }
        // c. AQ and split at the three leaders
        let mut leaders: Vec<(Knobs, Value)> = results.iter().filter(|(_, v)| valid(v)).cloned().collect();
        leaders.sort_by(|a, b| q(&b.1).partial_cmp(&q(&a.1)).unwrap());
        leaders.truncate(3);
        let engines = results.iter().filter_map(|(_, v)| v["nvenc_engines"].as_i64()).max().unwrap_or(base.engines as i64);
        let forced_split = match engines { n if n >= 3 => 3, 2 => 2, _ => 1 };
        for (k, _) in leaders {
            measure(Knobs { aq: if k.aq == 1 { 0 } else { 1 }, ..k }, &mut results);
            for split in [15, forced_split] {
                if split != k.split {
                    measure(Knobs { split, ..k }, &mut results);
                }
            }
        }
    }
    unsafe { nvh_bench_close(b) };
    if !keep.is_null() {
        unsafe { nvh_keepalive_stop(keep) };
    }

    let all: Vec<Value> = results.iter().map(|(_, v)| v.clone()).collect();
    let today_v = results.iter().find(|(k, _)| *k == today).map(|(_, v)| v.clone());
    let props = proposals(&all, today_v.as_ref(), budget_ms);
    let engines = all.iter().filter_map(|v| v["nvenc_engines"].as_i64()).max().unwrap_or(base.engines as i64) as i32;
    let split_check = split_check(&all, engines);
    // clock stability: the video (NVENC) clock across configurations
    let vclk: Vec<f64> = all.iter().filter_map(|v| v["gpu"]["video_mhz"].as_f64()).filter(|c| *c > 0.0).collect();
    let (vmin, vmax) = (vclk.iter().cloned().fold(f64::MAX, f64::min), vclk.iter().cloned().fold(0.0, f64::max));
    let clock_stable = vclk.is_empty() || vmin >= vmax * 0.9;
    let clock_check = json!({ "video_mhz_min": if vclk.is_empty() { Value::Null } else { json!(vmin) }, "video_mhz_max": vmax, "stable": clock_stable,
        "note": if clock_stable { "NVENC clock steady: encode times are comparable" } else { "NVENC clock varied by more than 10% during the benchmark (temperature / power limit): encode times are not comparable; plug in a laptop, close GPU-heavy apps and run it again" } });
    if !clock_stable {
        crate::logs::general("WARN", &format!("quality benchmark: NVENC clock varied {vmin:.0}-{vmax:.0} MHz; encode times are not comparable"));
    }
    let summary = json!({
        "gpu_clocks": clock_check,
        "version": 1, "seconds": t_start.elapsed().as_secs_f64(), "gpu": cstr(&gpu), "fps": base.fps, "bitrate_mbps": base.bitrate_mbps,
        "budget_ms": budget_ms, "realtime_limit_ms": budget_ms * 0.85, "reference_eye": [ref_w, ref_h], "today": today_v,
        "split_encode": split_check, "proposals": props, "latency_basis": "encode p95 only (network + decode come from the headset phase)",
        "results": all,
    });
    event(origin, "bq_summary", summary.clone());
    if let Some(p) = &opt.out {
        std::fs::write(p, serde_json::to_string_pretty(&summary).unwrap()).ok();
    }
    let line = |name: &str| -> String {
        props[name].as_object().map(|o| format!("{name}: {} ({:.1} dB, encode p95 {:.1} ms)", o["label"].as_str().unwrap_or("?"),
            o["psnr"]["weighted"].as_f64().unwrap_or(0.0), o["encode_ms"]["p95"].as_f64().unwrap_or(0.0))).unwrap_or_else(|| format!("{name}: none"))
    };
    crate::logs::general("INFO", &format!(
        "quality benchmark: {} configurations in {:.0} s at {:.0} Mbps | {} | {} | {}",
        all.len(), t_start.elapsed().as_secs_f64(), base.bitrate_mbps, line("lowest_latency"), line("recommended"), line("best_quality")
    ));
    0
}

/// Pareto front of (latency, quality) among the real-time configurations, and the three proposals:
/// lowest latency within 2 dB of the best quality; the knee of the front; the best quality within +8 ms of the fastest.
pub fn proposals(all: &[Value], today: Option<&Value>, _budget_ms: f64) -> Value {
    // quality compared at 0.1 dB: smaller differences are noise, the faster configuration wins them
    let q = |v: &Value| (v["psnr"]["weighted"].as_f64().unwrap_or(0.0) * 10.0).round() / 10.0;
    let lat = |v: &Value| v["latency_ms"].as_f64().unwrap_or_else(|| v["encode_ms"]["p95"].as_f64().unwrap_or(f64::MAX));
    let valid: Vec<&Value> = all.iter().filter(|v| v["realtime"] == true && v["recon"] == 1 && v["metric_frames"].as_i64().unwrap_or(0) > 0).collect();
    if valid.is_empty() {
        return json!({ "lowest_latency": null, "recommended": null, "best_quality": null, "front": [],
            "reason": "no configuration encodes in real time with a usable reconstruction" });
    }
    let mut front: Vec<&Value> = valid.iter().copied().filter(|v| !valid.iter().any(|o| (lat(o) <= lat(v) && q(o) > q(v)) || (lat(o) < lat(v) && q(o) >= q(v)))).collect();
    front.sort_by(|a, b| lat(a).partial_cmp(&lat(b)).unwrap());
    let best_q = valid.iter().map(|v| q(v)).fold(f64::MIN, f64::max);
    let min_lat = valid.iter().map(|v| lat(v)).fold(f64::MAX, f64::min);
    let lowest = valid.iter().filter(|v| q(v) >= best_q - 2.0).min_by(|a, b| lat(a).partial_cmp(&lat(b)).unwrap()).copied();
    // (equal quality: the faster one)
    let best = valid.iter().filter(|v| lat(v) <= min_lat + 8.0)
        .max_by(|a, b| q(a).partial_cmp(&q(b)).unwrap().then(lat(b).partial_cmp(&lat(a)).unwrap())).copied();
    // knee: the front point farthest from the chord between its ends (latency and quality normalized to the front's ranges)
    let knee = if front.len() >= 3 {
        let (l0, l1) = (lat(front[0]), lat(front[front.len() - 1]));
        let (q0, q1) = (q(front[0]), q(front[front.len() - 1]));
        let (dl, dq) = ((l1 - l0).max(1e-6), (q1 - q0).max(1e-6));
        front.iter().copied().max_by(|a, b| {
            let d = |v: &Value| ((q(v) - q0) / dq) - ((lat(v) - l0) / dl);
            d(a).partial_cmp(&d(b)).unwrap()
        })
    } else {
        front.last().copied()
    };
    let with_delta = |v: Option<&Value>| -> Value {
        match v {
            None => Value::Null,
            Some(v) => {
                let mut o = v.clone();
                if let Some(t) = today {
                    o["vs_today"] = json!({ "psnr_db": q(v) - q(t), "latency_ms": lat(v) - lat(t) });
                }
                o
            }
        }
    };
    json!({
        "lowest_latency": with_delta(lowest), "recommended": with_delta(knee), "best_quality": with_delta(best),
        "front": front.iter().map(|v| json!({ "label": v["label"], "latency_ms": lat(v), "psnr": q(v) })).collect::<Vec<_>>(),
    })
}

/// Whether split encode actually split the frame (slices per frame > 1 with split forced).
fn split_check(all: &[Value], engines: i32) -> Value {
    let forced: Vec<&Value> = all.iter().filter(|v| matches!(v["split"].as_i64(), Some(1..=3))).collect();
    let slices = forced.iter().filter_map(|v| v["slices_per_frame"].as_f64()).fold(0.0f64, f64::max);
    json!({
        "nvenc_engines": engines, "tested": !forced.is_empty(), "max_slices_per_frame": slices,
        "working": !forced.is_empty() && slices > 1.5,
        "note": if engines < 2 { "this GPU has one NVENC engine: split encode cannot split" } else if forced.is_empty() { "not tested" } else if slices > 1.5 { "NVENC split the frame" } else { "requested but NVENC encoded one slice" },
    })
}

/// Minimal PNG (RGB, stored deflate blocks): a preview for checks, no image crate needed.
fn write_png(path: &Path, w: u32, h: u32, rgba: &[u8]) -> std::io::Result<()> {
    fn crc32(data: &[u8]) -> u32 {
        let mut c = 0xFFFF_FFFFu32;
        for &b in data {
            c ^= b as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
        }
        !c
    }
    let mut raw = Vec::with_capacity(((w * 3 + 1) * h) as usize);
    for y in 0..h as usize {
        raw.push(0u8);
        for x in 0..w as usize {
            let p = &rgba[(y * w as usize + x) * 4..][..3];
            raw.extend_from_slice(p);
        }
    }
    let mut z = vec![0x78u8, 0x01];
    for (i, chunk) in raw.chunks(65535).enumerate() {
        let last = (i + 1) * 65535 >= raw.len();
        z.push(last as u8);
        let n = chunk.len() as u16;
        z.extend_from_slice(&n.to_le_bytes());
        z.extend_from_slice(&(!n).to_le_bytes());
        z.extend_from_slice(chunk);
    }
    let (mut a, mut bb) = (1u32, 0u32);
    for &x in &raw {
        a = (a + x as u32) % 65521;
        bb = (bb + a) % 65521;
    }
    z.extend_from_slice(&((bb << 16) | a).to_be_bytes());
    let mut out = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
    let mut chunk = |ty: &[u8], data: &[u8]| {
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        let mut c = ty.to_vec();
        c.extend_from_slice(data);
        out.extend_from_slice(&c);
        out.extend_from_slice(&crc32(&c).to_be_bytes());
    };
    let mut ihdr = vec![];
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    chunk(b"IHDR", &ihdr);
    chunk(b"IDAT", &z);
    chunk(b"IEND", &[]);
    std::fs::write(path, out)
}
