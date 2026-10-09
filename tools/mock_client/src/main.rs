//! Headless mock ALVR client for the harness: no window, no real decoder.
//!
//! Behaves like a headset as far as the ALVR protocol goes (discovery, stream config, scripted head pose,
//! frame/compositor/submit statistics), and writes the received video bitstream to a file. A JSON report and
//! the exit code say whether it worked.
//!
//! Exit codes: 0 = pass, 1 = streaming ran but fewer than --min-frames arrived, 2 = never connected,
//! 3 = bad arguments.
use alvr_client_core::{ClientCapabilities, ClientCoreContext, ClientCoreEvent};
use alvr_common::{
    glam::{Quat, UVec2, Vec3},
    parking_lot::Mutex,
    DeviceMotion, Fov, Pose, RelaxedAtomic, ViewParams, HAND_LEFT_ID, HAND_RIGHT_ID, HEAD_ID, LEFT_SQUEEZE_VALUE_ID,
    LEFT_THUMBSTICK_X_ID, LEFT_THUMBSTICK_Y_ID, LEFT_X_CLICK_ID, QUEST_CONTROLLER_PROFILE_ID, RIGHT_A_CLICK_ID,
    RIGHT_TRIGGER_VALUE_ID,
};
use alvr_packets::{ButtonEntry, ButtonValue, FaceData};
use serde_json::json;
use std::{
    collections::HashMap,
    fs::File,
    io::Write,
    sync::{Arc, Weak},
    thread,
    time::{Duration, Instant},
};

struct Args {
    out: Option<String>,
    report: Option<String>,
    frames_csv: Option<String>,
    seconds: f64,
    connect_timeout: f64,
    min_frames: usize,
    exit_after_frames: Option<usize>,
    yaw_amp: f32,
    yaw_hz: f32,
    height: f32,
    refresh: f32,
    res: UVec2,
    prefer_10bit: bool,
    foveated: bool,
    decode_ms: u64,
    controllers: bool,
    controllers_csv: Option<String>,
    views_after_first_frame: bool,
    encoding_gamma: f32,
}

fn parse_args() -> Result<Args, String> {
    let mut a = Args {
        out: None,
        report: None,
        frames_csv: None,
        seconds: 20.0,
        connect_timeout: 60.0,
        min_frames: 0,
        exit_after_frames: None,
        yaw_amp: 0.5,
        yaw_hz: 0.25,
        height: 1.5,
        refresh: 90.0,
        // Vision Pro defaults (what the visionOS client advertises): 3552x3200 per eye, 90 Hz, 10-bit, foveated encoding
        res: UVec2::new(3552, 3200),
        prefer_10bit: true,
        foveated: true,
        decode_ms: 2,
        controllers: false,
        controllers_csv: None,
        views_after_first_frame: false,
        encoding_gamma: 1.0,
    };
    let mut it = std::env::args().skip(1);
    while let Some(k) = it.next() {
        let mut v = || it.next().ok_or(format!("missing value for {k}"));
        match k.as_str() {
            "--out" => a.out = Some(v()?),
            "--report" => a.report = Some(v()?),
            "--frames-csv" => a.frames_csv = Some(v()?),
            "--seconds" => a.seconds = v()?.parse().map_err(|e| format!("{e}"))?,
            "--connect-timeout" => a.connect_timeout = v()?.parse().map_err(|e| format!("{e}"))?,
            "--min-frames" => a.min_frames = v()?.parse().map_err(|e| format!("{e}"))?,
            "--exit-after-frames" => a.exit_after_frames = Some(v()?.parse().map_err(|e| format!("{e}"))?),
            "--yaw-amp" => a.yaw_amp = v()?.parse().map_err(|e| format!("{e}"))?,
            "--yaw-hz" => a.yaw_hz = v()?.parse().map_err(|e| format!("{e}"))?,
            "--height" => a.height = v()?.parse().map_err(|e| format!("{e}"))?,
            "--refresh" => a.refresh = v()?.parse().map_err(|e| format!("{e}"))?,
            "--decode-ms" => a.decode_ms = v()?.parse().map_err(|e| format!("{e}"))?,
            "--res" => {
                let s = v()?;
                let (w, h) = s.split_once('x').ok_or("--res WxH")?;
                a.res = UVec2::new(w.parse().map_err(|e| format!("{e}"))?, h.parse().map_err(|e| format!("{e}"))?);
            }
            "--controllers" => a.controllers = true,
            // like the stock visionOS client once it is in its immersive space: ViewsConfig only after the first decoded frame
            "--views-after-first-frame" => a.views_after_first_frame = true,
            // the visionOS client asks for 1.5 (ALVR uses it unless the session sets server_overrides_encoding_gamma)
            "--encoding-gamma" => a.encoding_gamma = v()?.parse().map_err(|e| format!("{e}"))?,
            "--controllers-csv" => a.controllers_csv = Some(v()?),
            "--no-10bit" => a.prefer_10bit = false,
            "--no-foveation" => a.foveated = false,
            other => return Err(format!("unknown argument {other}")),
        }
    }
    Ok(a)
}

