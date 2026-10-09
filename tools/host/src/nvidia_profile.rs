//! Hardcoded NVIDIA encoder profile. This project targets NVENC only, so every "video encoder" parameter ALVR would normally
//! interpret from the session is fixed here; everything else in the session (bitrate, buffering, foveation geometry, controllers,
//! network, ...) is still read from session.json and handled by ALVR's server_core as usual.
//!
//! The profile is applied in two places so server_core (which negotiates codec/bit depth/range with the client) and the NVENC
//! code (which reads `openvr_config`-style numeric keys) always agree:
//!  * `apply_to_session`: patches `session_settings.video` on disk before ALVR initialises, logging every value it overrides.
//!  * `NVH_KEYS`: pushed into the encoder through `nvh_set`.
//! To tune an encoder parameter, change it here (one place). Current values equal the author's session baseline.
use serde_json::{json, Value};
use std::path::Path;

/// (path below `session_settings.video`, forced value)
fn session_profile() -> Vec<(&'static str, Value)> {
    vec![
        ("preferred_codec.variant", json!("Hevc")),
        ("encoder_config.rate_control_mode.variant", json!("Cbr")),
        ("encoder_config.h264_profile.variant", json!("High")),
        ("encoder_config.entropy_coding.variant", json!("Cabac")),
        ("encoder_config.filler_data", json!(false)),
        ("encoder_config.enable_vbaq", json!(false)),
        ("encoder_config.use_10bit", json!(true)),
        ("encoder_config.server_overrides_use_10bit", json!(true)),
        ("encoder_config.use_full_range", json!(true)),
        ("encoder_config.server_overrides_use_full_range", json!(true)),
        ("encoder_config.hdr.enable_hdr", json!(false)),
        ("encoder_config.hdr.server_overrides_enable_hdr", json!(true)),
        ("encoder_config.hdr.force_hdr_srgb_correction", json!(false)),
        ("encoder_config.hdr.clamp_hdr_extended_range", json!(false)),
        ("encoder_config.nvenc.quality_preset.variant", json!("P3")),
        ("encoder_config.nvenc.tuning_preset.variant", json!("LowLatency")),
        ("encoder_config.nvenc.multi_pass.variant", json!("Disabled")),
        ("encoder_config.nvenc.adaptive_quantization_mode.variant", json!("Spatial")),
        ("encoder_config.nvenc.low_delay_key_frame_scale", json!(-1)),
        ("encoder_config.nvenc.refresh_rate", json!(-1)),
        ("encoder_config.nvenc.enable_intra_refresh", json!(false)),
        ("encoder_config.nvenc.intra_refresh_period", json!(-1)),
        ("encoder_config.nvenc.intra_refresh_count", json!(-1)),
        ("encoder_config.nvenc.max_num_ref_frames", json!(-1)),
        ("encoder_config.nvenc.gop_length", json!(-1)),
        ("encoder_config.nvenc.p_frame_strategy", json!(-1)),
        ("encoder_config.nvenc.rate_control_mode", json!(-1)),
        ("encoder_config.nvenc.rc_buffer_size", json!(-1)),
        ("encoder_config.nvenc.rc_initial_delay", json!(-1)),
        ("encoder_config.nvenc.rc_max_bitrate", json!(-1)),
        ("encoder_config.nvenc.rc_average_bitrate", json!(-1)),
        ("encoder_config.nvenc.enable_weighted_prediction", json!(false)),
        ("encoder_config.software.force_software_encoding", json!(false)),
    ]
}

/// Numeric keys for the NVENC code (same names as ALVR's `openvr_config`). Booleans are 0/1.
pub const NVH_KEYS: &[(&str, i64)] = &[
    ("codec", 1), // HEVC
    ("use_10bit_encoder", 1),
    ("use_full_range_encoding", 1),
    ("enable_hdr", 0),
    ("nvenc_quality_preset", 3), // P3
    ("rate_control_mode", 0),    // CBR
    ("filler_data", 0),
    ("entropy_coding", 0), // CABAC
    ("nvenc_tuning_preset", 2), // low latency
    ("nvenc_multi_pass", 0),
    ("nvenc_adaptive_quantization_mode", 1), // spatial
    ("nvenc_low_delay_key_frame_scale", -1),
    ("nvenc_refresh_rate", -1),
    ("enable_intra_refresh", 0),
    ("intra_refresh_period", -1),
    ("intra_refresh_count", -1),
    ("max_num_ref_frames", -1),
    ("gop_length", -1),
    ("p_frame_strategy", -1),
    ("nvenc_rate_control_mode", -1),
    ("rc_buffer_size", -1),
    ("rc_initial_delay", -1),
    ("rc_max_bitrate", -1),
    ("rc_average_bitrate", -1),
    ("nvenc_enable_weighted_prediction", 0),
];

