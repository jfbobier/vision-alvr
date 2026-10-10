#!/usr/bin/env python3
"""Live harness: build -> deploy -> stream to the real headset -> measure, unattended, from WSL.

The Apple Vision Pro keeps its ALVR connection while the streamer restarts (the client reconnects by itself), so a
sweep of host/shim settings can run without anyone in the headset: each variant starts the host in the PC's
interactive session, optionally launches a game (or uses the host's synthetic 90 fps source), streams for N seconds,
stops everything, fetches the debug session and scores it with the same KPIs as tools/pipeline_report.py.

    python3 harness/live.py check                       # PC reachable, headset paired, Steam/game paths
    python3 harness/live.py build host|shim|all         # upload the working tree's sources and build on the PC
    python3 harness/live.py deploy                      # install host + shim + VDXR from the PC's build outputs
    python3 harness/live.py run harness/live_variants.json [--only name,name] [--seconds 60]
    python3 harness/live.py score logs/pilot3/<session>  # KPIs of an existing session folder

Variants (JSON): {"name": ..., "game": "synthetic"|"none"|"dead_space"|"cyberpunk"|"eoa", "settings": {...video keys...},
"session": {"max_prediction_ms": 0, "bitrate_mbps": 300}, "seconds": 60, "auto_input": "20:rA,25:rA"}.
Results: harness/results/live/<timestamp>_<name>.json and a summary table at the end.
Stdlib only; needs key-based ssh to the PC (see PC below).
"""
import csv, json, os, statistics, subprocess, sys, time, glob
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PC = os.environ.get("VISIONALVR_PC", "jean-@192.168.1.2")
CLONE = r"C:\Users\jean-\vision-alvr"
INSTALL = r"D:\_VR\VisionALVR"
LOGS_LOCAL = ROOT / "logs" / "pilot3"
RESULTS = ROOT / "harness" / "results" / "live"
SCRATCH = Path(os.environ.get("VISIONALVR_SCRATCH", "/tmp/visionalvr-live"))
GAMES = {
    # exe name (for taskkill / shim log), how to launch (steam app id or a path), default scripted input
    # EA app version: launching the exe starts the EA app itself (it must be logged in); the mod loads with the game
    "dead_space": {"exe": "Dead Space.exe", "path": r"C:\Program Files\EA Games\Dead Space (2023)\Dead Space.exe", "auto_input": "20:rA,26:rA", "settle_s": 45},
    "cyberpunk": {"exe": "Cyberpunk2077.exe", "path": r"C:\Games\Cyberpunk 2077\bin\x64\Cyberpunk2077.exe", "steam": 1091500, "auto_input": "", "settle_s": 60},
    "eoa": {"exe": "EndOfAbyss.exe", "path": r"D:\End of Abyss\EndOfAbyss\Binaries\Win64\EndOfAbyss.exe", "auto_input": "", "settle_s": 40},
}
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", PC]


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def pc(cmd, timeout=120):
    """Run a cmd.exe command line on the PC; returns (rc, text)."""
    p = subprocess.run(SSH + ["cmd /c " + cmd], capture_output=True, timeout=timeout)
    out = (p.stdout + p.stderr).decode(errors="replace").replace("\x00", "").replace("\r", "")
    out = "\n".join(l for l in out.splitlines() if "post-quantum" not in l and "store now" not in l and "openssh.com" not in l)
    return p.returncode, out


def ps(script, timeout=300):
    """Run a PowerShell script (text) on the PC through stdin; returns its output."""
    p = subprocess.run(SSH + ["powershell -NoProfile -Command -"], input=script.encode(), capture_output=True, timeout=timeout)
    out = (p.stdout + p.stderr).decode(errors="replace").replace("\x00", "").replace("\r", "")
    return "\n".join(l for l in out.splitlines() if "post-quantum" not in l and "store now" not in l and "openssh.com" not in l)


def put(local, remote):
    subprocess.run(["scp", "-o", "BatchMode=yes", "-q", str(local), f"{PC}:{remote}"], check=True, timeout=300)


def get_dir(remote, local):
    Path(local).mkdir(parents=True, exist_ok=True)
    subprocess.run(["scp", "-o", "BatchMode=yes", "-q", "-r", f"{PC}:{remote}/*", str(local) + "/"], timeout=600)