struct FrameRec {
    ts_ns: u128,
    arrival_ns: u128,
    len: usize,
    yaw: Option<f32>,
}

#[derive(Default)]
struct Shared {
    sent_yaw: HashMap<u128, f32>,
    poses_sent: usize,
    frames: Vec<FrameRec>,
    bytes: usize,
    decoder_config_bytes: usize,
    controller_log: Vec<String>,
    haptics: Vec<(f64, u64, u64, f32, f32)>, // t_ms, device, duration_ms, frequency, amplitude
    views_sent_at_ms: Option<f64>,
}

fn event(origin: Instant, name: &str, extra: serde_json::Value) {
    println!("{}", json!({ "t_ms": origin.elapsed().as_secs_f64() * 1000.0, "event": name, "data": extra }));
}

fn pct(sorted: &[f64], p: f64) -> f64 {
    if sorted.is_empty() {
        return 0.0;
    }
    sorted[((sorted.len() - 1) as f64 * p).round() as usize]
}

fn stats(mut v: Vec<f64>) -> serde_json::Value {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    json!({ "n": v.len(), "p50": pct(&v, 0.5), "p95": pct(&v, 0.95), "max": v.last().copied().unwrap_or(0.0) })
}

fn main() {
    env_logger::init();
    let args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(3);
        }
    };
    let origin = Instant::now();

    let capabilities = ClientCapabilities {
        default_view_resolution: args.res,
        refresh_rates: vec![args.refresh],
        foveated_encoding: args.foveated,
        encoder_high_profile: true,
        encoder_10_bits: true,
        encoder_av1: false,
        prefer_10bit: args.prefer_10bit,
        prefer_full_range: true,
        preferred_encoding_gamma: args.encoding_gamma,
        prefer_hdr: false,
    };
    let ctx = Arc::new(ClientCoreContext::new(capabilities));
    ctx.resume();

    let shared = Arc::new(Mutex::new(Shared::default()));
    let streaming = Arc::new(RelaxedAtomic::new(false));
    let mut tracking_thread = None;
    let mut config_json = serde_json::Value::Null;
    let mut codec = String::from("none");
    let mut connected_at: Option<f64> = None;
    let mut stream_started = false;
    let mut got_decoder_config = false; // ALVR: all DecoderConfig events after the first are ignored until reconnection
    let mut out_file: Option<Arc<Mutex<File>>> = args.out.as_ref().map(|p| Arc::new(Mutex::new(File::create(p).expect("create --out"))));

    let deadline_connect = Duration::from_secs_f64(args.connect_timeout);
    let mut stream_started_at = origin;
    loop {
        while let Some(ev) = ctx.poll_event() {
            match ev {
                ClientCoreEvent::UpdateHudMessage(m) => event(origin, "hud", json!(m)),
                ClientCoreEvent::StreamingStarted(cfg) => {
                    stream_started = true;
                    got_decoder_config = false;
                    stream_started_at = Instant::now();
                    connected_at = Some(origin.elapsed().as_secs_f64());
                    config_json = serde_json::to_value(&cfg.negotiated_config).unwrap_or_default();
                    event(origin, "streaming_started", config_json.clone());
                    streaming.set(true);
                    let fps = cfg.negotiated_config.refresh_rate_hint;
                    let (c, s, sh) = (Arc::clone(&ctx), Arc::clone(&streaming), Arc::clone(&shared));
                    let (amp, hz, h) = (args.yaw_amp, args.yaw_hz, args.height);
                    let controllers = args.controllers;
                    let late_views = args.views_after_first_frame;
                    tracking_thread = Some(thread::spawn(move || tracking(c, s, sh, origin, fps, amp, hz, h, controllers, late_views)));
                }
                ClientCoreEvent::StreamingStopped => {
                    event(origin, "streaming_stopped", json!(null));
                    streaming.set(false);
                    if let Some(t) = tracking_thread.take() {
                        t.join().ok();
                    }
                }
                ClientCoreEvent::DecoderConfig { .. } if got_decoder_config => {}
                ClientCoreEvent::DecoderConfig { codec: c, config_nal } => {
                    got_decoder_config = true;
                    codec = format!("{c:?}");
                    shared.lock().decoder_config_bytes = config_nal.len();
                    event(origin, "decoder_config", json!({ "codec": codec, "config_nal_bytes": config_nal.len() }));
                    if let Some(f) = &out_file {
                        f.lock().write_all(&config_nal).ok();
                    }
                    let weak: Weak<ClientCoreContext> = Arc::downgrade(&ctx);
                    let (sh, of, decode_ms) = (Arc::clone(&shared), out_file.clone(), args.decode_ms);
                    ctx.set_decoder_input_callback(Box::new(move |ts, data| {
                        let arrival = origin.elapsed();
                        if let Some(f) = &of {
                            f.lock().write_all(data).ok();
                        }
                        {
                            let mut s = sh.lock();
                            let yaw = s.sent_yaw.get(&ts.as_nanos()).copied();
                            s.frames.push(FrameRec { ts_ns: ts.as_nanos(), arrival_ns: arrival.as_nanos(), len: data.len(), yaw });
                            s.bytes += data.len();
                        }
                        if let Some(c) = weak.upgrade() {
                            thread::sleep(Duration::from_millis(decode_ms));
                            c.report_frame_decoded(ts);
                            c.report_compositor_start(ts);
                            c.report_submit(ts, Duration::from_millis(1));
                        }
                        true
                    }));
                }
                ClientCoreEvent::Haptics { device_id, duration, frequency, amplitude } => {
                    shared.lock().haptics.push((origin.elapsed().as_secs_f64() * 1000.0, device_id, duration.as_millis() as u64, frequency, amplitude));
                    event(origin, "haptics", json!({ "device": device_id, "ms": duration.as_millis() as u64, "freq": frequency, "amp": amplitude }))
                }
                ClientCoreEvent::RealTimeConfig(_) => event(origin, "realtime_config", json!(null)),
            }
        }

        let nframes = shared.lock().frames.len();
        if !stream_started && origin.elapsed() > deadline_connect {
            break;
        }
        if stream_started {
            if stream_started_at.elapsed().as_secs_f64() > args.seconds {
                break;
            }
            if let Some(n) = args.exit_after_frames {
                if nframes >= n {
                    break;
                }
            }
        }
        thread::sleep(Duration::from_millis(5));
    }

    // ALVR's own running average of tracking-sent -> frame-displayed (what it uses for head prediction)
    let pipeline_latency_ms = ctx.get_total_prediction_offset().as_secs_f64() * 1000.0;
    streaming.set(false);
    if let Some(t) = tracking_thread.take() {
        t.join().ok();
    }
    ctx.pause();
    out_file.take();

    let s = shared.lock();
    let n = s.frames.len();
    let span = match (s.frames.first(), s.frames.last()) {
        (Some(a), Some(b)) if n > 1 => (b.arrival_ns - a.arrival_ns) as f64 / 1e9,
        _ => 0.0,
    };
    let inter: Vec<f64> = s.frames.windows(2).map(|w| (w[1].arrival_ns - w[0].arrival_ns) as f64 / 1e6).collect();
    let lat: Vec<f64> = s.frames.iter().map(|f| (f.arrival_ns as f64 - f.ts_ns as f64) / 1e6).collect();
    let monotonic = s.frames.windows(2).all(|w| w[1].ts_ns >= w[0].ts_ns);
    let dup = s.frames.windows(2).filter(|w| w[1].ts_ns == w[0].ts_ns).count();
    let matched = s.frames.iter().filter(|f| f.yaw.is_some()).count();
    let pass = stream_started && n >= args.min_frames && (args.min_frames == 0 || matched == n);
    let report = json!({
        "pass": pass,
        "connected": stream_started,
        "connected_at_s": connected_at,
        "codec": codec,
        "negotiated_config": config_json,
        "decoder_config_nal_bytes": s.decoder_config_bytes,
        "frames": n,
        "bytes": s.bytes,
        "span_s": span,
        "fps_measured": if span > 0.0 { (n as f64 - 1.0) / span } else { 0.0 },
        "mbps_measured": if span > 0.0 { s.bytes as f64 * 8.0 / span / 1e6 } else { 0.0 },
        "interarrival_ms": stats(inter),
        "arrival_minus_timestamp_ms": stats(lat),
        "timestamps_monotonic": monotonic,
        "duplicate_timestamps": dup,
        "frames_with_matching_sent_pose": matched,
        "poses_sent": s.poses_sent,
        "total_pipeline_latency_ms": pipeline_latency_ms,
        "views_after_first_frame": args.views_after_first_frame,
        "views_sent_at_ms": s.views_sent_at_ms,
        "haptics": {
            "count": s.haptics.len(),
            "max_amplitude": s.haptics.iter().map(|h| h.4).fold(0.0f32, f32::max),
            "devices": s.haptics.iter().map(|h| h.1).collect::<std::collections::BTreeSet<_>>(),
            "first": s.haptics.iter().take(12).map(|h| json!({ "t_ms": h.0, "device": h.1, "duration_ms": h.2, "frequency": h.3, "amplitude": h.4 })).collect::<Vec<_>>(),
        },
    });
    if let Some(p) = &args.controllers_csv {
        let mut f = File::create(p).expect("create --controllers-csv");
        writeln!(f, "ts_ns,trigger_r,squeeze_l,a_r,stick_lx,stick_ly,x_l,rx,ry,rz,lx,ly,lz").ok();
        for l in &s.controller_log {
            writeln!(f, "{l}").ok();
        }
    }
    if let Some(p) = &args.report {
        std::fs::write(p, serde_json::to_string_pretty(&report).unwrap()).ok();
    }
    if let Some(p) = &args.frames_csv {
        let mut f = File::create(p).expect("create --frames-csv");
        writeln!(f, "ts_ns,arrival_ns,len,yaw").ok();
        for r in &s.frames {
            writeln!(f, "{},{},{},{}", r.ts_ns, r.arrival_ns, r.len, r.yaw.map(|y| y.to_string()).unwrap_or_default()).ok();
        }
    }
    println!("{}", json!({ "event": "report", "data": report }));
    std::process::exit(if !stream_started { 2 } else if pass { 0 } else { 1 });
}

