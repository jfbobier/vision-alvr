#!/usr/bin/env python3
"""Automated harness entry point.

  python3 harness/run.py setup [--clone-only <dir>]        # one-time builder setup (pinned ALVR + VDXR), then build everything
  python3 harness/run.py list
  python3 harness/run.py build [host|client|shim|probe|package|vdxr|nvenc ...]; test-install     # (re)build binaries on the builder
  python3 harness/run.py run  <scenario> [<scenario> ...]
  python3 harness/run.py all                               # every scenario, non-zero exit if any fail
  python3 harness/run.py report [results_dir]              # re-evaluate saved artifacts (no builder needed)
  python3 harness/run.py bench-quality [quick|full]        # benchmark phase 2 headless on the builder + sanity checks
  python3 harness/run.py test-zip                          # unzip the package into a fresh folder and run from it (logs, debug, config)

Exit code: 0 all PASS, 1 any FAIL, 2 any BLOCKED (infrastructure), 3 usage.
Results: harness/results/<UTC>_<scenario>/{result.json, analysis.json, *_report.json, client.hevc, ...}
"""
import json, subprocess, sys, time
from datetime import datetime, timezone
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import lib

SCEN = lib.ROOT / "harness" / "scenarios"


def load(name):
    p = SCEN / f"{name}.json"
    if not p.exists():
        p = SCEN / "optional" / f"{name}.json"      # cross-network scenarios, not part of `all`
    if not p.exists():
        raise SystemExit(f"unknown scenario {name}; available: {', '.join(x.stem for x in sorted(SCEN.glob('*.json')))}")
    sc = json.load(open(p))
    sc.setdefault("name", name)
    return sc


def print_checks(checks):
    for c in checks:
        print(f"    [{'PASS' if c['ok'] else 'FAIL'}] {c['name']}: {c['value']}  (expected {c['expected']})")


def verdict(checks):
    return "PASS" if checks and all(c["ok"] for c in checks) else "FAIL"


def run_one(name):
    sc = load(name)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = lib.RESULTS / f"{stamp}_{name}"
    print(f"== {name}: {sc.get('description', '')}")
    result = {"scenario": name, "started": stamp, "outdir": str(outdir)}
    try:
        steps = lib.run_scenario(sc, outdir, log=print)
        a = lib.analyse(sc, outdir)
        checks = lib.evaluate(sc, a)
        result.update(status=verdict(checks), steps=steps, checks=checks)
        (outdir / "analysis.json").write_text(json.dumps(a, indent=1, default=str))
    except lib.Blocked as e:
        result.update(status="BLOCKED", reason=str(e), checks=[])
        outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "result.json").write_text(json.dumps(result, indent=1))
    latest = lib.RESULTS / "latest"
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(outdir.name)
    except OSError:
        pass
    print_checks(result["checks"])
    print(f"   -> {result['status']}" + (f" ({result['reason']})" if result.get("reason") else "") + f"   [{outdir}]")
    return result["status"]