# ---------------------------------------------------------------------------------------------------------------------
def check():
    rc, out = pc("echo pc-ok & ver")
    if "pc-ok" not in out:
        sys.exit(f"PC unreachable: {out[:200]}")
    print(ps(f"""
'host elevated?'; ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole('Administrators')
'streamer running?'; [bool](Get-Process alvr_host,VisionALVR -ErrorAction SilentlyContinue)
'steam running?'; [bool](Get-Process steam -ErrorAction SilentlyContinue)
'install:'; Get-Item '{INSTALL}\\alvr_host.exe','{INSTALL}\\LibOVRRT64_1.dll','{INSTALL}\\virtualdesktop-openxr.dll' | Select-Object Name, LastWriteTime | Format-Table -AutoSize | Out-String
'runtime registered:'; (Get-ItemProperty 'HKLM:\\SOFTWARE\\Khronos\\OpenXR\\1' -ErrorAction SilentlyContinue).ActiveRuntime
'paired headset:'; Select-String -Path '{INSTALL}\\logs\\VisionALVR.log' -Pattern 'paired headset' | Select-Object -Last 1 | ForEach-Object {{ $_.Line }}
"""))


def build(what):
    files = {
        "host": [(f"tools/host/src/{m}", "tools/host/src/") for m in ("main.rs", "pipeline.rs", "discovery.rs", "vision.rs", "logs.rs")]
                + [("nvenc/hostlib/nvh.cpp", "nvenc/hostlib/"), ("nvenc/hostlib/nvh.h", "nvenc/hostlib/"), ("tools/host/build.rs", "tools/host/"),
                   ("tools/host/Cargo.toml", "tools/host/"),
                   ("ovrshim/ipc.h", "ovrshim/"), ("ovrshim/ipc_win.h", "ovrshim/")],
        "shim": [(f, "ovrshim/") for f in sorted(glob.glob(str(ROOT / "ovrshim" / "*"))) if Path(f).is_file()],
    }
    targets = ["host", "shim"] if what == "all" else [what]
    for t in targets:
        for f, d in files[t]:
            put(ROOT / f if not str(f).startswith("/") else f, f"{CLONE}/{d}".replace("\\", "/"))
    bld = " ".join(targets)
    log(f"building {bld} on the PC")
    out = ps(f"""
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
Set-Location '{CLONE}'
powershell -ExecutionPolicy Bypass -File build.ps1 {bld} 2>&1 | Select-String -Pattern 'RESULT|FAILED|== '
Get-Content logs\\host_build.log -ErrorAction SilentlyContinue | Select-String -Pattern '^error' | Select-Object -First 5
Get-Content logs\\ovrshim_build.log -ErrorAction SilentlyContinue | Select-String -Pattern 'error C' | Select-Object -First 5
""", timeout=900)
    print(out)
    if "RESULT: OK" not in out:
        sys.exit("build failed")


def deploy():
    out = ps(f"""
if (Get-Process alvr_host,VisionALVR -ErrorAction SilentlyContinue) {{ 'STREAMER RUNNING'; exit 1 }}
$inst = '{INSTALL}'; $src = '{CLONE}\\src'
$stamp = Get-Date -Format 'HHmm'
$bak = "$inst\\..\\VisionALVR_backup_$(Get-Date -Format yyyyMMdd)"
New-Item -ItemType Directory -Force $bak | Out-Null
Copy-Item "$inst\\alvr_host.exe" "$bak\\alvr_host_$stamp.exe" -Force
Copy-Item "$inst\\LibOVRRT64_1.dll" "$bak\\LibOVRRT64_1_$stamp.dll" -Force
Copy-Item "$src\\ALVR-v20.14.1\\target\\release\\alvr_host.exe" "$inst\\alvr_host.exe" -Force
Copy-Item "$src\\VDXR\\bin-shim\\x64\\Release\\LibOVRRT64_1.dll" "$inst\\LibOVRRT64_1.dll" -Force
Copy-Item "$src\\VDXR\\bin\\x64\\Release\\virtualdesktop-openxr.dll" "$inst\\virtualdesktop-openxr.dll" -Force
Get-Item "$inst\\alvr_host.exe", "$inst\\LibOVRRT64_1.dll", "$inst\\virtualdesktop-openxr.dll" | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize | Out-String
""")
    print(out)


