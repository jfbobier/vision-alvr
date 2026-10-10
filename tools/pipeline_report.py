#!/usr/bin/env python3
"""End-to-end frame timeline report for one debug session folder (debug mode on).

    python3 tools/pipeline_report.py logs/pilot3/<session>

Joins, on the shim/host shared clock (QPC seconds), the per-frame CSVs:
  shim_<exe>_<pid>_frames.csv  VDXR trace (xrWaitFrame .. xrEndFrame, async submit) + compose/stamp/publish (one per app)
  host_frames.csv              tick -> intake -> encode -> send, per frame counter
  tracking.csv                 headset tracking packets: arrival, step between samples, PLL residual
  client_frames.csv            the headset's per-frame statistics (ALVR GraphStatistics), by receive time
and prints stage durations (p50 / p95 / max), cadence, duplicates, warp, and the client-side late-frame picture.
"""
import csv, glob, os, statistics, sys

def load(path):
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    out = []
    for r in rows:
        try:
            out.append({k: (float(v) if v not in ('', None) else 0.0) for k, v in r.items()})
        except ValueError:
            pass  # a repeated header line (the shim re-opens its CSV when the debug folder is re-announced)
    return out

def pct(v, q):
    if not v:
        return float('nan')
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]

def dist(name, v, unit='ms', scale=1000.0):
    v = [x * scale for x in v if x == x]
    if not v:
        print(f'  {name:42s} (no data)')
        return
    print(f'  {name:42s} p50 {pct(v, .5):7.2f}  p95 {pct(v, .95):7.2f}  max {max(v):8.2f} {unit}  n={len(v)}')

