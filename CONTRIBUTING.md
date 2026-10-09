# Contributing

Thanks for helping. A few things keep this project workable:

## Scope
- **Headset: Apple Vision Pro with the stock ALVR visionOS app only.** The ALVR 20.14.1 protocol stays unchanged so the app
  cannot tell the difference. Other ALVR headsets may connect (the protocol is device agnostic) but are out of scope: issues
  and pull requests for them will be closed (use ALVR itself).
- **GPU: NVIDIA only** (NVENC), RTX 40 series or newer for users. The point of the project is an encode path written for one
  vendor. **OS: Windows** for the streamer.
- OpenXR games only (no SteamVR/OpenVR games).

## Before a pull request
1. Set up a builder and run the harness (`docs/DEVELOPMENT.md`).
2. Run the scenarios your change touches, and `python3 harness/run.py all` for anything in the host, the encoder library or the
   shim. Installer changes: `test-install`; package changes: `test-zip`; benchmark changes: `bench-quality full`.
3. In the pull request, say what you **tested** (scenario names, measurements, headset if any) and what you did **not**
   test. "Builds" is not "works". Add a line to `docs/progress.md` for anything a user would notice.

## Code
- Keep upstream code unmodified where it is vendored (`nvenc/upstream/`): change it through the asserted build-time patches in
  `tools/host/build.rs`, so a changed upstream fails loudly.
- Match the surrounding style and comment density. No new dependencies without a reason in the PR (the harness is stdlib
  Python on purpose; the GUIs need nothing beyond .NET Framework 4.8).
- Never commit logs, `harness/results/`, session files with your headset in them, or `tools/builder.local`.

## Bug reports
Attach `logs\VisionALVR.log` and the matching `logs\debug\<time>\` folder (turn debug on in VisionALVR.exe, reproduce, then zip
the folder), the game and how it is started (UEVR, mod, native OpenXR), GPU and driver.