BASE_SETTINGS = {
    "display": {"gamma": 1.2}, "headset": {"ip": "192.168.1.14"}, "debug": True,
    # the staged pipeline (tools/host/src/pipeline.rs): phase-paced sends, shim running start 2 ms, realtime GPU scheduling class
    "video": {"split_encode": "3", "send_pacing": "phase", "running_start_ms": 2.0, "boundary_offset_ms": 0, "gpu_priority": 0, "gpu_sched_class": 5},
}


def write_settings(video_overrides, session_overrides):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    s = json.loads(json.dumps(BASE_SETTINGS))
    s["video"].update(video_overrides or {})
    f = SCRATCH / "visionalvr.json"
    f.write_text(json.dumps(s, indent=4))  # no BOM: the host rejects one
    put(f, f"{INSTALL}/config/visionalvr.json".replace("\\", "/"))
    if session_overrides:
        # edit the ALVR session on the PC in place (ConvertFrom-Json keeps unknown keys)
        sets = []
        if "max_prediction_ms" in session_overrides:
            sets.append(f"$j.session_settings.headset.max_prediction_ms = {int(session_overrides['max_prediction_ms'])}")
        if "bitrate_mbps" in session_overrides:
            sets.append(f"$j.session_settings.video.bitrate.mode.ConstantMbps = {int(session_overrides['bitrate_mbps'])}")
        ps(f"""
$p = '{INSTALL}\\config\\session.json'
$j = Get-Content $p -Raw | ConvertFrom-Json
{chr(10).join(sets)}
[IO.File]::WriteAllText($p, ($j | ConvertTo-Json -Depth 32), (New-Object Text.UTF8Encoding $false))
""")


def start_host(auto_input="", synthetic=False, seconds=120):
    """Starts alvr_host in the interactive session, elevated (GPU scheduling class, same IPC namespace as the GUI launch).
    synthetic: the host's own 90 fps test-pattern source (--live, no game, no shim): a perfectly regular stream that isolates
    the headset + network side; the run ends by itself after `seconds`."""
    extra = f" --auto-input {auto_input}" if auto_input else ""
    mode = f"--live --seconds {seconds}" if synthetic else "--shim --daemon"
    cmd = f'{INSTALL}\\alvr_host.exe --install-dir "{INSTALL}" {mode} --debug{extra}'
    ps(f"""
schtasks /delete /tn visionalvr_live_host /f 2>$null | Out-Null
schtasks /create /tn visionalvr_live_host /tr '{cmd}' /sc once /st 00:00 /rl HIGHEST /it /f | Out-Null
schtasks /run /tn visionalvr_live_host | Out-Null
""")
    t0 = time.time()
    while time.time() - t0 < 60:
        out = ps(f"Get-Content '{INSTALL}\\logs\\VisionALVR.log' -Tail 8")
        if "headset connected" in out and "host start" in out:
            log("host up, headset connected")
            return True
        time.sleep(3)
    log("host did not report a connected headset within 60 s")
    return False


def start_game(game):
    g = GAMES[game]
    if g.get("path"):
        launch = f'start "" "{g["path"]}"'
    else:
        launch = f'start "" "steam://rungameid/{g["steam"]}"'
    ps(f"""
schtasks /delete /tn visionalvr_live_game /f 2>$null | Out-Null
schtasks /create /tn visionalvr_live_game /tr 'cmd /c {launch}' /sc once /st 00:00 /rl LIMITED /it /f | Out-Null
schtasks /run /tn visionalvr_live_game | Out-Null
""")
    t0 = time.time()
    while time.time() - t0 < 90:
        out = ps(f"[bool](Get-Process -Name '{Path(g['exe']).stem}' -ErrorAction SilentlyContinue)")
        if "True" in out:
            log(f"{g['exe']} running")
            return True
        time.sleep(3)
    log(f"{g['exe']} did not start")
    return False


