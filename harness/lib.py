"""Harness library: builder access, process orchestration, analysis, checks. Stdlib only."""
import hashlib, json, os, re, shutil, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # the repository root
BUILDER = ROOT / "tools" / "builder.sh"
RESULTS = ROOT / "harness" / "results"
CACHE = ROOT / "harness" / "cache"
HOST_EXE = r"src\ALVR-v20.14.1\target\release\alvr_host.exe"
CLIENT_EXE = r"src\ALVR-v20.14.1\target\release\alvr_client_headless.exe"
FFMPEG_WIN = "ffmpeg.exe"        # Windows ffmpeg via WSL interop, D3D11VA on the AMD iGPU
FFMPEG = "ffmpeg"                 # native software fallback
FFPROBE = "ffprobe"


class Blocked(Exception):
    """Infrastructure problem: the scenario could not be evaluated (not a product failure)."""


def sh(cmd, timeout=600, check=False, text=True):
    p = subprocess.run(cmd, capture_output=True, text=text, errors="replace", timeout=timeout)
    if check and p.returncode:
        raise Blocked(f"{cmd[:3]} failed ({p.returncode}): {p.stdout[-300:]} {p.stderr[-300:]}")
    return p


def b_run(cmd, timeout=600):
    """Run a cmd.exe command in ~/openxr on the builder."""
    p = subprocess.run(["bash", str(BUILDER), "run", cmd], capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode(errors="replace").replace("\x00", "").replace("\r", "")


def b_popen(cmd):
    return subprocess.Popen(["bash", str(BUILDER), "run", cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def b_put(local, remote_rel):
    sh(["bash", str(BUILDER), "put", str(local), remote_rel], check=True)


def b_get(remote_rel, local):
    Path(local).parent.mkdir(parents=True, exist_ok=True)
    p = sh(["bash", str(BUILDER), "get", remote_rel, str(local)])
    return p.returncode == 0


# ---------------------------------------------------------------------------------------------------------
def preflight(sc):
    notes = []
    rc, out = b_run("echo builder-ok")
    if "builder-ok" not in out:
        raise Blocked(f"builder unreachable (tools/builder.sh -> {out.strip()[:200]!r})")
    for exe in (HOST_EXE, CLIENT_EXE):
        rc, out = b_run(f"if exist {exe} (echo yes) else (echo no)")
        if "yes" not in out:
            raise Blocked(f"missing on builder: {exe} (run: python3 harness/run.py build)")
    if sc.get("source"):
        rc, out = b_run(f"if exist {sc['source'].replace('/', chr(92))} (echo yes) else (echo no)")
        if "yes" not in out:
            raise Blocked(f"missing source on builder: {sc['source']}")
    if shutil.which(FFPROBE) is None or shutil.which(FFMPEG) is None:
        raise Blocked("ffmpeg/ffprobe not installed in WSL")
    notes.append("builder ok")
    return notes


def kill_stale():
    for exe in ("alvr_host.exe", "alvr_client_headless.exe"):
        b_run(f"taskkill /f /im {exe} >nul 2>&1")


def derive_session(sc, dst: Path, pin=None):
    """pin = (hostname, ip): pre-trusted client reached by manual IP (discovery broadcasts do not cross subnets)."""
    s = json.load(open(ROOT / "harness" / "fixtures" / "session.user.json"))
    s["client_connections"] = {}
    if pin:
        s["client_connections"][pin[0]] = {"display_name": "Mock client", "current_ip": None, "manual_ips": [pin[1]],
                                           "trusted": True, "connection_state": "Disconnected"}
    disc = s["session_settings"]["connection"]["client_discovery"]
    disc["enabled"] = True
    disc["content"]["auto_trust_clients"] = True
    for path, val in sc.get("session_overrides", {}).items():     # "a.b.c": value
        d = s
        keys = path.split(".")
        for k in keys[:-1]:
            d = d[k]
        d[keys[-1]] = val
    oc = s["openvr_config"]
    if not oc.get("enable_foveated_encoding") and not sc.get("keep_cache_stale"):
        for k in [k for k in oc if k.startswith("foveation_")]:    # what ALVR itself persists when foveation is off
            oc[k] = 0.0
    dst.write_text(json.dumps(s, indent=1))


def argline(d, mapping):
    parts = []
    for k, flag in mapping.items():
        if k in d and d[k] is not None:
            v = d[k]
            if isinstance(v, bool):
                if v:
                    parts.append(flag)
            else:
                parts.append(f"{flag} {v}")
    return " ".join(parts)


def _builder_ip():
    """The builder's address as the mock client on the builder reaches the host (VISIONALVR_BUILDER_IP, else the host part of
    VISIONALVR_BUILDER / tools/builder.local)."""
    if os.environ.get("VISIONALVR_BUILDER_IP"):
        return os.environ["VISIONALVR_BUILDER_IP"]
    b = os.environ.get("VISIONALVR_BUILDER", "")
    loc = ROOT / "tools" / "builder.local"
    if not b and loc.exists():
        b = next((l.split("=", 1)[1].strip() for l in loc.read_text().splitlines() if l.startswith("VISIONALVR_BUILDER=")), "")
    return b.rsplit("@", 1)[-1] or "127.0.0.1"


BUILDER_IP = _builder_ip()
LOCAL_BIN = ROOT / "build" / "bin"


def builder_client_hostname():
    rc, out = b_run(r'type "%APPDATA%\ALVR\ALVR Client\session.json"')
    m = re.search(r'"hostname"\s*:\s*"([^"]+)"', out)
    if not m:       # generate the persistent client config by starting the client once
        b_run(f"{CLIENT_EXE} --connect-timeout 1 > nul 2>&1")
        rc, out = b_run(r'type "%APPDATA%\ALVR\ALVR Client\session.json"')
        m = re.search(r'"hostname"\s*:\s*"([^"]+)"', out)
    if not m:
        raise Blocked("cannot determine the mock client hostname on the builder")
    return m.group(1)


def win_scratch():
    """Windows-local scratch dir reachable from WSL (e.g. /mnt/c/Users/<u>/AppData/Local/Temp/openxr_harness)."""
    t = sh(["cmd.exe", "/c", "echo %LOCALAPPDATA%"]).stdout.strip().replace("\r", "")
    p = Path(sh(["wslpath", "-u", t]).stdout.strip()) / "Temp" / "openxr_harness"
    p.mkdir(parents=True, exist_ok=True)
    return p


class TaskProc:
    """Handle for a process started through a temporary LIMITED scheduled task in the interactive session."""
    def __init__(self, rid, task, rc_name):
        self.rid, self.task, self.rc_name, self.returncode = rid, task, rc_name, None

    def wait(self, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            rc, out = b_run(f"if exist runs\\{self.rid}\\{self.rc_name} (type runs\\{self.rid}\\{self.rc_name})")
            if out.strip().isdigit():
                self.returncode = int(out.strip())
                break
            time.sleep(2)
        b_run(f"schtasks /delete /tn {self.task} /f")
        if self.returncode is None:
            raise subprocess.TimeoutExpired(self.task, timeout)
        return self.returncode


def start_limited(rid, task, script_name, body, rc_name, env=None):
    """Starts `body` (cmd lines) non-elevated in the interactive session (needed for XR_RUNTIME_JSON and for D3D shared
    resources, which do not cross Windows sessions). The exit code is written to runs\\<rid>\\<rc_name>."""
    rc, home = b_run("echo %USERPROFILE%")
    home = home.strip().splitlines()[-1].strip()
    cmd = "@echo off\r\n" + "".join(f"set {k}={v}\r\n" for k, v in (env or {}).items()) + f"cd /d {home}\\openxr\r\n" + body + \
          f"echo %ERRORLEVEL% > runs\\{rid}\\{rc_name}\r\n"
    local = CACHE / script_name
    CACHE.mkdir(parents=True, exist_ok=True)
    local.write_text(cmd)
    b_put(local, f"runs/{rid}/{script_name}")
    b_run(f'schtasks /create /tn {task} /tr "cmd /c {home}\\openxr\\runs\\{rid}\\{script_name}" /sc once /st 00:00 /rl LIMITED /it /f')
    b_run(f"schtasks /run /tn {task}")
    return TaskProc(rid, task, rc_name)


def launch_app(app, rid, rd, log):
    """Runs the OpenXR app (xr_probe) against VDXR+OVRShim as a NON-elevated process: the SSH session is elevated and the
    OpenXR loader ignores XR_RUNTIME_JSON for elevated processes, so it goes through a temporary LIMITED scheduled task."""
    rc, home = b_run("echo %USERPROFILE%")
    home = home.strip().splitlines()[-1].strip()
    rt = app.get("runtime", r"out\vdxr-shim")
    cmd = ("@echo off\r\n"
           f"set XR_RUNTIME_JSON={home}\\openxr\\{rt}\\virtualdesktop-openxr.json\r\n"
           + "".join(f"set {k}={v}\r\n" for k, v in {"OVRSHIM_LOG": f"{home}\\openxr\\{rd}\\ovrshim.log", "XR_PROBE_TIMING_CSV": f"{home}\\openxr\\{rd}\\app_timing.csv", **app.get("env", {})}.items()) +
           f"cd /d {home}\\openxr\r\n"
           f"build\\probe\\xr_probe.exe {rd}\\app.log {app.get('frames', 300)} {app.get('timeout_s', 60)} {rd}\\app_pose.csv {(rd + chr(92) + 'app_input.csv') if app.get('input') else '-'}{(' ' + str(int(app['layer_test'])) if app.get('layer_test') else '')} > {rd}\\app_stdout.txt 2>&1\r\n"
           f"echo %ERRORLEVEL% > {rd}\\app.rc\r\n")
    local = CACHE / "app.cmd"
    CACHE.mkdir(parents=True, exist_ok=True)
    local.write_text(cmd)
    b_put(local, f"runs/{rid}/app.cmd")
    task = "openxr_app_tmp"
    b_run(f'schtasks /create /tn {task} /tr "cmd /c {home}\\openxr\\runs\\{rid}\\app.cmd" /sc once /st 00:00 /rl LIMITED /it /f')
    b_run(f"schtasks /run /tn {task}")
    log("  started OpenXR app (limited integrity)")
    t0 = time.time()
    done = False
    while time.time() - t0 < app.get("timeout_s", 60) + 40:
        rc, out = b_run(f"if exist runs\\{rid}\\app.rc (type runs\\{rid}\\app.rc)")
        if out.strip() and out.strip().isdigit():
            done = True
            break
        time.sleep(2)
    b_run(f"schtasks /delete /tn {task} /f")
    return {"app_finished": done, "app_wall_s": round(time.time() - t0, 1)}


def kill_local_stale():
    sh(["taskkill.exe", "/f", "/im", "alvr_host.exe"])


def topology(sc):
    t = sc.get("topology", {"host": "wsl", "client": "builder"})
    if t["host"] == t["client"] == "builder" and sc.get("host") is not None and not t.get("client_bind_ip"):
        raise Blocked("host and client cannot share a machine without topology.client_bind_ip (both bind UDP 9943/9944, os error 10048)")
    return t


def run_scenario(sc, outdir: Path, log=print):
    outdir.mkdir(parents=True, exist_ok=True)
    rid = outdir.name
    rd = f"runs\\{rid}"
    steps = {}
    t0 = time.time()
    topo = topology(sc)

    steps["preflight"] = preflight(sc)
    kill_stale()
    kill_local_stale()
    b_run(f"mkdir {rd}")
    h, c = sc.get("host"), sc.get("client", {})

    pin = None
    if h is not None and topo["client"] == "builder":
        pin = (builder_client_hostname(), topo.get("client_bind_ip") or BUILDER_IP)
        steps["pinned_client"] = pin[0]
    derive_session(sc, outdir / "session.json", pin)
    b_put(outdir / "session.json", f"runs/{rid}/session.json")

    gpu_p = None
    if sc.get("gpu_sample"):
        # sample the NVIDIA GPU on the builder during the run (state, clocks, engine utilization, power, throttle reasons)
        gpu_p = b_popen(f'nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.video,clocks.mem,utilization.gpu,utilization.encoder,'
                        f'utilization.decoder,power.draw,temperature.gpu,clocks_throttle_reasons.active --format=csv -lms 500 > {rd}\\gpu.csv 2>&1')
    host_exe = HOST_EXE
    if sc.get("install"):
        # the portable folder exactly as a user unzips it (no registry: the app gets XR_RUNTIME_JSON); the host runs from it
        # with --install-dir, so config\ and logs\ are the folder's own
        inst = f"{rd}\\VisionALVR"
        rc, out = b_run(f"xcopy /E /I /Q /Y out\\dist\\VisionALVR {inst} && if exist {inst}\\alvr_host.exe echo COPIED")
        if "COPIED" not in out:
            raise Blocked("portable package missing (build it first: python3 harness/run.py build gui package): " + out[-300:])
        host_exe = f"{inst}\\alvr_host.exe"
        h = sc["host"] = {**sc["host"], "install_dir": f"%USERPROFILE%\\openxr\\{inst}"}
        if sc.get("app"):
            sc["app"] = {**sc["app"], "runtime": inst}
        steps["installed_from"] = "out\\dist\\VisionALVR (portable folder)"
    host_p = host_log = None
    if h is not None:
        if topo["host"] == "wsl":
            LOCAL_BIN.mkdir(parents=True, exist_ok=True)
            b_get(HOST_EXE.replace("\\", "/"), LOCAL_BIN / "alvr_host.exe")
            src_local = CACHE / Path(sc["source"]).name
            if not src_local.exists():
                source_md5(sc["source"])
            # Windows Information Protection blocks Windows processes from writing to \\wsl.localhost, so the host
            # works in a Windows-local scratch dir and the harness copies the results back from the WSL side.
            wdir = win_scratch() / rid
            wdir.mkdir(parents=True, exist_ok=True)
            shutil.copy(outdir / "session.json", wdir / "session.json")
            shutil.copy(src_local, wdir / "source.hevc")
            hostcmd = [str(LOCAL_BIN / "alvr_host.exe"), "--config-dir", win(wdir / "host_cfg"), "--session", win(wdir / "session.json"),
                       "--file", win(wdir / "source.hevc"), "--report", win(wdir / "host_report.json")]
            for k, flag in {"fps": "--fps", "seconds": "--seconds", "min_frames": "--min-frames", "connect_timeout": "--connect-timeout",
                            "exit_after_frames": "--exit-after-frames"}.items():
                if k in h:
                    hostcmd += [flag, str(h[k])]
            log("  starting host (WSL/interop)")
            host_log = open(outdir / "host_stdout.txt", "w")
            host_p = subprocess.Popen(hostcmd, stdout=host_log, stderr=subprocess.STDOUT)
        else:      # host on the builder (loopback topology: the client binds topology.client_bind_ip)
            src_arg = "" if (h.get("live") or h.get("shim")) else f'--file {sc["source"].replace("/", chr(92))} '
            dump_arg = f"--dump-qpmap {rd}\\qpmap.json " if h.get("dump_qpmap") else ""
            hostcmd = (f'{host_exe} --config-dir {rd}\\host_cfg --session {rd}\\session.json {src_arg}{dump_arg}'
                       f'--report {rd}\\host_report.json --frames-csv {rd}\\host_frames.csv '
                       + argline(h, {"fps": "--fps", "seconds": "--seconds", "min_frames": "--min-frames", "live": "--live", "shim": "--shim", "daemon": "--daemon", "split_encode": "--split-encode", "gpu_keepalive": "--gpu-keepalive",
                                     "width": "--width", "height": "--height", "noise": "--noise", "qp_map": "--qp-map",
                                     "idle_rgb": "--idle-rgb", "gpu_priority": "--gpu-priority", "encode_profile": "--encode-profile",
                                     "install_dir": "--install-dir", "debug": "--debug", "benchmark_network": "--benchmark-network", "bench_step_s": "--bench-step-s",
                                     "connect_timeout": "--connect-timeout", "exit_after_frames": "--exit-after-frames",
                                     "send_pacing": "--send-pacing", "running_start_ms": "--running-start-ms", "boundary_offset_ms": "--boundary-offset-ms",
                                     "gpu_sched_class": "--gpu-sched-class"})
                       + f' > {rd}\\host_stdout.txt 2>&1')
            if topo.get("host_launch") == "interactive":
                log("  starting host (builder, interactive session, limited)")
                host_p = start_limited(rid, "openxr_host_tmp", "host.cmd", hostcmd + "\r\n", "host.rc")
            else:
                log("  starting host (builder)")
                host_p = b_popen(hostcmd)
        time.sleep(sc.get("host_head_start_s", 3))

    env = f'set ALVR_BIND_IP={topo["client_bind_ip"]}&& ' if topo.get("client_bind_ip") else ""
    for k, v in sc.get("client_env", {}).items():
        env += f"set {k}={v}&& "
    def make_client_cmd(sfx=""):
        return (env + f'{CLIENT_EXE} --report {rd}\\client{sfx}_report.json --frames-csv {rd}\\frames{sfx}.csv --out {rd}\\client{sfx}.hevc '
                + argline(c, {"seconds": "--seconds", "connect_timeout": "--connect-timeout", "min_frames": "--min-frames",
                              "exit_after_frames": "--exit-after-frames", "yaw_amp": "--yaw-amp", "yaw_hz": "--yaw-hz",
                              "refresh": "--refresh", "res": "--res", "decode_ms": "--decode-ms",
                              "no_10bit": "--no-10bit", "no_foveation": "--no-foveation", "controllers": "--controllers",
                              "views_after_first_frame": "--views-after-first-frame", "encoding_gamma": "--encoding-gamma",
                              "display_sim": "--display-sim", "poll_ms": "--poll-ms", "queue_max": "--queue-max", "motion": "--motion",
                              "noise_mdeg": "--noise-mdeg", "bunch": "--bunch"})
                + (f" --controllers-csv {rd}\\controllers.csv" if c.get("controllers") and not sfx else "")
                + (f" --display-csv {rd}\\display{sfx}.csv" if c.get("display_sim") else "")
                + f' > {rd}\\client{sfx}_stdout.txt 2>&1')
    clientcmd = make_client_cmd()
    client_p = b_popen(clientcmd)
    app = sc.get("app")
    if app:
        if app.get("delay_s"):
            time.sleep(app["delay_s"])     # let the stream run green first
        steps["app"] = launch_app(app, rid, rd, log)
    budget = sc.get("timeout_s", 120)
    try:
        client_p.wait(timeout=budget)
        if sc.get("reconnect"):
            # the headset went away and comes back (sleep, Wi-Fi drop, app restart): the daemon host must serve it again
            time.sleep(sc["reconnect"].get("gap_s", 3))
            log("  starting second client session (reconnect)")
            client2_p = b_popen(make_client_cmd("2"))
            client2_p.wait(timeout=budget)
        if host_p and not (h or {}).get("daemon"):
            host_p.wait(timeout=budget)
        elif host_p:
            kill_stale()       # a daemon never exits by itself
    except subprocess.TimeoutExpired:
        steps["timeout"] = True
        kill_stale()
        kill_local_stale()
    if host_log:
        host_log.close()
        wdir = win_scratch() / rid
        for f in ("host_report.json",):
            if (wdir / f).exists():
                shutil.copy(wdir / f, outdir / f)
        if (wdir / "host_cfg" / "session_log.txt").exists():
            shutil.copy(wdir / "host_cfg" / "session_log.txt", outdir / "host_session_log.txt")
        shutil.rmtree(wdir, ignore_errors=True)
    if gpu_p:
        b_run("taskkill /f /im nvidia-smi.exe >nul 2>&1")
    steps["wall_s"] = round(time.time() - t0, 1)
    log("  fetching artifacts")
    if h is not None and topo["host"] == "builder":
        for f in ("host_report.json", "host_stdout.txt", "host_frames.csv"):
            b_get(f"runs/{rid}/{f}", outdir / f)
        if sc.get("install"):
            b_get(f"runs/{rid}/VisionALVR/config/session.json", outdir / "host_session_final.json")
            b_get(f"runs/{rid}/VisionALVR/logs", outdir / "logs")      # VisionALVR.log, alvr.log, debug\<time>\...
        else:
            b_get(f"runs/{rid}/host_cfg/session_log.txt", outdir / "host_session_log.txt")
            b_get(f"runs/{rid}/host_cfg/session.json", outdir / "host_session_final.json")
        if h.get("dump_qpmap"):
            b_get(f"runs/{rid}/qpmap.json", outdir / "qpmap.json")   # what ALVR negotiated (openvr_config)
    if gpu_p:
        b_get(f"runs/{rid}/gpu.csv", outdir / "gpu.csv")
    if app:
        for f in ("app.log", "app_pose.csv", "app_input.csv", "app_timing.csv", "app_stdout.txt", "app.rc", "ovrshim.log"):
            b_get(f"runs/{rid}/{f}", outdir / f)
    for f in ("client_report.json", "client_stdout.txt", "frames.csv", "client.hevc"):
        b_get(f"runs/{rid}/{f}", outdir / f)
    if sc.get("reconnect"):
        for f in ("client2_report.json", "client2_stdout.txt", "frames2.csv", "client2.hevc"):
            b_get(f"runs/{rid}/{f}", outdir / f)
    if sc.get("client", {}).get("controllers"):
        b_get(f"runs/{rid}/controllers.csv", outdir / "controllers.csv")
    if sc.get("client", {}).get("display_sim"):
        b_get(f"runs/{rid}/display.csv", outdir / "display.csv")
    return steps


# ---------------------------------------------------------------------------------------------------------
def jload(p):
    try:
        return json.load(open(p))
    except Exception:
        return None


def report_from_stdout(path):
    """Fallback: the last {"event":"report"} JSON line printed by alvr_host."""
    try:
        for line in reversed(open(path, errors="replace").read().splitlines()):
            if '"event":"report"' in line.replace(" ", ""):
                return json.loads(line)["data"]
    except Exception:
        pass
    return None


def ffprobe_props(path):
    p = sh([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
            "stream=codec_name,profile,pix_fmt,width,height,color_range,color_space,color_transfer", "-of", "json", str(path)])
    try:
        return json.loads(p.stdout)["streams"][0]
    except Exception:
        return None


def win(path):
    return sh(["wslpath", "-w", str(path)]).stdout.strip()


def framemd5(path, hw=True):
    """Per-frame md5 list via Windows ffmpeg with D3D11VA (AMD iGPU), falling back to native software ffmpeg."""
    attempts = [([FFMPEG_WIN, "-hwaccel", "d3d11va"], win(path), "d3d11va"), ([FFMPEG], str(path), "software")] if hw else [([FFMPEG], str(path), "software")]
    for pre, arg, mode in attempts:
        if shutil.which(pre[0]) is None:
            continue
        p = sh(pre + ["-hide_banner", "-v", "error", "-i", arg, "-f", "framemd5", "-"], timeout=1800)
        sums = [l.split(",")[-1].strip() for l in p.stdout.splitlines() if l and not l.startswith("#")]
        if sums and "Error" not in p.stderr and "error" not in p.stderr.lower():
            return sums, mode, ""
        if sums:
            return sums, mode, p.stderr[:300]
    return [], "none", "decode failed"


def source_md5(source_rel):
    CACHE.mkdir(parents=True, exist_ok=True)
    local = CACHE / Path(source_rel.replace("\\", "/")).name
    if not local.exists():
        b_get(source_rel.replace("\\", "/"), local)
    key = CACHE / (local.name + "." + hashlib.sha1(str(local.stat().st_size).encode()).hexdigest()[:8] + ".md5.json")
    if key.exists():
        return json.load(open(key))
    sums, mode, err = framemd5(local)
    json.dump({"sums": sums, "mode": mode}, open(key, "w"))
    return {"sums": sums, "mode": mode}


def read_counters(path, block=128):
    """Decode the stamped 16-bit frame counter (16 blocks of block x block in the top-left) and the gray of the patch
    right of it, from every frame. Returns (counters, patch_values, decoder)."""
    vf = (f"split=2[a][b];[a]crop={16*block}:{block}:0:0,scale=16:1:flags=area,format=gray[c];"
          f"[b]crop={block}:{block}:{16*block}:0,scale=1:1:flags=area,format=gray,scale=16:1:flags=neighbor[g];[c][g]hstack")
    # software decode on purpose: the D3D11VA p010 download returns wrong values after crop/scale on this driver
    for pre, arg, mode in [([FFMPEG], str(path), "software")]:
        p = subprocess.run(pre + ["-hide_banner", "-v", "error", "-i", arg, "-filter_complex", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True, timeout=1800)
        data = p.stdout
        if data and len(data) % 32 == 0:
            vals, patch = [], []
            for i in range(0, len(data), 32):
                vals.append(sum((1 << b) for b in range(16) if data[i + b] > 127))
                patch.append(data[i + 16])
            return vals, patch, mode
    return [], [], "none"


# ---- ALVR foveated encoding oracle (independent Python port of FFR.cpp CalculateFoveationVars + the compress shader) ----
def fov_vars(eye_w, eye_h, cfg):
    """Port of FFR.cpp CalculateFoveationVars, including its float32 variables and double intermediates (the rounding decides whether
    the optimized size lands on a multiple of 32 or one step above it)."""
    import math
    import numpy as np
    f = np.float32
    tw, th = f(eye_w), f(eye_h)
    cx, cy = f(cfg["foveation_center_size_x"]), f(cfg["foveation_center_size_y"])
    sx, sy = f(cfg["foveation_center_shift_x"]), f(cfg["foveation_center_shift_y"])
    rx, ry = f(cfg["foveation_edge_ratio_x"]), f(cfg["foveation_edge_ratio_y"])
    d = float
    ex, ey = f(tw - f(cx * tw)), f(th - f(cy * th))
    cxa = f(1.0 - math.ceil(d(ex) / (d(rx) * 2.0)) * (d(rx) * 2.0) / d(tw))
    cya = f(1.0 - math.ceil(d(ey) / (d(ry) * 2.0)) * (d(ry) * 2.0) / d(th))
    exa, eya = f(tw - f(cxa * tw)), f(th - f(cya * th))
    sxa = f(math.ceil(d(f(sx * exa)) / (d(rx) * 2.0)) * (d(rx) * 2.0) / d(exa))
    sya = f(math.ceil(d(f(sy * eya)) / (d(ry) * 2.0)) * (d(ry) * 2.0) / d(eya))
    fsx = f(d(cxa) + (1.0 - d(cxa)) / d(rx))
    fsy = f(d(cya) + (1.0 - d(cya)) / d(ry))
    ow, oh = f(fsx * tw), f(fsy * th)
    owa = math.ceil(d(f(ow / f(32.0)))) * 32
    oha = math.ceil(d(f(oh / f(32.0)))) * 32
    ratio = (float(f(ow / f(owa))), float(f(oh / f(oha))))
    return {"opt_w": owa, "opt_h": oha, "ratio": ratio, "center": (float(cxa), float(cya)), "shift": (float(sxa), float(sya)),
            "edge": (float(rx), float(ry))}


def _fov_f(e, cs, sh, er):
    """The compress shader's output->source mapping for one axis (eye uv in, source uv out)."""
    c0 = (1.0 - cs) / 2.0
    c1 = (er - 1.0) * c0 * (sh + 1.0) / er
    c2 = (er - 1.0) * cs + 1.0
    lo = c0 * (sh + 1.0) / c2
    hi = c0 * (sh - 1.0) / c2 + 1.0
    center = e * c2 / er + c1
    d2, d3 = e * c2, (e - 1.0) * c2 + 1.0
    if e < lo:
        g1 = e / lo
        return g1 * center + (1.0 - g1) * d2
    if e > hi:
        g2 = (1.0 - e) / (1.0 - hi)
        return g2 * center + (1.0 - g2) * d3
    return center


def fov_forward(src, axis, v):
    """Source position (0..1 of the eye) -> position (0..1 of the optimized eye area) by inverting the monotone mapping."""
    cs, sh, er, ratio = v["center"][axis], v["shift"][axis], v["edge"][axis], v["ratio"][axis]
    lo, hi = 0.0, 1.0 / ratio
    for _ in range(60):
        mid = (lo + hi) / 2
        if _fov_f(mid, cs, sh, er) < src:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2 * ratio


def stamp_points(cfg, block, foveated):
    """Where the left eye's stamp (16 counter blocks + gray patch, 64px blocks at the eye's top-left) sits in the decoded frame."""
    ew, eh = cfg["eye_resolution_width"], cfg["eye_resolution_height"]
    srcs = [(block * i + block // 2, block // 2) for i in range(16)] + [(16 * block + block // 2, block // 2)]
    if not foveated:
        return [(x, y) for x, y in srcs]
    v = fov_vars(ew, eh, cfg)
    return [(int(fov_forward(x / ew, 0, v) * v["opt_w"]), int(fov_forward(y / eh, 1, v) * v["opt_h"])) for x, y in srcs]


def read_stamps_at(path, pts):
    """Per frame, thresholds a 2x2 average at each point: returns (counters, gray_patch_values). Software decode."""
    n = len(pts)
    chains = ";".join(f"[a{i}]crop=2:2:{max(x - 1, 0)}:{max(y - 1, 0)},scale=1:1:flags=area,format=gray[b{i}]" for i, (x, y) in enumerate(pts))
    vf = f"split={n}" + "".join(f"[a{i}]" for i in range(n)) + ";" + chains + ";" + "".join(f"[b{i}]" for i in range(n)) + f"hstack=inputs={n}"
    p = subprocess.run([FFMPEG, "-hide_banner", "-v", "error", "-i", str(path), "-filter_complex", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True, timeout=1800)
    d = p.stdout
    vals, patch = [], []
    if d and len(d) % n == 0:
        for i in range(0, len(d), n):
            vals.append(sum((1 << b) for b in range(16) if d[i + b] > 127))
            patch.append(d[i + 16])
    return vals, patch


# ---- composition-layer oracle: independent pinhole projection of where each layer's test points must appear ----------------
PHASE_FRAMES = 90
CUBE_FACE = {"+X": (255, 0, 0), "-X": (0, 255, 0), "+Y": (0, 0, 255), "-Y": (255, 255, 0), "+Z": (255, 0, 255), "-Z": (0, 255, 255)}
QUAD_COLORS = {"TL": (255, 0, 0), "TR": (0, 255, 0), "BL": (0, 0, 255), "BR": (255, 255, 255)}


def layer_expectations(eye_w, eye_h, tan_l, tan_r, tan_u, tan_d, cube=False):
    """[(phase, name, (x, y) in the decoded frame, expectation)], expectation = ('rgb', (r, g, b), tol) | ('dark',) | ('notred',)."""
    import math

    def px(X, Y, Z, eye=0):
        nx = (X / -Z) / (tan_r if X >= 0 else tan_l)
        ny = (Y / -Z) / (tan_u if Y >= 0 else tan_d)
        return int((nx * 0.5 + 0.5) * eye_w) + eye * eye_w, int((0.5 - ny * 0.5) * eye_h)

    def ndc_px(nx, ny, eye=0):
        return int((nx * 0.5 + 0.5) * eye_w) + eye * eye_w, int((0.5 - ny * 0.5) * eye_h)

    out = []
    quads = {"TL": (-1, 1), "TR": (1, 1), "BL": (-1, -1), "BR": (1, -1)}
    for q, (sx, sy) in quads.items():                     # P0: head-locked quad 1x1 at z=-2
        out.append((0, f"headlocked_quad_{q}", px(0.25 * sx, 0.25 * sy, -2.0), ("rgb", QUAD_COLORS[q], 45)))
    out.append((0, "headlocked_quad_outside", px(0.75, 0.0, -2.0), ("dark",)))
    a = math.radians(25.0)                                # P1: world-locked quad, centered (0.3, 0, -2) relative to the head, yawed 25 deg
    lx = (math.cos(a), 0.0, -math.sin(a))
    for q, (sx, sy) in quads.items():
        X = 0.3 + 0.25 * sx * lx[0]
        Z = -2.0 + 0.25 * sx * lx[2]
        out.append((1, f"worldlocked_quad_{q}", px(X, 0.25 * sy, Z), ("rgb", QUAD_COLORS[q], 45)))
    R, A, asp = 2.5, 1.2, 2.0                             # P2: cylinder centered on the head, radius 2.5, 1.2 rad, aspect 2 -> 1.5 m tall
    h = R * A / asp
    for q, (sx, sy) in quads.items():
        th = sx * A / 4
        out.append((2, f"cylinder_{q}", px(R * math.sin(th), sy * h / 4, -R * math.cos(th)), ("rgb", QUAD_COLORS[q], 45)))
    out.append((2, "cylinder_outside_top", px(0.0, h / 2 + 0.35, -R), ("dark",)))
    if cube:                                              # P3 (cube mode): faces seen in the middle and at the four edges
        out.append((3, "cube_forward_-Z", ndc_px(0.0, 0.0), ("rgb", CUBE_FACE["-Z"], 45)))         # P3: cube
        out.append((3, "cube_up_+Y", ndc_px(0.0, 0.9), ("rgb", CUBE_FACE["+Y"], 45)))
        out.append((3, "cube_down_-Y", ndc_px(0.0, -0.9), ("rgb", CUBE_FACE["-Y"], 45)))
        out.append((3, "cube_left_+X_(OVR_left_handed)", ndc_px(-0.9, 0.0), ("rgb", CUBE_FACE["+X"], 45)))
        out.append((3, "cube_right_-X_(OVR_left_handed)", ndc_px(0.9, 0.0), ("rgb", CUBE_FACE["-X"], 45)))
    else:                                                 # P3 (default): submission order
        out.append((3, "order_later_quad_on_top_center", px(0.0, 0.0, -2.0), ("rgb", (0, 255, 0), 45)))
        out.append((3, "order_earlier_quad_visible_outside", px(0.4, 0.4, -2.0), ("rgb", (255, 0, 0), 45)))
    out.append((4, "left_eye_only_quad_left", px(-0.25, 0.25, -2.0, 0), ("rgb", QUAD_COLORS["TL"], 45)))   # P4: eye visibility
    out.append((4, "left_eye_only_quad_right_eye_absent", px(-0.25, 0.25, -2.0, 1), ("notred",)))
    out.append((5, "alpha_blend_center", px(0.0, 0.0, -2.0), ("rgb", (255, 188, 188), 40)))           # P5: white 50% over red, blended in linear light
    out.append((5, "alpha_opaque_below", px(0.4, 0.4, -2.0), ("rgb", (255, 0, 0), 45)))
    return out


def read_rgb_at(path, pts, win=4):
    """Per decoded frame, the mean RGB of a win x win window at each point. Software decode (the D3D11VA download is unreliable here)."""
    n = len(pts)
    chains = ";".join(f"[a{i}]crop={win}:{win}:{max(x - win // 2, 0)}:{max(y - win // 2, 0)},scale=1:1:flags=area[b{i}]" for i, (x, y) in enumerate(pts))
    vf = f"split={n}" + "".join(f"[a{i}]" for i in range(n)) + ";" + chains + ";" + "".join(f"[b{i}]" for i in range(n)) + f"hstack=inputs={n},format=rgb24"
    p = subprocess.run([FFMPEG, "-hide_banner", "-v", "error", "-i", str(path), "-filter_complex", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, timeout=3600)
    d = p.stdout
    stride = 3 * n
    return [[tuple(d[o + 3 * i: o + 3 * i + 3]) for i in range(n)] for o in range(0, len(d) - stride + 1, stride)]


def qp_map_expected(cfg, w, h, foveated, c, e, t):
    """Independent Python port of nvh.cpp build_qp_map (per 32x32 CTB QP deltas). cfg = negotiated openvr_config."""
    import math
    cx, cy = cfg["foveation_center_size_x"], cfg["foveation_center_size_y"]
    sx, sy = cfg["foveation_center_shift_x"], cfg["foveation_center_shift_y"]
    rx = ry = 1.0
    ratio = (1.0, 1.0)
    if foveated:
        ew, eh = cfg["eye_resolution_width"], cfg["eye_resolution_height"]
        v = fov_vars(ew, eh, cfg)
        cx, cy = v["center"]
        sx, sy = v["shift"]
        rx, ry = cfg["foveation_edge_ratio_x"], cfg["foveation_edge_ratio_y"]
        ratio = v["ratio"]

    def axis(cs, sh, er):
        c0, c2 = (1 - cs) / 2, (er - 1) * cs + 1
        lo, hi = c0 * (sh + 1) / c2, c0 * (sh - 1) / c2 + 1
        s0, s1 = _fov_f(lo, cs, sh, er), _fov_f(hi, cs, sh, er)
        return (cs, sh, er), (s0 + s1) / 2, max(1e-4, (s1 - s0) / 2)
    ax, ay = axis(cx, sx, rx), axis(cy, sy, ry)
    cols, rows = (w + 31) // 32, (h + 31) // 32
    eye_w = w / 2
    out = []
    for j in range(rows):
        for i in range(cols):
            px, py = min((i + .5) * 32, w - .5), min((j + .5) * 32, h - .5)
            pe = px / eye_w if px < eye_w else (w - px) / eye_w
            exx, eyy = pe / ratio[0], (py / h) / ratio[1]
            sxs, sys_ = _fov_f(exx, *ax[0]), _fov_f(eyy, *ay[0])
            d = math.hypot((sxs - ax[1]) / ax[2], (sys_ - ay[1]) / ay[2])
            tt = min(1.0, max(0.0, (d - 1) / max(1e-3, t)))
            tt = tt * tt * (3 - 2 * tt)
            val = int(math.floor(c + (e - c) * tt + 0.5))
            out.append(max(-51, min(51, val)))
    return cols, rows, out


def region_detail(frame_gray, w, h, cols, rows, data, lo_val, hi_val):
    """Mean horizontal |difference| (retained detail) over CTBs whose delta equals lo_val (center) vs hi_val (edge)."""
    import numpy as np
    img = frame_gray.astype(np.int16).reshape(h, w)
    dx = np.abs(np.diff(img, axis=1))
    res = {}
    for name, val in (("center", lo_val), ("edge", hi_val)):
        acc, n = 0.0, 0
        for j in range(rows):
            for i in range(cols):
                if data[j * cols + i] == val:
                    blk = dx[j * 32:(j + 1) * 32, i * 32:min((i + 1) * 32, w - 1)]
                    if blk.size:
                        acc += blk.mean()
                        n += 1
        res[name] = (acc / n) if n else None
        res[name + "_ctbs"] = n
    return res


def negotiated_cfg(outdir):
    """openvr_config the oracles expect: what ALVR persisted after negotiating with the client (the input session's copy is only
    a cache and may be stale on purpose). Foveation geometry that ALVR zeroed (foveation off) is filled from the session's
    foveation settings, as the host does."""
    try:
        s = json.load(open(outdir / "host_session_final.json"))
    except (OSError, ValueError):
        s = json.load(open(outdir / "session.json"))
    cfg = dict(s["openvr_config"])
    fs = s.get("session_settings", {}).get("video", {}).get("foveated_encoding", {}).get("content", {})
    for k, sk in (("foveation_center_size_x", "center_size_x"), ("foveation_center_size_y", "center_size_y"), ("foveation_center_shift_x", "center_shift_x"),
                  ("foveation_center_shift_y", "center_shift_y"), ("foveation_edge_ratio_x", "edge_ratio_x"), ("foveation_edge_ratio_y", "edge_ratio_y")):
        if not cfg.get(k) and fs.get(sk):
            cfg[k] = fs[sk]
    return cfg


def analyse_layers(sc, outdir):
    cfg = negotiated_cfg(outdir)
    ew, eh = cfg["eye_resolution_width"], cfg["eye_resolution_height"]
    fov = None
    for line in open(outdir / "host_stdout.txt", errors="replace"):
        if '"views_config"' in line:
            try:
                fov = json.loads(line)["data"]["fov_tan"]
                break
            except Exception:
                pass
    if not fov:
        return {"error": "no views_config event with the field of view in host_stdout.txt"}
    tl, tr, tu, td = fov[0], fov[1], fov[2], fov[3]
    exps = layer_expectations(ew, eh, tl, tr, tu, td, cube=int(sc.get("app", {}).get("layer_test", 0)) == 2)
    stamp_pts = [(64 * i + 32, 32) for i in range(16)]
    pts = stamp_pts + [e[2] for e in exps]
    frames = read_rgb_at(outdir / "client.hevc", pts)
    results = {}
    for fr in frames:
        stamp = sum((1 << b) for b in range(16) if fr[b][1] > 127)
        if stamp < 1 or stamp > 6 * PHASE_FRAMES:
            continue
        phase, k = (stamp - 1) // PHASE_FRAMES, (stamp - 1) % PHASE_FRAMES
        if not (20 <= k <= 70):
            continue                                      # skip transitions between phases
        for j, (ph, name, pt, exp) in enumerate(exps):
            if ph != phase:
                continue
            r, g, b = fr[16 + j]
            if exp[0] == "rgb":
                ok = all(abs(c - e) <= exp[2] for c, e in zip((r, g, b), exp[1]))
            elif exp[0] == "dark":
                ok = max(r, g, b) < 110
            else:   # notred
                ok = r < 120
            res = results.setdefault(name, {"phase": ph, "n": 0, "ok": 0, "expected": exp, "samples": []})
            res["n"] += 1
            res["ok"] += 1 if ok else 0
            if len(res["samples"]) < 3:
                res["samples"].append((r, g, b))
    return {"decoded_frames": len(frames), "tangents": fov, "points": {n: r for n, r in results.items()}}


def read_frame_classes(path):
    """Classifies every decoded frame by its center pixel: 'G' = pure green (nothing running), 'A' = anything else."""
    vf = "crop=16:16:(iw-16)/2:(ih-16)/2,scale=1:1:flags=area,format=rgb24"
    p = subprocess.run([FFMPEG, "-hide_banner", "-v", "error", "-i", str(path), "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, timeout=1800)
    d = p.stdout
    out = []
    for i in range(0, len(d) - 2, 3):
        r, g, b = d[i], d[i + 1], d[i + 2]
        out.append("G" if g > 200 and r < 60 and b < 60 else "A")
    return out


def rle(seq, min_run=3):
    """Run-length encode, dropping runs shorter than min_run (stray/transition frames) and merging neighbors."""
    runs = []
    for c in seq:
        if runs and runs[-1][0] == c:
            runs[-1][1] += 1
        else:
            runs.append([c, 1])
    stray = sum(n for c, n in runs if n < min_run)
    kept = [[c, n] for c, n in runs if n >= min_run]
    merged = []
    for c, n in kept:
        if merged and merged[-1][0] == c:
            merged[-1][1] += n
        else:
            merged.append([c, n])
    return merged, stray


def read_csv(path):
    try:
        rows = [l.strip().split(",") for l in open(path).read().splitlines() if l.strip()]
        return rows[0], rows[1:]
    except Exception:
        return None, []


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0.0


def analyse_pacing(sc, outdir, period_ms):
    """Client-side arrival spacing of the app's frames (green idle frames and the first `warmup_frames` app frames excluded)."""
    warm = sc["checks"]["pacing"].get("warmup_frames", 30)
    _, hrows = read_csv(outdir / "host_frames.csv")        # frame,ts_ns,bytes,idr,encode_ms
    _, crows = read_csv(outdir / "frames.csv")             # ts_ns,arrival_ns,len,yaw
    app_ts = [r[1] for r in hrows if int(r[0]) >= 0]
    if not app_ts or not crows:
        return None
    first = next((i for i, c in enumerate(crows) if c[0] == app_ts[0]), None)
    if first is None:
        return None
    last = max((i for i, c in enumerate(crows) if c[0] == app_ts[-1]), default=len(crows) - 1)
    arr = [int(c[1]) for c in crows[first:last + 1]]
    d = [(y - x) / 1e6 for x, y in zip(arr, arr[1:])][warm:]
    if len(d) < 30:
        return None
    return {"period_ms": period_ms, "n": len(d), "p50": round(pct(d, .5), 2), "p95": round(pct(d, .95), 2), "p99": round(pct(d, .99), 2),
            "max": round(max(d), 2), "late": sum(x > 1.5 * period_ms for x in d), "late_frac": round(sum(x > 1.5 * period_ms for x in d) / len(d), 4)}


def analyse_display(sc, outdir, client, period_ms):
    """The emulated Vision Pro display (mock client --display-sim): repeats, skips, dwell. The same numbers the real headset's
    statistics give (client_frames.csv), so a loopback run and a headset run read alike."""
    d = (client or {}).get("display")
    if not d:
        return None
    out = dict(d)
    out["period_ms"] = period_ms
    out["dwell_spread_ms"] = round((d.get("dwell_ms") or {}).get("p95", 0) - (d.get("dwell_ms") or {}).get("p05", 0), 2)
    _, rows = read_csv(outdir / "display.csv")   # tick_ns,shown_ts_ns,repeat,dwell_ms,queue_len
    if rows:
        reps = [int(r[2]) for r in rows[90:]]
        runs, cur = [], 0     # runs of consecutive repeats (a stall) vs isolated ones (a missed slot)
        for x in reps:
            if x:
                cur += 1
            elif cur:
                runs.append(cur)
                cur = 0
        if cur:
            runs.append(cur)
        out["repeat_runs"] = {"n": len(runs), "max": max(runs) if runs else 0, "over_2": sum(1 for r in runs if r > 2)}
    return out


def analyse_pipeline(outdir):
    """The host's staged pipeline counters (the last encoder_stats window carries the totals)."""
    last = None
    p = outdir / "host_stdout.txt"
    for line in (open(p, errors="replace").read().splitlines() if p.exists() else []):
        if '"encoder_stats"' in line:
            try:
                e = json.loads(line)
                if e["data"].get("pipeline"):
                    last = e["data"]["pipeline"]
            except (ValueError, KeyError):
                pass
    if not last:
        return None
    out = {k: last.get(k) for k in ("send_pacing", "arrived", "superseded", "slots_filled", "slots_empty", "encoder_overruns", "send_late", "encode_envelope_ms")}
    out["shim"] = last.get("shim")
    out["gpu_scheduling"] = last.get("gpu_scheduling")
    filled = out.get("slots_filled") or 0
    out["superseded_frac"] = round((out.get("superseded") or 0) / max(1, (out.get("arrived") or 1)), 4)
    out["empty_frac"] = round((out.get("slots_empty") or 0) / max(1, filled + (out.get("slots_empty") or 0)), 4)
    return out


def analyse_app_timing(outdir, period_ms):
    """The game side (xr_probe XR_PROBE_TIMING_CSV): frame time = xrWaitFrame return to xrEndFrame return, and the release cadence."""
    _, rows = read_csv(outdir / "app_timing.csv")   # frame,display_time_ns,t_wait_return_ms,t_end_ms,load_factor
    if len(rows) < 60:
        return None
    rows = rows[30:]
    ft = [float(r[3]) - float(r[2]) for r in rows]
    rel = [float(b[2]) - float(a[2]) for a, b in zip(rows, rows[1:])]
    return {"n": len(ft), "frame_ms": {"p50": round(pct(ft, .5), 2), "p95": round(pct(ft, .95), 2), "max": round(max(ft), 2)},
            "release_interval_ms": {"p50": round(pct(rel, .5), 2), "p95": round(pct(rel, .95), 2), "max": round(max(rel), 2),
                                    "over_1p5_period": sum(1 for x in rel if x > 1.5 * period_ms)},
            "fps": round(1000.0 / max(1e-6, sum(rel) / max(1, len(rel))), 1)}


def analyse_qpmap(sc, outdir, host):
    out = {}
    qk = sc["checks"]["qpmap"]
    dump = jload(outdir / "qpmap.json")
    out["dump"] = bool(dump)
    cfgh = (host.get("nvenc_config") or {})
    out["nvenc_qp_map_mode"] = (cfgh.get("nvenc") if "nvenc" in cfgh else cfgh).get("qp_map_mode")
    out["qp"] = cfgh.get("qp_map")
    fr = cfgh.get("frame")
    if not fr:
        return out
    w, h, fov = fr[0], fr[1], bool(cfgh.get("foveated"))
    cfg = negotiated_cfg(outdir)
    cols, rows, exp = qp_map_expected(cfg, w, h, fov, qk["center"], qk["edge"], qk["transition"])
    out["expected_hist"] = {str(v): exp.count(v) for v in sorted(set(exp))}
    if dump:
        got = dump["data"]
        out["dims_ok"] = (cols, rows) == (dump["cols"], dump["rows"]) and len(got) == len(exp)
        out["mismatch"] = sum(1 for x, y in zip(exp, got) if x != y)
        out["ctbs"] = len(got)
        out["hist"] = {str(v): got.count(v) for v in sorted(set(got))}
    if qk.get("effect") and (outdir / "client.hevc").exists():
        n = qk["effect"].get("frame", 60)
        p = subprocess.run([FFMPEG, "-hide_banner", "-v", "error", "-i", str(outdir / "client.hevc"), "-vf", f"select=eq(n\\,{n})", "-frames:v", "1", "-pix_fmt", "gray", "-f", "rawvideo", "-"], capture_output=True, timeout=600)
        if len(p.stdout) == w * h:
            import numpy as np
            det = region_detail(np.frombuffer(p.stdout, dtype=np.uint8), w, h, cols, rows, exp, qk["center"], qk["edge"])
            out["detail"] = det
            if det.get("center") and det.get("edge"):
                out["detail_ratio"] = round(det["center"] / det["edge"], 3)
        else:
            out["detail_error"] = f"decoded {len(p.stdout)} bytes, expected {w * h}"
    return out


def analyse(sc, outdir: Path):
    a = {}
    a["host"] = jload(outdir / "host_report.json") or report_from_stdout(outdir / "host_stdout.txt")
    a["client"] = jload(outdir / "client_report.json")
    hs = (outdir / "host_stdout.txt").read_text(errors="replace") if (outdir / "host_stdout.txt").exists() else ""
    a["host_event_names"] = sorted(set(re.findall(r'"event":"([a-z_]+)"', hs)))
    a["bench_steps"] = [json.loads(l)["data"] for l in hs.splitlines() if '"event":"bench_step"' in l]
    lg = outdir / "logs"
    if lg.exists():
        dbg = sorted((lg / "debug").glob("*")) if (lg / "debug").exists() else []
        a["install_logs"] = {"general": (lg / "VisionALVR.log").read_text(errors="replace") if (lg / "VisionALVR.log").exists() else "",
                             "debug_files": sorted(p.name for p in dbg[-1].iterdir()) if dbg else []}
    if sc.get("reconnect"):
        a["client2"] = jload(outdir / "client2_report.json")
    if sc.get("app"):
        log = (outdir / "app.log").read_text(errors="replace") if (outdir / "app.log").exists() else ""
        rc = (outdir / "app.rc").read_text().strip() if (outdir / "app.rc").exists() else None
        _, prow = read_csv(outdir / "app_pose.csv")            # frame,yaw,x,y,z,display_time_ns
        _, hrow2 = read_csv(outdir / "host_frames.csv")        # shim_frame,ts_ns,bytes,idr,encode_ms
        _, crow2 = read_csv(outdir / "frames.csv")             # ts_ns,arrival_ns,len,yaw
        host_ts2 = {int(r[0]): r[1] for r in hrow2}
        client_yaw = {c[0]: float(c[3]) for c in crow2 if len(c) > 3 and c[3]}
        devs = []
        for r in prow:
            f = int(r[0])
            if f in host_ts2 and host_ts2[f] in client_yaw:
                devs.append(float(r[1]) - client_yaw[host_ts2[f]])
        med = sorted(devs)[len(devs) // 2] if devs else 0.0
        resid = sorted(abs(d - med) for d in devs)
        yaws = [float(r[1]) for r in prow]
        a["app"] = {"rc": rc, "passed": "PASS (rc=0)" in log, "frames_logged": len(prow), "pose_pairs": len(devs),
                    "yaw_offset": med, "yaw_resid_max": resid[-1] if resid else None,
                    "yaw_resid_p95": resid[int(0.95 * (len(resid) - 1))] if resid else None,
                    "yaw_range": (max(yaws) - min(yaws)) if yaws else 0.0,
                    "fps": next((float(m.group(1)) for m in [re.search(r"app fps ([0-9.]+)", log)] if m), None)}
    if sc.get("checks", {}).get("controllers") and (outdir / "app_input.csv").exists():
        _, irows = read_csv(outdir / "app_input.csv")      # frame,display_ns,trigger_r,squeeze_l,a_r,x_l,stick_lx,stick_ly,rx,ry,rz,lx,ly,lz,vl,vr,haptics
        _, srows = read_csv(outdir / "controllers.csv")    # ts_ns,trigger_r,squeeze_l,a_r,stick_lx,stick_ly,x_l,rx,ry,rz,lx,ly,lz
        _, hrows = read_csv(outdir / "host_frames.csv")
        host_ts3 = {int(r[0]): int(r[1]) for r in hrows}
        sent = [[float(x) for x in r] for r in srows]
        sent_ts = [r[0] for r in sent]
        import bisect
        # (app column, sent column, tolerance, positional)
        fields = {"trigger_r": (2, 1, 0.03), "squeeze_l": (3, 2, 0.03), "a_r": (4, 3, 0.0), "x_l": (5, 6, 0.0),
                  "stick_lx": (6, 4, 0.03), "stick_ly": (7, 5, 0.03),
                  "rx": (8, 7, None), "ry": (9, 8, None), "rz": (10, 9, None), "lx": (11, 10, None), "ly": (12, 11, None), "lz": (13, 12, None)}
        res = {}
        window = 100_000_000
        for name, (ac, sc_, tol) in fields.items():
            diffs, oks, n = [], 0, 0
            for r in irows:
                f = int(r[0])
                if f not in host_ts3:
                    continue
                ts = host_ts3[f]
                lo, hi = bisect.bisect_left(sent_ts, ts - window), bisect.bisect_right(sent_ts, ts + window)
                if lo >= hi:
                    continue
                v = float(r[ac])
                cand = [abs(v - sent[k][sc_]) for k in range(lo, hi)]
                n += 1
                if tol is not None:
                    oks += 1 if min(cand) <= tol else 0
                else:      # position: compare against the sample closest in time (offset removed below)
                    k = min(range(lo, hi), key=lambda k: abs(sent_ts[k] - ts))
                    diffs.append(v - sent[k][sc_])
            if tol is None:
                med = sorted(diffs)[len(diffs) // 2] if diffs else 0.0
                resid = sorted(abs(d - med) for d in diffs)
                res[name] = {"n": n, "offset": med, "resid_p95": resid[int(0.95 * (len(resid) - 1))] if resid else None}
            else:
                res[name] = {"n": n, "frac_ok": oks / n if n else 0.0}
        res["pose_valid_frac"] = (sum(1 for r in irows if r[14] == "1" and r[15] == "1") / len(irows)) if irows else 0.0
        res["app_haptics_sent"] = int(irows[-1][16]) if irows else 0
        a["controllers"] = res
    cl = outdir / "client.hevc"
    if sc.get("checks", {}).get("layers") and cl.exists() and cl.stat().st_size > 0:
        a["layers"] = analyse_layers(sc, outdir)
    if sc.get("checks", {}).get("qpmap"):
        a["qpmap"] = analyse_qpmap(sc, outdir, a.get("host") or {})
    if sc.get("checks", {}).get("pacing") and sc.get("app"):
        a["pacing"] = analyse_pacing(sc, outdir, 1000.0 / ((a.get("host") or {}).get("fps_target") or 90.0))
    period = 1000.0 / ((a.get("host") or {}).get("fps_target") or 90.0)
    if sc.get("client", {}).get("display_sim"):
        a["display"] = analyse_display(sc, outdir, a.get("client"), period)
    if (outdir / "host_stdout.txt").exists():
        a["pipeline"] = analyse_pipeline(outdir)
    if (outdir / "app_timing.csv").exists():
        a["app_timing"] = analyse_app_timing(outdir, period)
    if sc.get("checks", {}).get("frame_classes") and cl.exists() and cl.stat().st_size > 0:
        classes = read_frame_classes(cl)
        runs, stray = rle(classes)
        a["classes"] = {"frames": len(classes), "runs": runs, "stray_frames": stray}
    if sc.get("checks", {}).get("counter_readback") and cl.exists() and cl.stat().st_size > 0:
        if sc["checks"].get("foveation") is not None:
            cfgs = negotiated_cfg(outdir)
            fov_on = bool(cfgs.get("enable_foveated_encoding"))
            pts = stamp_points(cfgs, sc["checks"].get("counter_block_px", 64), fov_on)
            vals, patch = read_stamps_at(cl, pts)
            mode = "software"
            v = fov_vars(cfgs["eye_resolution_width"], cfgs["eye_resolution_height"], cfgs) if fov_on else None
            a["fov"] = {"enabled": fov_on, "expected_w": (v["opt_w"] * 2) if v else cfgs["eye_resolution_width"] * 2,
                        "expected_h": v["opt_h"] if v else cfgs["eye_resolution_height"], "stamp_points": pts[:3] + pts[-2:]}
        else:
            vals, patch, mode = read_counters(cl, sc["checks"].get("counter_block_px", 128))
        _, crow = read_csv(outdir / "frames.csv")           # ts_ns,arrival_ns,len,yaw  (client side, per received frame)
        _, hrow = read_csv(outdir / "host_frames.csv")      # frame,ts_ns,bytes,idr,encode_ms
        # Pair each client-received frame with the host row that sent it, in order (timestamps are not unique), then require that
        # (decoded app counter - host frame number) is the same constant for every frame. The host may skip frame numbers (it encodes
        # the newest frame when two are ready), so the counters cannot be compared by value.
        host_rows = [(int(r[0]), r[1]) for r in hrow]
        pairs, j = [], 0
        for ci, c_ in enumerate(crow):
            while j < len(host_rows) and host_rows[j][1] != c_[0]:
                j += 1
            if j < len(host_rows):
                pairs.append((ci, host_rows[j][0]))
                j += 1
        shift = len(crow) - len(vals)                          # frames the decoder could not output are at the start
        app_vals = [v for v in vals if v not in (0, 65535)]   # green frames read all-ones, an app's first empty-layer frame reads 0: neither is stamped
        offs = [vals[ci - shift] - lab for ci, lab in pairs if lab > 0 and 0 <= ci - shift < len(vals) and vals[ci - shift] not in (0, 65535)]
        mode_off = max(set(offs), key=offs.count) if offs else None
        mapped = len(offs)
        mism = sum(1 for o in offs if o != mode_off)
        inc = all(b > a for a, b in zip(app_vals, app_vals[1:]))
        a["counter"] = {"decoder": mode, "decoded": len(vals), "client_rows": len(crow), "aligned": abs(len(vals) - len(crow)) <= max(2, int(0.02 * len(crow))),   # frames whose reference was lost cannot decode
                        "mapped_to_host_rows": mapped, "mismatches": mism, "strictly_increasing": inc,
                        "first": app_vals[:3], "last": app_vals[-3:], "green_frames": len(vals) - len(app_vals),
                        "gray_patch_median": (lambda ps: sorted(ps)[len(ps) // 2] if ps else None)([p for v, p in zip(vals, patch) if v not in (0, 65535)])}
    if cl.exists() and cl.stat().st_size > 0:
        a["props"] = ffprobe_props(cl)
        if sc.get("checks", {}).get("content_integrity") or sc.get("checks", {}).get("decode"):
            sums, mode, err = framemd5(cl)
            a["decode"] = {"frames": len(sums), "mode": mode, "stderr": err}
            if sc.get("checks", {}).get("content_integrity") and sc.get("source"):
                src = source_md5(sc["source"])
                known = set(src["sums"])
                bad = [i for i, s in enumerate(sums) if s not in known]
                a["integrity"] = {"client_frames": len(sums), "source_frames": len(src["sums"]), "unknown_frames": len(bad),
                                  "decoder": mode, "source_decoder": src["mode"]}
    return a


def evaluate(sc, a):
    """Returns list of checks: {name, ok, value, expected}."""
    out = []

    def chk(name, ok, value, expected):
        out.append({"name": name, "ok": bool(ok), "value": value, "expected": expected})

    ck = sc.get("checks", {})
    h, c = a.get("host"), a.get("client")
    exp = sc.get("expect", {})
    if "client_connected" in exp:
        chk("client_connected", bool(c and c.get("connected")) == exp["client_connected"], bool(c and c.get("connected")), exp["client_connected"])
    if exp.get("client_connected", True) is False:
        return out
    chk("client_report_present", c is not None, c is not None, True)
    if c is None:
        return out
    if sc.get("reconnect"):
        c2 = a.get("client2") or {}
        need = sc["reconnect"].get("min_frames", 100)
        chk("reconnect_second_session_streams", bool(c2.get("connected")) and c2.get("frames", 0) >= need,
            {"connected": c2.get("connected"), "frames": c2.get("frames")}, f"the daemon host serves a second client session: connected and >= {need} frames")
        chk("reconnect_first_session_streamed", c.get("frames", 0) >= need, c.get("frames"), f">= {need} frames before the disconnect")
    if sc.get("host") is not None and not (sc.get("host") or {}).get("daemon"):
        chk("host_report_present", h is not None, h is not None, True)
        if h:
            chk("host_connected", h.get("connected"), h.get("connected"), True)
            if "host_min_frames" in ck:
                chk("host_frames_sent", h["frames_sent"] >= ck["host_min_frames"], h["frames_sent"], f">={ck['host_min_frames']}")
            if "host_min_reconfigurations" in ck:
                chk("host_reconfigured_for_negotiated_stream", h.get("reconfigurations", 0) >= ck["host_min_reconfigurations"], h.get("reconfigurations"), f">={ck['host_min_reconfigurations']} (encoder rebuilt from the negotiated stream parameters)")
            if "host_idr_requests_max" in ck:
                chk("host_idr_requests", h["idr_requests"] <= ck["host_idr_requests_max"], h["idr_requests"], f"<={ck['host_idr_requests_max']}")
    chk("client_streaming", c.get("connected"), c.get("connected"), True)
    if "client_min_frames" in ck:
        chk("client_frames", c["frames"] >= ck["client_min_frames"], c["frames"], f">={ck['client_min_frames']}")
    if ck.get("timestamps_monotonic", True):
        chk("timestamps_monotonic", c.get("timestamps_monotonic"), c.get("timestamps_monotonic"), True)
    chk("duplicate_timestamps", c.get("duplicate_timestamps", 0) <= ck.get("max_duplicate_timestamps", 0), c.get("duplicate_timestamps"), f"<={ck.get('max_duplicate_timestamps', 0)}")
    if "min_delivery_ratio" in ck and h and h.get("frames_sent"):
        r = c["frames"] / h["frames_sent"]
        chk("delivery_ratio", r >= ck["min_delivery_ratio"], round(r, 3), f">={ck['min_delivery_ratio']} (client frames / host frames sent; UDP loss)")
    if "min_pose_match_ratio" in ck and c["frames"]:
        r = c["frames_with_matching_sent_pose"] / c["frames"]
        chk("pose_match_ratio", r >= ck["min_pose_match_ratio"], round(r, 4), f">={ck['min_pose_match_ratio']}")
    if "max_latency_p95_ms" in ck:
        v = c["arrival_minus_timestamp_ms"]["p95"]
        chk("arrival_minus_timestamp_p95_ms", v <= ck["max_latency_p95_ms"], round(v, 2), f"<={ck['max_latency_p95_ms']}")
    if "fps" in ck:
        lo, hi = ck["fps"]
        chk("fps_measured", lo <= c["fps_measured"] <= hi, round(c["fps_measured"], 2), f"[{lo},{hi}]")
    if "mbps" in ck:
        lo, hi = ck["mbps"]
        chk("mbps_measured", lo <= c["mbps_measured"] <= hi, round(c["mbps_measured"], 1), f"[{lo},{hi}]")
    if "negotiated" in ck:
        for k, v in ck["negotiated"].items():
            got = (c.get("negotiated_config") or {}).get(k)
            chk(f"negotiated.{k}", got == v, got, v)
    if "codec" in ck:
        chk("codec", c.get("codec") == ck["codec"], c.get("codec"), ck["codec"])
    p = a.get("props")
    if "stream" in ck:
        chk("stream_probe_ok", p is not None, p is not None, True)
        for k, v in ck["stream"].items():
            chk(f"stream.{k}", p is not None and str(p.get(k)) == str(v), p.get(k) if p else None, v)
    if ck.get("decode"):
        d = a.get("decode", {})
        chk("decode_no_errors", d.get("frames", 0) > 0 and not d.get("stderr"), f"{d.get('frames')} frames via {d.get('mode')} {d.get('stderr', '')[:80]}", ">0 frames, no errors")
        if c and d.get("frames"):
            chk("decoded_frames_match_received", abs(d["frames"] - c["frames"]) <= ck.get("decode_frame_slack", 0), d["frames"], f"{c['frames']}±{ck.get('decode_frame_slack', 0)}")
    if "controllers" in a:
        c3 = a["controllers"]
        tolf = ck["controllers"]
        for name in ("trigger_r", "squeeze_l", "a_r", "x_l", "stick_lx", "stick_ly"):
            chk(f"input.{name}", c3[name]["n"] > 50 and c3[name]["frac_ok"] >= tolf["min_frac_ok"], {"frames": c3[name]["n"], "frac_ok": round(c3[name]["frac_ok"], 3)}, f"app value equals a value the client sent (+-100 ms) in >= {tolf['min_frac_ok']} of frames")
        for name in ("rx", "ry", "rz", "lx", "ly", "lz"):
            chk(f"controller_pose.{name}", c3[name]["n"] > 50 and c3[name]["resid_p95"] is not None and c3[name]["resid_p95"] <= tolf["max_pose_resid_m"],
                {"offset_m": round(c3[name]["offset"], 4), "resid_p95_m": c3[name]["resid_p95"]}, f"p95 residual (constant grip offset removed) <= {tolf['max_pose_resid_m']} m")
        chk("controller_pose.valid", c3["pose_valid_frac"] >= 0.95, round(c3["pose_valid_frac"], 3), ">=0.95 of frames have both hand poses valid")
        hap = (c or {}).get("haptics", {})
        chk("haptics_received_by_client", hap.get("count", 0) >= tolf["min_haptic_events"] and len(hap.get("devices", [])) >= 2 and hap.get("max_amplitude", 0) >= tolf["min_haptic_amplitude"],
            {"events": hap.get("count"), "devices": hap.get("devices"), "max_amp": hap.get("max_amplitude"), "app_requested": c3["app_haptics_sent"]},
            f">= {tolf['min_haptic_events']} events on both hands, max amplitude >= {tolf['min_haptic_amplitude']}")
    if ck.get("foveation") is not None and "fov" in a:
        f = a["fov"]
        pr = a.get("props") or {}
        chk("encoded_size_matches_alvr_layout", f["enabled"] == ck["foveation"] and str(pr.get("width")) == str(f["expected_w"]) and str(pr.get("height")) == str(f["expected_h"]),
            {"foveation_enabled": f["enabled"], "decoded": f"{pr.get('width')}x{pr.get('height')}", "expected": f"{f['expected_w']}x{f['expected_h']}"},
            "stream size == 2 x optimized eye width x optimized eye height computed from the session (independent port of ALVR's FFR math)")
    if "layers" in a:
        L_ = a["layers"]
        if "error" in L_:
            chk("layers_analysis", False, L_["error"], "layer oracle ran")
        else:
            need = ck["layers"].get("min_frames", 20)
            for name, r in sorted(L_["points"].items(), key=lambda kv: (kv[1]["phase"], kv[0])):
                frac = r["ok"] / r["n"] if r["n"] else 0.0
                chk(f"layer.p{r['phase']}.{name}", r["n"] >= need and frac >= ck["layers"].get("min_ok_frac", 0.9),
                    {"frames": r["n"], "ok_frac": round(frac, 2), "samples": r["samples"][:2]}, f"expected {r['expected']} at the projected point in >= {ck['layers'].get('min_ok_frac', 0.9)} of {need}+ frames")
    if "qpmap" in ck:
        qk, qa = ck["qpmap"], a.get("qpmap") or {}
        chk("qpmap_enabled_in_nvenc", qa.get("nvenc_qp_map_mode") == (2 if qk.get("on", True) else 0), qa.get("nvenc_qp_map_mode"), "NVENC reports qpMapMode 2 (delta) when the map is on, 0 when off")
        if qk.get("on", True):
            chk("qpmap_matches_oracle", qa.get("dump") and qa.get("dims_ok") and qa.get("mismatch", 99) <= max(1, int(0.005 * qa.get("ctbs", 0))),
                {"mismatch": qa.get("mismatch"), "ctbs": qa.get("ctbs"), "hist": qa.get("hist")}, "the C++ map equals the independent Python port (<= 0.5% CTBs differing by rounding)")
        if "min_detail_ratio" in qk:
            chk("qpmap_center_keeps_more_detail", (qa.get("detail_ratio") or 0) >= qk["min_detail_ratio"], qa.get("detail") or qa.get("detail_error"), f"decoded detail center/edge >= {qk['min_detail_ratio']} (ratio {qa.get('detail_ratio')})")
        if "max_detail_ratio" in qk:
            chk("qpmap_off_is_flat", (qa.get("detail_ratio") or 99) <= qk["max_detail_ratio"], qa.get("detail"), f"decoded detail center/edge <= {qk['max_detail_ratio']} with the map off (ratio {qa.get('detail_ratio')})")
    if "display" in ck:
        dc, dk = ck["display"], a.get("display")
        chk("display_measured", dk is not None and dk.get("ticks", 0) > 200, dk and dk.get("ticks"), "> 200 emulated display ticks")
        if dk:
            if "max_repeat_pct" in dc:
                chk("display_repeat_pct", dk["repeat_pct"] <= dc["max_repeat_pct"], round(dk["repeat_pct"], 2), f"<= {dc['max_repeat_pct']} % of ticks re-presented the last frame (SteamVR+ALVR on the headset: 3.4)")
            if "max_skip_pct" in dc:
                chk("display_skip_pct", dk["skip_pct"] <= dc["max_skip_pct"], round(dk["skip_pct"], 2), f"<= {dc['max_skip_pct']} % of shown frames jumped a frame")
            if "max_dwell_spread_ms" in dc:
                chk("display_dwell_spread_ms", dk["dwell_spread_ms"] <= dc["max_dwell_spread_ms"], dk["dwell_spread_ms"], f"<= {dc['max_dwell_spread_ms']} (dwell p95 - p05: a flat queue means regular arrival)")
            if "min_fps" in dc:
                chk("display_fps", dk["display_fps"] >= dc["min_fps"], round(dk["display_fps"], 1), f">= {dc['min_fps']}")
            if "max_repeat_run" in dc and dk.get("repeat_runs"):
                chk("display_max_repeat_run", dk["repeat_runs"]["max"] <= dc["max_repeat_run"], dk["repeat_runs"]["max"], f"<= {dc['max_repeat_run']} consecutive repeats (no stalls)")
    if "pipeline" in ck:
        pc2, pp = ck["pipeline"], a.get("pipeline")
        chk("pipeline_measured", pp is not None, pp is not None, "staged pipeline counters in encoder_stats")
        if pp:
            if "max_superseded_frac" in pc2:
                chk("pipeline_superseded_frac", pp["superseded_frac"] <= pc2["max_superseded_frac"], pp["superseded_frac"], f"<= {pc2['max_superseded_frac']} of arrived frames replaced before a boundary (app faster than the display)")
            if "max_encoder_overruns" in pc2:
                chk("pipeline_encoder_overruns", (pp.get("encoder_overruns") or 0) <= pc2["max_encoder_overruns"], pp.get("encoder_overruns"), f"<= {pc2['max_encoder_overruns']} (encode longer than a period)")
            if "max_empty_frac" in pc2:
                chk("pipeline_empty_frac", pp["empty_frac"] <= pc2["max_empty_frac"], pp["empty_frac"], f"<= {pc2['max_empty_frac']} boundaries without a new frame")
            if "max_send_late" in pc2:
                chk("pipeline_send_late", (pp.get("send_late") or 0) <= pc2["max_send_late"], pp.get("send_late"), f"<= {pc2['max_send_late']} sends after boundary + lead")
            if pc2.get("gpu_sched_class") is not None:
                g = pp.get("gpu_scheduling") or {}
                chk("pipeline_gpu_sched_class", g.get("class") == pc2["gpu_sched_class"] and g.get("status") == 0, g, f"class {pc2['gpu_sched_class']} granted")
    if "app_timing" in ck:
        at, ak = ck["app_timing"], a.get("app_timing")
        chk("app_timing_measured", ak is not None, ak is not None, "xr_probe per-frame timing")
        if ak and "min_fps" in at:
            chk("app_timing_fps", ak["fps"] >= at["min_fps"], ak["fps"], f">= {at['min_fps']} (the shim's running start must not starve the app)")
        if ak and "max_release_p95_ms" in at:
            chk("app_timing_release_p95_ms", ak["release_interval_ms"]["p95"] <= at["max_release_p95_ms"], ak["release_interval_ms"]["p95"], f"<= {at['max_release_p95_ms']} (one release per display period)")
    if "pacing" in ck:
        pc, pk = ck["pacing"], a.get("pacing")
        chk("pacing_measured", pk is not None, pk, "client arrival spacing of the app's frames (after warm-up)")
        if pk:
            if "client_p99_ms" in pc:
                chk("pacing_client_p99_ms", pk["p99"] <= pc["client_p99_ms"], pk["p99"], f"<= {pc['client_p99_ms']} (p50 {pk['p50']}, p95 {pk['p95']}, max {pk['max']})")
            if "max_late_frac" in pc:
                chk("pacing_late_frac", pk["late_frac"] <= pc["max_late_frac"], pk["late_frac"], f"<= {pc['max_late_frac']} of frames later than 1.5 periods ({pk['late']}/{pk['n']})")
        pj = (h or {}).get("pacing") or {}
        vs, it = pj.get("vsync_interval_ms"), pj.get("frame_intake_ms")
        if "vsync_p99_ms" in pc:
            chk("pacing_vsync_p99_ms", bool(vs) and vs["p99"] <= pc["vsync_p99_ms"], vs and vs["p99"], f"<= {pc['vsync_p99_ms']} (host display clock tick-to-tick; max {vs and vs['max']})")
        if "vsync_max_ms" in pc:
            chk("pacing_vsync_max_ms", bool(vs) and vs["max"] <= pc["vsync_max_ms"], vs and vs["max"], f"<= {pc['vsync_max_ms']}")
        # never faster than the display: no short ticks (a double-tick bug once hid behind p50/p99), app fps <= refresh
        if vs:
            per = 1000.0 / ck.get("refresh_hz", 90)
            chk("pacing_vsync_no_short_ticks", vs.get("min", 0) >= 0.5 * per and vs.get("under_0p5_period", 1) == 0,
                {"min": vs.get("min"), "p01": vs.get("p01"), "under_0p5_period": vs.get("under_0p5_period")}, f"every tick >= {0.5 * per:.1f} ms apart (shorter = a double tick; a late tick is followed by a shorter one that keeps ALVR's grid)")
        if a.get("app", {}).get("fps") is not None:
            v = a["app"]["fps"]
            chk("pacing_app_not_faster_than_display", v <= ck.get("refresh_hz", 90) * 1.03, v, f"<= {ck.get('refresh_hz', 90) * 1.03:.1f} fps")
        if "intake_p99_ms" in pc:
            chk("pacing_intake_p99_ms", bool(it) and it["p99"] <= pc["intake_p99_ms"], it and it["p99"], f"<= {pc['intake_p99_ms']} (host frame intake spacing; p50 {it and it['p50']})")
    if "frame_classes" in ck:
        fc = ck["frame_classes"]
        k = a.get("classes") or {}
        runs = k.get("runs", [])
        seq = [r[0] for r in runs]
        lens_ok = len(runs) == len(fc["expect"]) and all(r[1] >= m for r, m in zip(runs, fc["min_len"]))
        chk("frame_sequence", seq == fc["expect"] and lens_ok and k.get("stray_frames", 99) <= fc.get("max_stray", 5),
            {"runs": runs, "stray": k.get("stray_frames")}, f"{'->'.join(fc['expect'])} with run lengths >= {fc['min_len']} (G = pure green, A = app) and <= {fc.get('max_stray', 5)} stray frames")
    if "min_slices_per_frame" in ck and h and h.get("encoder"):
        v = h["encoder"].get("slices_per_frame") or 0
        chk("split_encode_slices_per_frame", v >= ck["min_slices_per_frame"],
            {"slices_per_frame": round(v, 2), "nvenc_engines": h["encoder"].get("nvenc_engines")}, f">= {ck['min_slices_per_frame']} (NVENC split-frame encoding needs a GPU with >= 2 NVENC engines)")
    if "max_pipeline_latency_ms" in ck:
        v = c.get("total_pipeline_latency_ms", 0.0)
        chk("pipeline_latency_ms", 0.0 < v <= ck["max_pipeline_latency_ms"], round(v, 1), f"(0, {ck['max_pipeline_latency_ms']}] (client's ALVR statistic: tracking sent -> frame displayed; mock decode/compositor are emulated)")
    if "refresh_hz" in ck and h:
        chk("host_refresh_hz", abs(h.get("refresh_hz", 0) - ck["refresh_hz"]) < 0.5, h.get("refresh_hz"), ck["refresh_hz"])
    if "app" in a:
        ap = a["app"]
        chk("app_exit_ok", ap["rc"] == "0" and ap["passed"], {"rc": ap["rc"], "log_pass": ap["passed"]}, "rc 0 and PASS in app.log")
        if "app_min_frames" in ck:
            chk("app_frames", ap["frames_logged"] >= ck["app_min_frames"], ap["frames_logged"], f">={ck['app_min_frames']}")
        if "pose_roundtrip_max_rad" in ck:
            chk("pose_roundtrip", ap["pose_pairs"] > 10 and ap["yaw_resid_p95"] is not None and ap["yaw_resid_p95"] <= ck["pose_roundtrip_max_rad"],
                {"pairs": ap["pose_pairs"], "p95_rad": ap["yaw_resid_p95"], "max_rad": ap["yaw_resid_max"], "yaw_range_rad": round(ap["yaw_range"], 3)},
                f"p95 |app yaw - yaw the client sent for that frame's timestamp| (offset removed) <= {ck['pose_roundtrip_max_rad']}")
        if "min_yaw_range_rad" in ck:
            chk("pose_varies", ap["yaw_range"] >= ck["min_yaw_range_rad"], round(ap["yaw_range"], 3), f">={ck['min_yaw_range_rad']}")
    if ck.get("install_logs"):
        il = a.get("install_logs") or {}
        want = ["[host] INFO  VisionALVR", "headset connected", "stream: ", "game started: xr_probe.exe", "[shim:xr_probe.exe] INFO  game start", "game ended: xr_probe.exe", "host stop"]
        gen = il.get("general", "")
        chk("general_log_key_lines", all(w in gen for w in want), [w for w in want if w not in gen] or "all present", "general log has: " + ", ".join(want))
        files, need = il.get("debug_files", []), ["host_events.jsonl", "alvr.log", "gpu.csv"]
        chk("debug_session_files", bool(files) and all(n in files for n in need) and any(n.startswith("shim_xr_probe_") for n in files), files,
            "logs\\debug\\<time>\\ has " + ", ".join(need) + ", shim_xr_probe_<pid>.log")
    if ck.get("bench_tracks_target"):
        st = a.get("bench_steps") or []
        ok = bool(st) and all(abs(x["actual_mbps"] - x["target_mbps"]) <= 0.2 * x["target_mbps"] and x.get("client_fps", 0) >= 80 for x in st)
        chk("bench_steps_hit_target", ok, [(x["target_mbps"], round(x["actual_mbps"]), x.get("client_fps")) for x in st], "each step within 20% of its target, headset at >= 80 fps (keep-alive on)")
    if ck.get("host_events"):
        names = set(a.get("host_event_names", []))
        missing = [e for e in ck["host_events"] if e not in names]
        chk("host_events_present", not missing, missing or "all present", ck["host_events"])
    if "app" in a and "app_min_fps" in ck:
        v = a["app"].get("fps")
        chk("app_fps", v is not None and v >= ck["app_min_fps"], v, f">= {ck['app_min_fps']} (frames the app completed per second, xr_probe)")
    if "audio_min_device_events" in ck and h:
        v = (h.get("audio") or {}).get("device_events", 0)
        chk("game_audio_capture_started", v >= ck["audio_min_device_events"], {"device_events": v, "config": (h.get("audio") or {}).get("config")},
            f">= {ck['audio_min_device_events']} (server_core's game-audio thread opened its loopback device)")
    if "shim_ts_matched_min_frac" in ck and h and h.get("shim"):
        m, f = h["shim"].get("ts_matched", 0), h["shim"].get("ts_fallback", 0)
        v = m / max(1, m + f)
        chk("shim_frame_ts_from_pose_match", v >= ck["shim_ts_matched_min_frac"], {"matched": m, "fallback": f}, f">= {ck['shim_ts_matched_min_frac']} of frames")
    if "shim_submit_mode" in ck and h and h.get("shim"):
        chk("shim_submit_mode", h["shim"].get("submit_mode") == ck["shim_submit_mode"], h["shim"].get("submit_mode"), ck["shim_submit_mode"])
    if "gray_patch" in ck:
        k = a.get("counter") or {}
        v = k.get("gray_patch_median")
        lo, hi = ck["gray_patch"]
        chk("gray_patch", v is not None and lo <= v <= hi, v, f"[{lo},{hi}] (linear 0.5 -> sRGB 188 -> decoded gray)")
    if ck.get("counter_readback"):
        k = a.get("counter") or {}
        chk("counter_decoded_all", bool(k) and k.get("aligned"), f"{k.get('decoded')} decoded vs {k.get('client_rows')} received", "equal, >0")
        chk("counter_matches_host_frame_for_timestamp", bool(k) and k.get("mapped_to_host_rows", 0) > 0 and k.get("mismatches", 1) == 0,
            f"{k.get('mismatches')} mismatches / {k.get('mapped_to_host_rows')} mapped", "0 mismatches: decoded frame number == host frame sent with that timestamp")
        chk("counter_strictly_increasing", bool(k) and k.get("strictly_increasing"), {"first": k.get("first"), "last": k.get("last")}, True)
    if "max_encode_ms_p95" in ck and h and h.get("encoder"):
        v = h["encoder"]["encode_ms"]["p95"]
        chk("encode_ms_p95", v <= ck["max_encode_ms_p95"], round(v, 2), f"<={ck['max_encode_ms_p95']}")
    if ck.get("content_integrity"):
        i = a.get("integrity")
        tol = ck.get("max_unknown_frame_ratio", 0.0)
        ratio = (i["unknown_frames"] / i["client_frames"]) if i and i["client_frames"] else 1.0
        chk("content_integrity", i is not None and i["client_frames"] > 0 and ratio <= tol, {**(i or {}), "unknown_ratio": round(ratio, 4)},
            f"unknown (not bit-identical to a source frame) ratio <= {tol}")
    return out