def build(what):
    stages = {"host": ("tools/host/Cargo.toml", "tools/host/src/main.rs", "_tmp/host", "tools/remote/stage8_host.ps1"),
              "client": ("tools/mock_client/Cargo.toml", "tools/mock_client/src/main.rs", "_tmp/mock_client", "tools/remote/stage7_mockclient.ps1")}
    if any(w in ("client", "host") for w in (what or ["client", "host"])):
        lib.b_run("if not exist _tmp mkdir _tmp")
        lib.b_put(lib.ROOT / "tools/patches/loopback-client.patch", "_tmp/loopback-client.patch")
        r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage9_patch.ps1"], cwd=lib.ROOT, capture_output=True)
        o = r.stdout.decode(errors="replace").replace("\x00", "")
        print("patch:", "ok" if "applied" in o else "FAILED " + o[-300:])
        if "applied" not in o:
            return 2
    for w in what or ["client", "host"]:
        if w == "vdxr":      # VDXR with the small OVRShim patches (tools/remote/stage15_vdxr.ps1)
            r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage15_vdxr.ps1"], cwd=lib.ROOT, capture_output=True)
            o = r.stdout.decode(errors="replace").replace("\x00", "")
            print(f"build vdxr: {'ok' if 'RESULT: OK' in o else 'FAILED'}")
            if "RESULT: OK" not in o:
                print(o[-1200:])
                return 2
            continue
        if w == "shim":      # OVRShim: fork of VDXR's OVRNull (ovrshim/), built next to it in the builder's VDXR checkout
            lib.b_run("if not exist _tmp\\ovrshim mkdir _tmp\\ovrshim")
            for f in ("driver.cpp", "ipc.h", "ipc_win.h", "constantsbuffer.h", "layerconstants.h", "ReprojectPS.hlsl", "LayerVS.hlsl", "LayerPS.hlsl", "LayerCubePS.hlsl"):
                lib.b_put(lib.ROOT / "ovrshim" / f, f"_tmp/ovrshim/{f}")
            r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage11_ovrshim.ps1"], cwd=lib.ROOT, capture_output=True)
            o = r.stdout.decode(errors="replace").replace("\x00", "")
            print(f"build shim: {'ok' if 'RESULT: OK' in o else 'FAILED'}")
            if "RESULT: OK" not in o:
                print(o[-1500:])
                return 2
            continue
        if w == "gui":       # VisionALVR.exe + configure.exe (C# WinForms, .NET Framework 4.8)
            lib.b_run("if not exist _tmp\\gui\\res mkdir _tmp\\gui\\res")
            for f in ("Common.cs", "VisionALVR.cs", "Configure.cs", "app.manifest"):
                lib.b_put(lib.ROOT / "tools/gui" / f, f"_tmp/gui/{f}")
            for f in ("visionalvr.ico", "logo_640.png"):
                lib.b_put(lib.ROOT / "tools/gui/res" / f, f"_tmp/gui/res/{f}")
            r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage16_gui.ps1"], cwd=lib.ROOT, capture_output=True)
            o = r.stdout.decode(errors="replace").replace("\x00", "")
            print(f"build gui: {'ok' if 'RESULT: OK' in o else 'FAILED'}")
            if "RESULT: OK" not in o:
                print(o[-3000:])
                return 2
            continue
        if w == "package":   # out\\VisionALVR: the portable folder a user unzips (+ out\\VisionALVR-<version>.zip)
            lib.b_run("if not exist _tmp\\install mkdir _tmp\\install")
            for f in ("register_openxr_runtime.ps1", "register_openxr_runtime.bat", "unregister_openxr_runtime.bat", "session.default.json", "README.txt"):
                lib.b_put(lib.ROOT / "tools/install" / f, f"_tmp/install/{f}")
            # licences shipped with the binaries: the notices, the project licence (once chosen) and the upstream texts
            lib.b_run("if not exist _tmp\\install\\licenses mkdir _tmp\\install\\licenses")
            lib.b_put(lib.ROOT / "THIRD_PARTY_NOTICES.md", "_tmp/install/THIRD_PARTY_NOTICES.md")
            if (lib.ROOT / "LICENSE").exists():
                lib.b_put(lib.ROOT / "LICENSE", "_tmp/install/LICENSE.txt")
            for f in sorted((lib.ROOT / "third_party/licenses").glob("*.txt")):
                lib.b_put(f, f"_tmp/install/licenses/{f.name}")
            r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage13_package.ps1"], cwd=lib.ROOT, capture_output=True)
            o = r.stdout.decode(errors="replace").replace("\x00", "")
            print(f"build package: {'ok' if 'RESULT: OK' in o else 'FAILED'}")
            if "RESULT: OK" not in o:
                print(o[-1200:])
                return 2
            continue
        if w == "probe":     # the OpenXR test app
            lib.b_put(lib.ROOT / "tools/probe/xr_probe.cpp", "_tmp/xr_probe.cpp")
            r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage12_probe_build.ps1"], cwd=lib.ROOT, capture_output=True)
            o = r.stdout.decode(errors="replace").replace("\x00", "")
            ok = "RESULT: OK" in o
            print(f"build probe: {'ok' if ok else 'FAILED'}")
            if not ok:
                print(o[-1200:])
                return 2
            continue
        if w == "nvenc":
            subprocess.run(["bash", str(lib.BUILDER), "put", "nvenc/.", "nvenc/"], cwd=lib.ROOT)
            r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage6_nvenc.ps1"], cwd=lib.ROOT)
            continue
        cargo, src, dst, ps = stages[w]
        lib.b_run(f"if not exist {dst.replace('/', chr(92))}\\src mkdir {dst.replace('/', chr(92))}\\src")
        lib.b_put(lib.ROOT / cargo, f"{dst}/Cargo.toml")
        lib.b_put(lib.ROOT / src, f"{dst}/src/main.rs")
        if w == "host":
            lib.b_put(lib.ROOT / "tools/host/build.rs", f"{dst}/build.rs")
            for rs in sorted((lib.ROOT / "tools/host/src").glob("*.rs")):   # every module, not just main.rs
                if rs.name != "main.rs":
                    lib.b_put(rs, f"{dst}/src/{rs.name}")
            subprocess.run(["bash", str(lib.BUILDER), "put", "nvenc/.", "nvenc/"], cwd=lib.ROOT, capture_output=True)
            lib.b_run("if not exist ovrshim mkdir ovrshim")
            subprocess.run(["bash", str(lib.BUILDER), "put", "ovrshim/.", "ovrshim/"], cwd=lib.ROOT, capture_output=True)
            # stb_image (bench.cpp decodes the benchmark scene's textures) and the scene itself (tools/bench_scene/build.sh)
            lib.b_run("if not exist third_party\\stb mkdir third_party\\stb")
            lib.b_put(lib.ROOT / "third_party/stb/stb_image.h", "third_party/stb/stb_image.h")
            vab = lib.ROOT / "build/bench/littlest_tokyo.vab"
            if vab.exists():
                lib.b_run("if not exist bench mkdir bench")
                lib.b_put(vab, "bench/littlest_tokyo.vab")
        r = subprocess.run(["bash", str(lib.BUILDER), "ps", ps], cwd=lib.ROOT, capture_output=True)
        out = r.stdout.decode(errors="replace").replace("\x00", "")
        print(f"build {w}: {'ok' if 'Finished' in out else 'FAILED'}")
        if "Finished" not in out:
            print(out[-1500:])
            return 2
    return 0