def stop_all(game):
    if game in GAMES:
        ps(f"Stop-Process -Name '{Path(GAMES[game]['exe']).stem}' -Force -ErrorAction SilentlyContinue")
        time.sleep(3)
    ps("Stop-Process -Name alvr_host -Force -ErrorAction SilentlyContinue; Stop-Process -Name VisionALVR -Force -ErrorAction SilentlyContinue; "
       "schtasks /delete /tn visionalvr_live_host /f 2>$null | Out-Null; schtasks /delete /tn visionalvr_live_game /f 2>$null | Out-Null")
    time.sleep(2)


def newest_session():
    rc, out = pc(f"dir /b /o-d {INSTALL}\\logs\\debug")
    names = [l.strip() for l in out.splitlines() if l.strip()[:8].isdigit()]
    return names[0] if names else None


# ---------------------------------------------------------------------------------------------------------------------
def rows(path):
    if not Path(path).exists():
        return []
    out = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                out.append({k: float(v) for k, v in r.items()})
            except (ValueError, TypeError):
                pass
    return out


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def score(session_dir):
    """KPIs of a debug session folder (the numbers that separate SteamVR + ALVR from us)."""
    d = Path(session_dir)
    hf = rows(d / "host_frames.csv")
    cf = rows(d / "client_frames.csv")
    tr = rows(d / "tracking.csv")
    k = {"session": d.name, "host_frames": len(hf), "client_reports": len(cf)}
    if hf:
        hf = hf[90:] if len(hf) > 180 else hf  # skip the connection settling
        sends = sorted(r["t_send"] for r in hf)
        si = [(b - a) * 1000 for a, b in zip(sends, sends[1:]) if 0 < b - a < 0.2]
        k.update(send_fps=round(len(hf) / max(1e-9, sends[-1] - sends[0]), 1), send_p50=round(pct(si, .5), 2), send_p95=round(pct(si, .95), 2),
                 send_gaps_pct=round(100 * sum(1 for x in si if x > 16.5) / max(1, len(si)), 1),
                 encode_p50=round(pct([r["encode_ms"] for r in hf], .5), 2), encode_p95=round(pct([r["encode_ms"] for r in hf], .95), 2))
        ts = [int(r["ts_ns"]) for r in hf if r["ts_ns"] > 0]
        dup = sum(1 for a, b in zip(ts, ts[1:]) if b == a)
        k["dup_pct"] = round(100 * dup / max(1, len(ts)), 1)
        if "t_boundary" in hf[0]:
            # pipeline: send phase after the compositor boundary (flat = regular arrival at the headset), hold before sending
            ph = [(r["t_send"] - r["t_boundary"]) * 1000 for r in hf if r["t_boundary"] > 0]
            k.update(phase_p50=round(pct(ph, .5), 2), phase_p95=round(pct(ph, .95), 2), hold_p50=round(pct([r["hold_ms"] for r in hf], .5), 2))
    if cf and "frame_interval_ms" in cf[0]:
        fi = [r["frame_interval_ms"] for r in cf]
        k["client_repeats_pct"] = round(100 * sum(1 for x in fi if x > 16.5) / len(fi), 1)
        dq = [r["decoder_queue_ms"] for r in cf]
        k.update(dwell_p05=round(pct(dq, .05), 1), dwell_p50=round(pct(dq, .5), 1), dwell_p95=round(pct(dq, .95), 1),
                 total_p50=round(pct([r["total_ms"] for r in cf], .5), 1))
        shown = [int(r["ts_ns"]) for r in cf]
        steps = [round((b - a) / 11.111e6) for a, b in zip(shown, shown[1:])]
        k["client_skip_pct"] = round(100 * sum(1 for s in steps if s >= 2) / max(1, len(steps)), 1)
    if tr:
        t = [r["t_arrival"] for r in tr]
        g = [(b - a) * 1000 for a, b in zip(t, t[1:]) if 0 < b - a < 0.2]
        k.update(tracking_p50=round(pct(g, .5), 2), tracking_p95=round(pct(g, .95), 2), tracking_bunched_pct=round(100 * sum(1 for x in g if x < 8) / max(1, len(g)), 1))
    # host_events window stats: slot counters
    ev = d / "host_events.jsonl"
    if ev.exists():
        same_ts = 0
        last_pipeline = None
        for line in open(ev):
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("event") == "encoder_stats":
                same_ts += int(e["data"].get("same_ts_frames", 0) or 0)
                if e["data"].get("pipeline"):
                    last_pipeline = e["data"]["pipeline"]
            if e.get("event") == "gpu_scheduling":
                k["gpu_sched"] = f'{e["data"].get("class")} status {e["data"].get("status")}'
        k["same_ts_frames"] = same_ts
        if last_pipeline:
            # cumulative counters of the staged pipeline (the last window carries the totals)
            for key in ("slots_filled", "slots_empty", "superseded", "encoder_overruns", "send_late"):
                k[key] = last_pipeline.get(key)
            sh = last_pipeline.get("shim") or {}
            k.update(app_frame_ms=sh.get("app_frame_ms"), releases_late=sh.get("releases_late"), slot_busy_waits=sh.get("slot_busy_waits"))
    return k


