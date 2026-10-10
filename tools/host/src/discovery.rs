//! Headset discovery, ALVR's own two protocols carved out of its server (server_core/src/sockets.rs):
//!  - mDNS/Bonjour: the Apple Vision Pro client advertises `_alvr._tcp.local.` with TXT `device_id` (its ALVR hostname) and
//!    `protocol` (the ALVR protocol id string); it never sends a UDP broadcast, which is why a 9943 listener alone never finds it.
//!  - UDP: Quest-style clients broadcast a 56-byte packet to port 9943: "ALVR", zeros, protocol id (u64 LE at 16..24), hostname.
//! Results are reported as `headset_seen` events (hostname, ip, protocol, compatible, via) for the GUIs: `--discover <s>` runs
//! only this and exits; the daemon host runs the mDNS part alongside the stream so the launcher can show what is on the network.

use alvr_common::anyhow::Result;
use mdns_sd::{ServiceDaemon, ServiceEvent};
use serde_json::json;
use std::{
    collections::HashMap,
    net::UdpSocket,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, Debug)]
pub struct Seen {
    pub hostname: String,
    pub ip: String,
    pub protocol: String,
    pub compatible: bool,
    pub via: &'static str,
}

/// Browses until `stop` is set. `udp`: also listen for the broadcast packets on UDP 9943 (not while server_core runs: it owns
/// that port). Each (hostname, ip) is reported once per 10 s.
pub fn run(stop: Arc<AtomicBool>, udp: bool, on_seen: &dyn Fn(Seen)) -> Result<()> {
    let server_protocol = alvr_common::protocol_id();
    let server_protocol_u64 = alvr_common::protocol_id_u64();
    let daemon = ServiceDaemon::new()?;
    let receiver = daemon.browse(alvr_sockets::MDNS_SERVICE_TYPE)?;
    let socket = if udp {
        match UdpSocket::bind(("0.0.0.0", alvr_sockets::CONTROL_PORT)) {
            Ok(s) => {
                s.set_read_timeout(Some(Duration::from_millis(200))).ok();
                Some(s)
            }
            Err(e) => {
                crate::logs::general("WARN", &format!("discovery: cannot listen on UDP {} ({e}): mDNS only", alvr_sockets::CONTROL_PORT));
                None
            }
        }
    } else {
        None
    };
    let mut reported: HashMap<(String, String), Instant> = HashMap::new();
    let mut report = |s: Seen| {
        let key = (s.hostname.clone(), s.ip.clone());
        if reported.get(&key).map(|t| t.elapsed() < Duration::from_secs(10)).unwrap_or(false) {
            return;
        }
        reported.insert(key, Instant::now());
        on_seen(s);
    };
    let mut buf = [0u8; 64];
    while !stop.load(Ordering::Relaxed) {
        while let Ok(ev) = receiver.try_recv() {
            if let ServiceEvent::ServiceResolved(info) = ev {
                let hostname = info.get_property_val_str(alvr_sockets::MDNS_DEVICE_ID_KEY).unwrap_or_else(|| info.get_hostname()).to_string();
                let protocol = info.get_property_val_str(alvr_sockets::MDNS_PROTOCOL_KEY).unwrap_or("").to_string();
                let mut addrs: Vec<String> = info.get_addresses_v4().iter().map(|a| a.to_string()).collect();
                addrs.sort();
                let compatible = protocol == server_protocol;
                for ip in addrs {
                    report(Seen { hostname: hostname.clone(), ip, protocol: protocol.clone(), compatible, via: "mdns" });
                }
            }
        }
        match &socket {
            Some(s) => match s.recv_from(&mut buf) {
                Ok((n, from)) if n == alvr_sockets::HANDSHAKE_PACKET_SIZE_BYTES && &buf[..4] == b"ALVR" => {
                    let mut id = [0u8; 8];
                    id.copy_from_slice(&buf[16..24]);
                    let id = u64::from_le_bytes(id);
                    let end = buf[24..n].iter().position(|b| *b == 0).map(|p| 24 + p).unwrap_or(n);
                    let hostname = String::from_utf8_lossy(&buf[24..end]).to_string();
                    report(Seen { hostname, ip: from.ip().to_string(), protocol: id.to_string(), compatible: id == server_protocol_u64, via: "udp" });
                }
                _ => {}
            },
            None => thread::sleep(Duration::from_millis(200)),
        }
    }
    daemon.shutdown().ok();
    Ok(())
}

pub fn event_json(s: &Seen) -> serde_json::Value {
    json!({ "hostname": s.hostname, "ip": s.ip, "protocol": s.protocol, "compatible": s.compatible, "via": s.via,
            "server_protocol": alvr_common::protocol_id() })
}