def bench_quality(plan):
    """Runs the quality sweep (tools/remote/stage17_bench_quality.ps1) and checks that it measured something real: the
    reconstructed frame was used and matches the input's colour conversion, quality orders sensibly, proposals exist."""
    rid = f"bq_{plan}"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = lib.RESULTS / f"{stamp}_bench_quality_{plan}"
    outdir.mkdir(parents=True, exist_ok=True)
    lib.b_run(f"if not exist runs\\{rid} mkdir runs\\{rid}")
    lib.b_put(lib.ROOT / "harness/fixtures/session.user.json", f"runs/{rid}/session.json")
    frames = ["-Frames", "30", "-Warmup", "10", "-Every", "10"] if plan == "quick" else []
    r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage17_bench_quality.ps1", "-Label", rid, "-Plan", plan, *frames],
                       cwd=lib.ROOT, capture_output=True, timeout=900)
    o = r.stdout.decode(errors="replace").replace("\x00", "")
    for f in ("stdout.jsonl", "stderr.txt", "bq.json", "preview.png"):
        lib.b_get(f"runs/{rid}/{f}", outdir / f)
    events = []
    for line in (outdir / "stdout.jsonl").read_text(errors="replace").splitlines() if (outdir / "stdout.jsonl").exists() else []:
        try:
            events.append(json.loads(line))
        except ValueError:
            pass
    res = [e["data"] for e in events if e.get("event") == "bq_result"]
    summ = next((e["data"] for e in events if e.get("event") == "bq_summary"), None)
    ok = [x for x in res if x["ok"]]
    by = {x["label"]: x for x in ok}
    def psnr(label, k="weighted"):
        return by.get(label, {}).get("psnr", {}).get(k)
    p1, p5 = by.get("100% strong foveation P1 AQ on split auto"), by.get("100% strong foveation P5 AQ on split auto")
    checks = [
        ("ran", "exit 0" in o and summ is not None, o.strip().splitlines()[-2:] if o.strip() else "no output"),
        ("all_configs_ok_or_size_limit", all(x["ok"] or "INVALID_PARAM" in x["error"] for x in res), [x["label"] for x in res if not x["ok"]]),
        ("recon_used", bool(ok) and all(x["recon"] == 1 for x in ok), sorted({x["recon_format"] for x in ok})),
        ("recon_matches_input", bool(ok) and all(abs(x["y_fit"][0] - 1) < 0.05 and abs(x["y_fit"][1]) < 0.05 for x in ok), sorted({round(x["y_fit"][0], 3) for x in ok})),
        ("codec_psnr_plausible", bool(ok) and all(30 < x["psnr"]["codec"] < 70 for x in ok), [round(x["psnr"]["codec"], 1) for x in ok]),
        ("slower_preset_not_worse", p1 is not None and p5 is not None and p5["psnr"]["codec"] >= p1["psnr"]["codec"] - 0.2,
         p1 and p5 and (round(p1["psnr"]["codec"], 2), round(p5["psnr"]["codec"], 2))),
        ("encode_time_by_preset", p1 is not None and p5 is not None and p5["encode_ms"]["p50"] > p1["encode_ms"]["p50"],
         p1 and p5 and (round(p1["encode_ms"]["p50"], 1), round(p5["encode_ms"]["p50"], 1))),
    ]
    if plan != "quick":
        checks += [
            ("foveation_costs_periphery", (psnr("100% no foveation P1 AQ on split auto", "periphery") or 0) > (psnr("100% mild foveation P1 AQ on split auto", "periphery") or 99) > (psnr("100% strong foveation P1 AQ on split auto", "periphery") or 99),
             [psnr(f"100% {f} foveation P1 AQ on split auto", "periphery") for f in ("no", "mild", "strong")]),
            ("lower_resolution_costs_center", (psnr("100% mild foveation P1 AQ on split auto", "center") or 0) > (psnr("80% mild foveation P1 AQ on split auto", "center") or 99),
             [psnr(f"{s} mild foveation P1 AQ on split auto", "center") for s in ("100%", "80%")]),
            ("proposals", summ is not None and all(summ["proposals"].get(k) for k in ("lowest_latency", "recommended", "best_quality")),
             summ and {k: (summ["proposals"][k] or {}).get("label") for k in ("lowest_latency", "recommended", "best_quality")}),
            ("within_time", summ is not None and summ["seconds"] < 180, summ and round(summ["seconds"])),
        ]
    for name, passed, detail in checks:
        print(f"  {'PASS' if passed else 'FAIL'}  {name}: {detail}")
    verdict = "PASS" if all(c[1] for c in checks) else "FAIL"
    json.dump({"plan": plan, "verdict": verdict, "checks": [{"name": n, "pass": bool(p), "detail": str(d)} for n, p, d in checks]},
              open(outdir / "result.json", "w"), indent=1)
    print(f"bench-quality {plan}: {verdict} ({outdir})")
    return 0 if verdict == "PASS" else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 3
    cmd = argv[1]
    if cmd == "list":
        for p in sorted(SCEN.glob("*.json")):
            print(f"{p.stem:28s} {json.load(open(p)).get('description', '')}")
        return 0
    if cmd == "build":
        return build(argv[2:])
    if cmd == "test-install":      # installer logic against a throwaway HKCU key (never touches HKLM or the real runtime)
        lib.b_put(lib.ROOT / "tools/install/Test-Install.ps1", "_tmp/install/Test-Install.ps1")
        rc, out = lib.b_run('powershell -NoProfile -ExecutionPolicy Bypass -File _tmp\\install\\Test-Install.ps1', timeout=300)
        for line in out.splitlines():
            if line.startswith(("CHECK", "RESULT")) or "rror" in line:
                print(line)
        return 0 if "RESULT: PASS" in out else 1
    if cmd == "setup":             # fresh builder: tools check, pinned sources (deps.lock.json), prerequisites, then every build
        lib.b_run("if not exist _tmp mkdir _tmp")
        lib.b_put(lib.ROOT / "deps.lock.json", "_tmp/deps.lock.json")
        extra = ["-CloneOnly", "-Src", argv[3]] if len(argv) > 3 and argv[2] == "--clone-only" else []
        r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/setup_builder.ps1", *extra], cwd=lib.ROOT, capture_output=True, timeout=7200)
        out = r.stdout.decode(errors="replace").replace("\x00", "")
        for line in out.splitlines():
            if line.startswith(("SETUP", "RESULT")):
                print(line.rstrip())
        if "RESULT: OK" not in out:
            return 2
        return 0 if extra else build(["nvenc", "vdxr", "client", "host", "shim", "probe", "gui", "package"])
    if cmd == "test-zip":          # the shipped zip, unzipped into a fresh folder and run from there (no registry)
        r = subprocess.run(["bash", str(lib.BUILDER), "ps", "tools/remote/stage18_zip_test.ps1"], cwd=lib.ROOT, capture_output=True, timeout=600)
        out = r.stdout.decode(errors="replace").replace("\x00", "")
        for line in out.splitlines():
            if line.startswith(("CHECK", "RESULT", "folder")):
                print(line.rstrip())
        return 0 if "RESULT: PASS" in out else 1
    if cmd == "bench-quality":     # alvr_host --benchmark-quality on the builder (no headset), then checks on its results
        return bench_quality(argv[2] if len(argv) > 2 else "quick")
    if cmd == "report":
        d = Path(argv[2]) if len(argv) > 2 else lib.RESULTS / "latest"
        r = json.load(open(d / "result.json"))
        sc = load(r["scenario"])
        a = json.load(open(d / "analysis.json"))
        checks = lib.evaluate(sc, a)
        print_checks(checks)
        print(verdict(checks))
        return 0 if verdict(checks) == "PASS" else 1
    names = [p.stem for p in sorted(SCEN.glob("*.json"))] if cmd == "all" else argv[2:] if cmd == "run" else None
    if not names:
        print(__doc__)
        return 3
    statuses = {n: run_one(n) for n in names}
    print("\n== summary")
    for n, s in statuses.items():
        print(f"  {s:8s} {n}")
    vals = set(statuses.values())
    return 2 if "BLOCKED" in vals else 1 if "FAIL" in vals else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
