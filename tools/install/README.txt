VisionALVR 0.1 alpha - PC streamer for the ALVR app on Apple Vision Pro (OpenXR games, NVIDIA RTX 40 series or newer)
ALPHA SOFTWARE: use at your own risk.

1. Unzip this folder anywhere you like (it stays self-contained: config\ and logs\ are created inside it).
2. Run register_openxr_runtime.bat once (asks for administrator rights): makes this folder the Windows OpenXR runtime
   and allows alvr_host.exe through the firewall. unregister_openxr_runtime.bat puts your previous runtime back.
   Moved the folder? Run register_openxr_runtime.bat again from the new place.
3. Run configure.exe: open the ALVR app on the Vision Pro, pair it, check the settings (optional: benchmarks).
   Set the ALVR app's chroma key colour to the "colour when no game runs" (default pure green #00FF00).
4. Run VisionALVR.exe and keep it open while you play. It connects to the headset by itself; start any OpenXR game.
   Close SteamVR and ALVR's own launcher/dashboard first: they use the same network ports.

Logs: logs\VisionALVR.log (always), logs\debug\<date-time>\ (when debug logging is on). Send both when reporting a problem.

Credits: the benchmark scene bench\littlest_tokyo.vab is "Littlest Tokyo" by Glen Fox (glenatron,
https://sketchfab.com/glenatron), licensed CC-BY-4.0 (https://creativecommons.org/licenses/by/4.0/), from the three.js
examples. Changed: converted to VisionALVR's own format (meshes decompressed, simplified shading) and shown inside a room
tiled with the scene's own texture. stb_image (public domain / MIT) decodes its textures.
Third-party licences: THIRD_PARTY_NOTICES.md and licenses\.
