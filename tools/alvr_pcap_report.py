#!/usr/bin/env python3
"""Frame delivery report from a packet capture of the ALVR stream (UDP, default port 9944), for ANY ALVR server:
SteamVR + ALVR and VisionALVR alike, so the two can be compared on the same footing.

    python3 tools/alvr_pcap_report.py capture.pcapng [--pc 192.168.1.2] [--hmd 192.168.1.14]

Capture on the PC with tools/remote/pktmon_capture.ps1 (pktmon, 160-byte snaplen is enough). Parsed streams (ALVR 20.x
shard prefix: u32 BE length, u16 BE stream id, u32 BE packet index, u32 BE shard count, u32 BE shard index, then the
bincode header): video (id 3: timestamp + idr) from the PC, tracking (id 0: target timestamp) and statistics (id 4:
the headset's per-frame report) from the headset. Times are the capture clock.
"""
import struct, sys, collections, statistics

def read_pcapng(path):
    data = open(path, 'rb').read()
    pos = 0; n = len(data)
    endian = '<'; tsres = 1e-6; linktype = 1
    while pos + 12 <= n:
        btype, blen = struct.unpack_from(endian + 'II', data, pos)
        if btype == 0x0A0D0D0A:  # section header
            magic = struct.unpack_from('<I', data, pos + 8)[0]
            endian = '<' if magic == 0x1A2B3C4D else '>'
            btype, blen = struct.unpack_from(endian + 'II', data, pos)
        elif btype == 1:  # interface description
            linktype = struct.unpack_from(endian + 'H', data, pos + 8)[0]
            # options: if_tsresol (9)
            o = pos + 16
            while o + 4 <= pos + blen - 4:
                code, olen = struct.unpack_from(endian + 'HH', data, o)
                if code == 0: break
                if code == 9 and olen >= 1:
                    v = data[o + 4]
                    tsres = 2.0 ** -(v & 0x7F) if v & 0x80 else 10.0 ** -v
                o += 4 + ((olen + 3) & ~3)
        elif btype == 6:  # enhanced packet
            iface, tsh, tsl, caplen, origlen = struct.unpack_from(endian + 'IIIII', data, pos + 8)
            ts = ((tsh << 32) | tsl) * tsres
            yield ts, data[pos + 28:pos + 28 + caplen], origlen, linktype
        if blen < 12: break
        pos += blen

def udp_payload(frame, linktype):
    if linktype == 1:  # ethernet
        if len(frame) < 34: return None
        et = struct.unpack_from('!H', frame, 12)[0]; off = 14
        if et == 0x8100:
            et = struct.unpack_from('!H', frame, 16)[0]; off = 18
        if et != 0x0800: return None
    elif linktype in (101, 228):  # raw ip
        off = 0
    else:
        return None
    ihl = (frame[off] & 0x0F) * 4
    if frame[off + 9] != 17: return None
    src = '.'.join(str(b) for b in frame[off + 12:off + 16]); dst = '.'.join(str(b) for b in frame[off + 16:off + 20])
    u = off + ihl
    sport, dport, ulen = struct.unpack_from('!HHH', frame, u)
    return src, dst, sport, dport, frame[u + 8:], ulen - 8

def duration(b, o):
    secs, nanos = struct.unpack_from('<QI', b, o)
    return secs + nanos / 1e9

def pct(v, q):
    if not v: return float('nan')
    s = sorted(v); return s[min(len(s) - 1, int(q * len(s)))]

def dist(name, v, unit='ms', scale=1000.0):
    v = [x * scale for x in v]
    if not v: print(f'  {name:46s} (no data)'); return
    print(f'  {name:46s} p50 {pct(v, .5):7.2f}  p95 {pct(v, .95):7.2f}  p99 {pct(v, .99):7.2f}  max {max(v):8.2f} {unit}  n={len(v)}')

