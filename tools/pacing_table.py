# Tabulates the pacing fields of a debug session: python3 tools/pacing_table.py logs/<session>/host_events.jsonl
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
for r in rows:
    if r.get("event") == "pacing_config":
        d = r["data"]
        print("send_pacing:", d.get("send_pacing"), "| stamp:", d.get("stamp"), "fresh_wait_ms:", d.get("fresh_wait_ms"), "| pacing:", d.get("pacing"), "guard_ms:", d.get("pacing_guard_ms"))
    if r.get("event") != "encoder_stats":
        continue
    d = r["data"]; c = d.get("client") or {}
    if not c.get("samples") or d["frames"] < 100:
        continue
    g = d.get("tracking_gap_ms") or {}; sp = d.get("send_after_tick_ms") or {}; h = d.get("send_hold_ms") or {}
    afi = d.get("app_frame_interval_ms") or {}
    pll = d.get("tracking_pll") or {}; tat = d.get("tick_after_tracking_ms") or {}
    hs = d.get("head_step_mdeg") or {}; hm = d.get("head_step_mm") or {}
    def f(x, w=5, p=1):
        return f"{x:{w}.{p}f}" if isinstance(x, (int, float)) else " " * w
    print(f'{r["t_ms"]/1000:7.1f}s sent {f(d["fps"])} n{d["frames"]:3d} dupTs {d.get("same_ts_frames","-"):>3} | disp {f(c.get("displayed_fps"))} smp {c.get("samples"):3d} '
          f'game {f(c.get("game_ms"))} net {f(c.get("network_ms"),4)} dq {f(c.get("decoder_queue_ms"),4)} late {c.get("vsync_late","-"):>2} | '
          f'trk p50 {f(g.get("p50"),4)} p95 {f(g.get("p95"),4)} max {f(g.get("max"),5)} >12 {g.get("over_12ms","-"):>2} >15 {g.get("over_15ms","-"):>2} | '
          f'send+tick p50 {f(sp.get("p50"),4)} p95 {f(sp.get("p95"),4)} | hold p50 {f(h.get("p50"),4)} late {h.get("late_frames","-"):>3} | '
          f'pll {"L" if pll.get("locked") else "-"} {f(pll.get("period_ms"),5,2)}ms tick-trk p50 {f(tat.get("p50"),4,2)} p95 {f(tat.get("p95"),4,2)} | '
          f'head step mdeg p05 {f(hs.get("p05"),5)} p50 {f(hs.get("p50"),5)} mm p05 {f(hm.get("p05"),4,2)} p50 {f(hm.get("p50"),4,2)} | app p50 {f(afi.get("p50"),5,2)} p95 {f(afi.get("p95"),5,2)}')