#[allow(clippy::too_many_arguments)]
fn tracking(
    ctx: Arc<ClientCoreContext>,
    streaming: Arc<RelaxedAtomic>,
    shared: Arc<Mutex<Shared>>,
    origin: Instant,
    fps: f32,
    amp: f32,
    hz: f32,
    height: f32,
    controllers: bool,
    late_views: bool,
) {
    if controllers {
        // Announce the Quest/Touch profile (what ALVR emulates) for both hands.
        ctx.send_active_interaction_profile(*HAND_LEFT_ID, *QUEST_CONTROLLER_PROFILE_ID);
        ctx.send_active_interaction_profile(*HAND_RIGHT_ID, *QUEST_CONTROLLER_PROFILE_ID);
    }
    let mut last_buttons: [Option<ButtonValue>; 3] = [None, None, None];
    let vp = ViewParams { pose: Pose::default(), fov: Fov { left: -1.0, right: 1.0, up: 1.0, down: -1.0 } };
    let mut views_sent = false;
    let mut next = Instant::now();
    while streaming.value() {
        if !views_sent && (!late_views || !shared.lock().frames.is_empty()) {
            ctx.send_view_params([vp, vp]);
            views_sent = true;
            shared.lock().views_sent_at_ms = Some(origin.elapsed().as_secs_f64() * 1000.0);
        }
        let ts = origin.elapsed();
        let yaw = amp * (2.0 * std::f32::consts::PI * hz * ts.as_secs_f32()).sin();
        shared.lock().sent_yaw.insert(ts.as_nanos(), yaw);
        shared.lock().poses_sent += 1;
        let mut motions = vec![(
            *HEAD_ID,
            DeviceMotion {
                pose: Pose { orientation: Quat::from_rotation_y(yaw), position: Vec3::new(0.0, height, 0.0) },
                linear_velocity: Vec3::ZERO,
                angular_velocity: Vec3::ZERO,
            },
        )];
        if controllers {
            // Scripted controllers, deterministic in time so the harness can compare what the app sees.
            let t = ts.as_secs_f32();
            let tri = |t: f32, period: f32| {
                let p = (t / period).fract();
                if p < 0.5 { p * 2.0 } else { 2.0 - p * 2.0 }
            };
            let lpos = Vec3::new(-0.25, 1.2 + 0.05 * (t * 1.3).sin(), -0.3);
            let rpos = Vec3::new(0.25 + 0.1 * (std::f32::consts::TAU * 0.5 * t).sin(), 1.3, -0.3 + 0.05 * (t * 0.7).cos());
            for (id, pos) in [(*HAND_LEFT_ID, lpos), (*HAND_RIGHT_ID, rpos)] {
                motions.push((id, DeviceMotion { pose: Pose { orientation: Quat::IDENTITY, position: pos }, linear_velocity: Vec3::ZERO, angular_velocity: Vec3::ZERO }));
            }
            let trigger_r = tri(t, 4.0);
            let squeeze_l = tri(t + 1.0, 3.0) * 0.8;
            let a_r = (t as i64) % 2 == 1;
            let stick = ((std::f32::consts::TAU * t / 3.0).sin(), (std::f32::consts::TAU * t / 3.0).cos() * 0.5);
            let x_l = (t as i64) % 3 == 2;
            // continuous values every tick, clicks only on change (like the headset)
            let mut entries = vec![
                ButtonEntry { path_id: *RIGHT_TRIGGER_VALUE_ID, value: ButtonValue::Scalar(trigger_r) },
                ButtonEntry { path_id: *LEFT_SQUEEZE_VALUE_ID, value: ButtonValue::Scalar(squeeze_l) },
                ButtonEntry { path_id: *LEFT_THUMBSTICK_X_ID, value: ButtonValue::Scalar(stick.0) },
                ButtonEntry { path_id: *LEFT_THUMBSTICK_Y_ID, value: ButtonValue::Scalar(stick.1) },
            ];
            for (i, (path, on)) in [(*RIGHT_A_CLICK_ID, a_r), (*LEFT_X_CLICK_ID, x_l)].into_iter().enumerate() {
                let v = ButtonValue::Binary(on);
                let changed = match (&last_buttons[i], &v) {
                    (Some(ButtonValue::Binary(p)), ButtonValue::Binary(n)) => p != n,
                    _ => true,
                };
                if changed {
                    entries.push(ButtonEntry { path_id: path, value: ButtonValue::Binary(on) });
                    last_buttons[i] = Some(v);
                }
            }
            ctx.send_buttons(entries);
            shared.lock().controller_log.push(format!(
                "{},{},{},{},{},{},{},{},{},{},{},{},{}",
                ts.as_nanos(), trigger_r, squeeze_l, a_r as u8, stick.0, stick.1, x_l as u8, rpos.x, rpos.y, rpos.z, lpos.x, lpos.y, lpos.z
            ));
        }
        ctx.send_tracking(
            ts,
            motions,
            [None, None],
            FaceData { eye_gazes: [None, None], fb_face_expression: None, htc_eye_expression: None, htc_lip_expression: None },
        );
        next += Duration::from_secs_f32(1.0 / fps / 3.0);
        thread::sleep(next.saturating_duration_since(Instant::now()));
    }
}