def main(path, pc='192.168.1.2', hmd='192.168.1.14', port=9944):
    video = {}      # packet index -> dict(ts, t_first, t_last, shards, bytes)
    tracking = []   # (t_arrival, ts)
    stats = []      # (t_arrival, dict)
    other = collections.Counter(); total = 0; dupes = 0
    seen = set()  # pktmon logs the same packet once per network component it passes: keep the first copy of each shard
    for t, frame, origlen, lt in read_pcapng(path):
        u = udp_payload(frame, lt)
        if not u: continue
        src, dst, sport, dport, pl, ulen = u
        if port not in (sport, dport) or len(pl) < 18: continue
        plen, sid, pidx, shards, sidx = struct.unpack_from('!IHIII', pl, 0)
        key = (src, sid, pidx, sidx)
        if key in seen:
            dupes += 1
            continue
        seen.add(key)
        total += 1
        if src == pc and sid == 3:
            e = video.setdefault(pidx, {'ts': None, 't_first': t, 't_last': t, 'shards': 0, 'bytes': 0, 'idr': 0, 'count': shards})
            e['t_last'] = max(e['t_last'], t); e['shards'] += 1; e['bytes'] += ulen
            if sidx == 0 and len(pl) >= 18 + 13:
                e['ts'] = duration(pl, 18); e['idr'] = pl[30]
        elif src == hmd and sid == 0 and sidx == 0 and len(pl) >= 30:
            tracking.append((t, duration(pl, 18)))
        elif src == hmd and sid == 4 and sidx == 0 and len(pl) >= 18 + 84:
            f = [duration(pl, 18 + 12 * i) for i in range(7)]
            stats.append((t, dict(ts=f[0], frame_interval=f[1], decode=f[2], decoder_queue=f[3], rendering=f[4], vsync_queue=f[5], total=f[6])))
        else:
            other[(src == pc, sid)] += 1
    frames = sorted((e for e in video.values() if e['ts'] is not None), key=lambda e: e['t_first'])
    print(f'{path}: {total} ALVR packets ({dupes} duplicate copies dropped); video frames {len(frames)} ({sum(e["shards"] for e in frames)} shards, complete {sum(1 for e in frames if e["shards"] == e["count"])}), tracking {len(tracking)}, statistics {len(stats)}; other {dict(other)}')
    if not frames: return
    span = frames[-1]['t_first'] - frames[0]['t_first']
    print(f'  span {span:.1f} s, video {len(frames) / span:.1f} frames/s, {sum(e["bytes"] for e in frames) * 8 / span / 1e6:.0f} Mbps, tracking {len(tracking) / span:.1f}/s, statistics {len(stats) / span:.1f}/s')

    print('\nServer -> headset (video)')
    # spacing between consecutive packet indices only: a capture that dropped packets must not look like delivery gaps
    byidx = sorted((i, e) for i, e in video.items() if e['ts'] is not None)
    si = [b['t_first'] - a['t_first'] for (ia, a), (ib, b) in zip(byidx, byidx[1:]) if ib == ia + 1]
    miss = sum(ib - ia - 1 for (ia, _), (ib, _) in zip(byidx, byidx[1:]) if ib > ia)
    print(f'  capture completeness: {miss} video packets missing from the capture (index gaps)')
    dist('send interval (first shard to first shard)', si)
    print(f'  {"send intervals > 16.5 ms / < 6 ms":46s} {sum(1 for x in si if x > 0.0165)} / {sum(1 for x in si if x < 0.006)}')
    dist('frame on the wire (first to last shard)', [e['t_last'] - e['t_first'] for e in frames])
    steps = collections.Counter()
    for a, b in zip(frames, frames[1:]):
        k = round((b['ts'] - a['ts']) / 0.011111)
        steps[max(-2, min(4, k))] += 1
    tot = sum(steps.values())
    print('  stamp step (display periods): ' + ', '.join(f'{k:+d}: {v} ({100 * v / tot:.1f}%)' for k, v in sorted(steps.items())))
    if tracking:
        import bisect
        tt = [t for t, _ in tracking]
        ph = []
        for e in frames:
            i = bisect.bisect_right(tt, e['t_first']) - 1
            if i >= 0: ph.append(e['t_first'] - tt[i])
        dist('video send after the latest tracking arrival', ph)
        tsmap = {round(ts, 6): t for t, ts in tracking}
        age = [e['t_first'] - tsmap[round(e['ts'], 6)] for e in frames if round(e['ts'], 6) in tsmap]
        dist('stamp sample arrival -> video send (pose age)', age)

    print('\nHeadset -> server (tracking)')
    ti = [b - a for a, b in zip([t for t, _ in tracking], [t for t, _ in tracking][1:])]
    dist('tracking packet spacing', ti)
    tsi = [b - a for a, b in zip([s for _, s in tracking], [s for _, s in tracking][1:])]
    dist('tracking timestamp step', tsi)

    if stats:
        print('\nHeadset -> server (per displayed frame statistics)')
        for k in ('frame_interval', 'decode', 'decoder_queue', 'rendering', 'vsync_queue', 'total'):
            vals = [s[k] for _, s in stats]
            if k == 'vsync_queue':
                sane = [x for x in vals if 0 <= x < 1]
                print(f'  {"vsync_queue late (wrapped)":46s} {len(vals) - len(sane)} / {len(vals)}')
                vals = sane
            dist(k, vals)
        shown = [s['ts'] for _, s in stats]
        sst = collections.Counter()
        for a, b in zip(shown, shown[1:]):
            sst[max(-2, min(4, round((b - a) / 0.011111)))] += 1
        tot = sum(sst.values())
        print('  displayed-frame stamp step: ' + ', '.join(f'{k:+d}: {v} ({100 * v / tot:.1f}%)' for k, v in sorted(sst.items())))
        sent = {round(e['ts'], 6) for e in frames}
        shown_set = {round(x, 6) for x in shown}
        print(f'  sent frames never reported displayed: {len(sent - shown_set)} / {len(sent)} ({100 * len(sent - shown_set) / max(1, len(sent)):.1f}%)')
        ri = [b - a for a, b in zip([t for t, _ in stats], [t for t, _ in stats][1:])]
        dist('statistics arrival spacing (display cadence)', ri)
        print(f'  {"report gaps > 16.5 ms":46s} {sum(1 for x in ri if x > 0.0165)}')
        vmap = {round(e['ts'], 6): e['t_first'] for e in frames}
        rt = [t - vmap[round(s['ts'], 6)] for t, s in stats if round(s['ts'], 6) in vmap]
        dist('video send -> statistics arrival (round trip)', rt)

if __name__ == '__main__':
    a = sys.argv[1:]
    kw = {}
    if '--pc' in a: kw['pc'] = a[a.index('--pc') + 1]
    if '--hmd' in a: kw['hmd'] = a[a.index('--hmd') + 1]
    main(a[0], **kw)