def main(d):
    hf = load(os.path.join(d, 'host_frames.csv')) if os.path.exists(os.path.join(d, 'host_frames.csv')) else []
    tr = load(os.path.join(d, 'tracking.csv')) if os.path.exists(os.path.join(d, 'tracking.csv')) else []
    cf = load(os.path.join(d, 'client_frames.csv')) if os.path.exists(os.path.join(d, 'client_frames.csv')) else []
    shims = sorted(glob.glob(os.path.join(d, 'shim_*_frames.csv')))
    print(f'session {d}: host frames {len(hf)}, tracking samples {len(tr)}, client reports {len(cf)}, shim CSVs {len(shims)}')

    if tr:
        gaps = [b['t_arrival'] - a['t_arrival'] for a, b in zip(tr, tr[1:]) if 0 < b['t_arrival'] - a['t_arrival'] < 0.2]
        print('\nTracking (headset -> host)')
        dist('packet spacing', gaps)
        dist('PLL residual (arrival - grid)', [r['pll_residual_ms'] for r in tr], scale=1.0)
        steps = [r['step_mdeg'] for r in tr]
        print(f'  {"head step per sample (mdeg)":42s} p05 {pct(steps, .05):7.1f}  p50 {pct(steps, .5):7.1f}  p95 {pct(steps, .95):7.1f}')
        mm = [r['step_mm'] for r in tr]
        print(f'  {"head step per sample (mm)":42s} p05 {pct(mm, .05):7.2f}  p50 {pct(mm, .5):7.2f}  p95 {pct(mm, .95):7.2f}')

    for path in shims:
        sf = load(path)
        if not sf:
            continue
        print(f'\nApp frame timeline: {os.path.basename(path)} ({len(sf)} frames)')
        # the host's frame counter restarts for every app: join this shim CSV with the host segment that overlaps it in time
        segs, cur = [], []
        for r in hf:
            if cur and r['frame'] < cur[-1]['frame']:
                segs.append(cur)
                cur = []
            cur.append(r)
        if cur:
            segs.append(cur)
        t0, t1 = min(r['t_submit'] for r in sf), max(r['t_submit'] for r in sf)
        seg = max(segs, key=lambda g: sum(1 for r in g if t0 - 1 <= r['t_intake'] <= t1 + 1), default=[])
        byframe = {int(r['frame']): r for r in seg}
        have_trace = [r for r in sf if r['t_wait_ret'] > 0]
        print(f'  frames with VDXR trace: {len(have_trace)} / {len(sf)}')
        if have_trace:
            dist('xrWaitFrame return -> xrBeginFrame', [r['t_begin'] - r['t_wait_ret'] for r in have_trace if r['t_begin'] > 0])
            dist('xrBeginFrame -> xrEndFrame (app CPU frame)', [r['t_xr_end'] - r['t_begin'] for r in have_trace if r['t_begin'] > 0 and r['t_xr_end'] > 0])
            dist('xrEndFrame -> layers submitted (precompose)', [r['t_end_submit'] - r['t_xr_end'] for r in have_trace if r['t_end_submit'] > 0 and r['t_xr_end'] > 0])
            dist('submitted -> async ovr_EndFrame', [r['t_async_end'] - r['t_end_submit'] for r in have_trace if r['t_async_end'] > 0 and r['t_end_submit'] > 0])
            dist('display time asked - xrWaitFrame return', [r['pred_display'] - r['t_wait_ret'] for r in have_trace if r['pred_display'] > 0])
            dist('locate display - end display (should be 0)', [r['locate_display'] - r['end_display'] for r in have_trace if r['locate_display'] > 0 and r['end_display'] > 0])
            print(f'  {"xrLocateViews calls per frame":42s} p50 {pct([r["n_locate"] for r in have_trace], .5):7.0f}  max {max(r["n_locate"] for r in have_trace):7.0f}')
        dist('shim SubmitFrame -> GPU done (compose)', [r['t_gpu_done'] - r['t_submit'] for r in sf if r['t_gpu_done'] > 0])
        dist('GPU done -> published', [r['t_publish'] - r['t_gpu_done'] for r in sf if r['t_gpu_done'] > 0])
        joined = [(r, byframe[int(r['frame'])]) for r in sf if int(r['frame']) in byframe]
        print(f'  frames joined with the host: {len(joined)}')
        if joined:
            dist('published -> host intake (IPC)', [h['t_intake'] - r['t_publish'] for r, h in joined if r['t_publish'] > 0])
            dist('tick -> shim SubmitFrame (app render)', [r['t_submit'] - h['t_tick'] for r, h in joined if h['t_tick'] > 0])
            dist('intake -> send (encode + hold)', [h['t_send'] - h['t_intake'] for r, h in joined])
            dist('tick -> send', [h['t_send'] - h['t_tick'] for r, h in joined if h['t_tick'] > 0])
        dist('stamp newer than rendered pose', [(r['ts_stamp_ns'] - r['ts_render_ns']) / 1e9 for r in sf if r['ts_stamp_ns'] >= r['ts_render_ns'] > 0])
        dist('warp (mdeg)', [r['warp_mdeg'] for r in sf], unit='mdeg', scale=1.0)
        dist('fresh wait', [r['fresh_wait_ms'] for r in sf if r['fresh_wait_ms'] > 0], scale=1.0)
        dups = sum(1 for r in sf if r['dup'] > 0)
        print(f'  duplicate timestamps: {dups} / {len(sf)} ({100.0 * dups / max(1, len(sf)):.1f}%)')
        subs = sorted(r['t_submit'] for r in sf)
        dist('submit interval', [b - a for a, b in zip(subs, subs[1:]) if 0 < b - a < 0.2])

    if hf:
        print('\nHost send cadence')
        sends = sorted(r['t_send'] for r in hf)
        dist('send interval', [b - a for a, b in zip(sends, sends[1:]) if 0 < b - a < 0.2])
        ins = sorted(r['t_intake'] for r in hf)
        dist('intake (slot boundary) spacing', [b - a for a, b in zip(ins, ins[1:]) if 0 < b - a < 0.2])
        dist('intake -> send (encode + hold)', [r['t_send'] - r['t_intake'] for r in hf if r['t_send'] >= r['t_intake']])
        ticks = sorted(set(r['t_tick'] for r in hf if r['t_tick'] > 0))
        dist('tick interval', [b - a for a, b in zip(ticks, ticks[1:]) if 0 < b - a < 0.2])
        if tr:
            # tick phase vs the nearest preceding tracking arrival
            arr = sorted(r['t_arrival'] for r in tr)
            import bisect
            ph = []
            for t in ticks:
                i = bisect.bisect_right(arr, t) - 1
                if i >= 0:
                    ph.append(t - arr[i])
            dist('tick after latest tracking packet', ph)

    cg = load(os.path.join(d, 'client_graph.csv')) if os.path.exists(os.path.join(d, 'client_graph.csv')) else []
    if cf and 'ts_ns' in cf[0]:
        print('\nHeadset per-frame statistics (raw, by frame timestamp)')
        for k in ('frame_interval_ms', 'decode_ms', 'decoder_queue_ms', 'client_comp_ms', 'total_ms'):
            dist(k, [r[k] for r in cf], scale=1.0)
        vq = [r['vsync_queue_ms'] for r in cf]
        sane = [v for v in vq if 0 <= v < 1000]
        print(f'  {"vsync_queue (sane)":42s} p05 {pct(sane, .05):7.2f}  p50 {pct(sane, .5):7.2f}  p95 {pct(sane, .95):7.2f} ms; late (wrapped): {len(vq) - len(sane)}')
        shown = [int(r['ts_ns']) for r in cf]
        import collections
        sst = collections.Counter()
        for a, b in zip(shown, shown[1:]):
            sst[max(-2, min(4, round((b - a) / 11.111e6)))] += 1
        tot = sum(sst.values())
        print('  displayed-frame stamp step: ' + ', '.join(f'{k:+d}: {v} ({100 * v / tot:.1f}%)' for k, v in sorted(sst.items())))
        if hf:
            sent = {int(r['ts_ns']) for r in hf if r['ts_ns'] > 0}
            print(f'  sent frames never reported displayed: {len(sent - set(shown))} / {len(sent)} ({100 * len(sent - set(shown)) / max(1, len(sent)):.1f}%)')
            tsend = {int(r['ts_ns']): r['t_send'] for r in hf}
            rt = [r['t_recv'] - tsend[int(r['ts_ns'])] for r in cf if int(r['ts_ns']) in tsend]
            dist('video send -> statistics arrival (round trip)', rt)
        recv = sorted(r['t_recv'] for r in cf)
        dist('report interval (displayed-frame spacing)', [b - a for a, b in zip(recv, recv[1:]) if 0 < b - a < 0.2])
        print(f'  report gaps > 16.5 ms: {sum(1 for a, b in zip(recv, recv[1:]) if b - a > 0.0165)}')
        cf = cg
    if cf:
        print('\nHeadset per-frame statistics (what the client reports)')
        for k in ('total_ms', 'game_ms', 'network_ms', 'decode_ms', 'decoder_queue_ms', 'client_comp_ms'):
            dist(k, [r[k] for r in cf], scale=1.0)
        vq = [r['vsync_queue_ms'] for r in cf]
        sane = [v for v in vq if 0 <= v < 1000]
        late = len(vq) - len(sane)
        print(f'  {"vsync_queue (sane)":42s} p05 {pct(sane, .05):7.2f}  p50 {pct(sane, .5):7.2f}  p95 {pct(sane, .95):7.2f} ms')
        print(f'  late frames (vsync_queue wrapped: the client finished after its vsync): {late} / {len(vq)} ({100.0 * late / max(1, len(vq)):.1f}%)')
        recv = sorted(r['t_recv'] for r in cf)
        dist('report interval (displayed-frame spacing)', [b - a for a, b in zip(recv, recv[1:]) if 0 < b - a < 0.2])
        gaps2 = sum(1 for a, b in zip(recv, recv[1:]) if b - a > 0.0165)
        print(f'  report gaps > 16.5 ms (a display frame without a new stream frame): {gaps2}')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