/// QP delta map (HEVC per-CTB QP offsets): lower QP in the foveal region, higher in the periphery; rate control keeps the bitrate,
/// so the center gains quality at the periphery's expense. Overridable at run time: `--qp-map on|off` or env `VISIONALVR_QP_MAP=0|1`.
pub const QP_MAP_ENABLED: bool = true;
pub const QP_MAP_CENTER_DELTA: i64 = -3;
pub const QP_MAP_EDGE_DELTA: i64 = 6;
/// width of the center->edge ramp, in units of the center region's half size (0.3 = hard edge, 1.0 = gentle)
pub const QP_MAP_TRANSITION: f64 = 1.0;

/// Encode profiles (`--encode-profile`): which frame NVENC gets.
///  * `session`    (default): whatever the session says (today: ALVR foveated encoding + the QP map).
///  * `foveated`:  forces ALVR foveated encoding on (written to the session, it stays on afterwards).
///  * `full-split`: the whole 2 x eye frame, no foveation warp, no QP map, NVENC split-frame encoding forced to 3 strips (the
///    5090 has 3 engines). Costs NVENC pixels (~3.2x at 3552x3200/eye) and bitrate per pixel, and the headset decodes the full
///    7104x3200; split encode should absorb the encode time. Foveation stays off in the session until `foveated` is used.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum EncodeProfile {
    Session,
    Foveated,
    FullSplit,
}

impl EncodeProfile {
    pub fn parse(s: &str) -> Result<Self, String> {
        match s {
            "session" => Ok(Self::Session),
            "foveated" => Ok(Self::Foveated),
            "full-split" | "full_split" => Ok(Self::FullSplit),
            o => Err(format!("--encode-profile session|foveated|full-split, got {o}")),
        }
    }
}

/// Writes the profile's foveation choice into the session (before server_core reads it). Returns what changed.
pub fn apply_encode_profile(path: &Path, profile: EncodeProfile) -> Result<Vec<String>, String> {
    let want = match profile {
        EncodeProfile::Session => return Ok(vec![]),
        EncodeProfile::Foveated => true,
        EncodeProfile::FullSplit => false,
    };
    let mut s: Value = serde_json::from_str(&std::fs::read_to_string(path).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    let node = &mut s["session_settings"]["video"]["foveated_encoding"]["enabled"];
    if node.as_bool() == Some(want) {
        return Ok(vec![]);
    }
    let old = node.clone();
    *node = json!(want);
    std::fs::write(path, serde_json::to_string_pretty(&s).unwrap()).map_err(|e| e.to_string())?;
    Ok(vec![format!("foveated_encoding.enabled: session={old} forced={want}")])
}

/// Forces the profile into the session file. Returns "path: session=<old> forced=<new>" for each value that differed.
pub fn apply_to_session(path: &Path) -> Result<Vec<String>, String> {
    let mut s: Value = serde_json::from_str(&std::fs::read_to_string(path).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    let mut changed = vec![];
    for (p, v) in session_profile() {
        let mut node = &mut s["session_settings"]["video"];
        let keys: Vec<&str> = p.split('.').collect();
        for k in &keys[..keys.len() - 1] {
            node = &mut node[*k];
        }
        let last = keys[keys.len() - 1];
        if node[last] != v {
            changed.push(format!("{p}: session={} forced={}", node[last], v));
            node[last] = v;
        }
    }
    if !changed.is_empty() {
        std::fs::write(path, serde_json::to_string_pretty(&s).unwrap()).map_err(|e| e.to_string())?;
    }
    Ok(changed)
}
