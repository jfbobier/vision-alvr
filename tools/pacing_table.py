# Tabulates the pacing fields of a debug session: python3 tools/pacing_table.py logs/<session>/host_events.jsonl
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
for r in rows:
    if r.get("event") == "nvenc_config":
        print("send_pacing:", r["data"].get("send_pacing"))
    if r.get("event") != "encoder_stats":
        continue
    d = r["data"]; c = d.get("client") or {}
    if not c.get("samples") or d["frames"] < 100:
        continue
    g = d.get("tracking_gap_ms") or {}; sp = d.get("send_after_tick_ms") or {}; h = d.get("send_hold_ms") or {}
    afi = d.get("app_frame_interval_ms") or {}
    def f(x, w=5, p=1):
        return f"{x:{w}.{p}f}" if isinstance(x, (int, float)) else " " * w
    print(f'{r["t_ms"]/1000:7.1f}s sent {f(d["fps"])} n{d["frames"]:3d} dupTs {d.get("same_ts_frames","-"):>3} | disp {f(c.get("displayed_fps"))} smp {c.get("samples"):3d} '
          f'game {f(c.get("game_ms"))} net {f(c.get("network_ms"),4)} dq {f(c.get("decoder_queue_ms"),4)} | '
          f'trk p50 {f(g.get("p50"),4)} p95 {f(g.get("p95"),4)} max {f(g.get("max"),5)} >12 {g.get("over_12ms","-"):>2} >15 {g.get("over_15ms","-"):>2} | '
          f'send+tick p50 {f(sp.get("p50"),4)} p95 {f(sp.get("p95"),4)} | hold p50 {f(h.get("p50"),4)} late {h.get("late_frames","-"):>3} | app p50 {f(afi.get("p50"),5,2)} p95 {f(afi.get("p95"),5,2)}')