SUMMARY_COLS = ["name", "game", "send_fps", "send_p95", "send_gaps_pct", "phase_p50", "phase_p95", "slots_filled", "slots_empty", "superseded",
                "encoder_overruns", "send_late", "releases_late", "app_frame_ms", "client_repeats_pct", "client_skip_pct", "dwell_p05", "dwell_p50",
                "dwell_p95", "total_p50", "encode_p95", "tracking_p95", "gpu_sched"]


def run(variants_file, only=None, seconds=None):
    variants = json.load(open(variants_file))
    if only:
        variants = [v for v in variants if v["name"] in only]
    RESULTS.mkdir(parents=True, exist_ok=True)
    results = []
    for v in variants:
        name, game = v["name"], v.get("game", "synthetic")
        secs = seconds or v.get("seconds", 60)
        log(f"=== variant {name}: game={game} {secs}s settings={v.get('settings')} session={v.get('session')}")
        stop_all(game)
        write_settings(v.get("settings"), v.get("session"))
        auto = v.get("auto_input", GAMES.get(game, {}).get("auto_input", ""))
        if not start_host(auto, synthetic=(game == "synthetic"), seconds=secs + 40):
            stop_all(game)
            results.append({"name": name, "game": game, "error": "no headset"})
            continue
        if game in GAMES:
            if not start_game(game):
                stop_all(game)
                results.append({"name": name, "game": game, "error": "game did not start"})
                continue
            settle = GAMES[game]["settle_s"]
            log(f"settling {settle} s (menus / scripted input)")
            time.sleep(settle)
        log(f"streaming {secs} s")
        time.sleep(secs)
        sess = newest_session()
        stop_all(game)
        if not sess:
            results.append({"name": name, "game": game, "error": "no session"})
            continue
        local = LOGS_LOCAL / sess
        get_dir(f"{INSTALL}/logs/debug/{sess}".replace("\\", "/"), local)
        k = score(local)
        k.update(name=name, game=game, settings=v.get("settings"), session_overrides=v.get("session"))
        (RESULTS / f"{time.strftime('%Y%m%d-%H%M%S')}_{name}.json").write_text(json.dumps(k, indent=2))
        results.append(k)
        print("  " + " ".join(f"{c}={k.get(c)}" for c in SUMMARY_COLS if c in k))
    print("\nSUMMARY (SteamVR + ALVR reference: send_p95 11.7, client_repeats 3.4%, dwell 9-11 ms)")
    print(" | ".join(SUMMARY_COLS))
    for k in results:
        print(" | ".join(str(k.get(c, "")) for c in SUMMARY_COLS))


def main(argv):
    if not argv:
        print(__doc__)
        return
    cmd = argv[0]
    if cmd == "check":
        check()
    elif cmd == "build":
        build(argv[1] if len(argv) > 1 else "all")
    elif cmd == "deploy":
        deploy()
    elif cmd == "score":
        print(json.dumps(score(argv[1]), indent=2))
    elif cmd == "run":
        only = None
        secs = None
        if "--only" in argv:
            only = argv[argv.index("--only") + 1].split(",")
        if "--seconds" in argv:
            secs = int(argv[argv.index("--seconds") + 1])
        run(argv[1], only, secs)
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
