// configure.exe: one-time setup. 1) find the Vision Pro on the LAN (ALVR's discovery broadcast) and pair it, 2) optional
// benchmarks (encoder alone, then the network with the headset) that recommend a bitrate and show where the latency goes,
// 3) the settings VisionALVR actually uses (everything else stays in config\session.json for hand editing).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace VisionALVR
{
    public class ConfigureForm : Form
    {
        readonly AppSettings settings = new AppSettings();
        Dictionary<string, object> session = Session.Load();
        readonly Dictionary<string, object> check;
        Discovery discovery;
        readonly ListView found = new ListView { View = View.Details, FullRowSelect = true, Height = 150, Width = 760, HideSelection = false };
        readonly Label paired = Ui.Line("", Ui.Bold);
        readonly Label discStatus = Ui.Line("");
        readonly Dictionary<string, Discovery.Found> seen = new Dictionary<string, Discovery.Found>();
        // settings controls
        readonly NumericUpDown eyeW = Num(1024, 4096, 32), eyeH = Num(1024, 4096, 32), bitrate = Num(10, 1000, 10);
        readonly ComboBox fps = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 80 };
        readonly RadioButton profFov = new RadioButton { Text = "Foveated (ALVR foveated encoding; recommended)", AutoSize = true };
        readonly RadioButton profFull = new RadioButton { Text = "Full frame + split encode (experimental: 3x the pixels, needs a strong Wi-Fi link)", AutoSize = true };
        readonly NumericUpDown fcx = Dec(0.1m, 1m), fcy = Dec(0.1m, 1m), fsx = Dec(-1m, 1m), fsy = Dec(-1m, 1m), frx = Dec(1m, 10m), fry = Dec(1m, 10m);
        readonly Button idle = new Button { Width = 90, Height = 24, FlatStyle = FlatStyle.Flat };
        readonly CheckBox audio = new CheckBox { Text = "Stream game audio (Windows default output)", AutoSize = true };
        readonly CheckBox mute = new CheckBox { Text = "Mute the PC speakers while streaming", AutoSize = true };
        readonly ComboBox proto = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 80 };
        readonly NumericUpDown packet = Num(500, 4000, 100);
        readonly NumericUpDown haptics = Dec(0m, 5m);
        readonly CheckBox debugDefault = new CheckBox { Text = "Debug logging on by default (recommended during the alpha)", AutoSize = true };
        // benchmark
        readonly ListView benchList = new ListView { View = View.Details, FullRowSelect = true, Height = 190, Width = 640 };
        readonly Label benchStatus = Ui.Line("");
        readonly TextBox steps = new TextBox { Text = "100,150,200,250,300,350,400,500", Width = 260 };
        readonly NumericUpDown stepSecs = Num(4, 30, 1);
        readonly Button applyBitrate = new Button { Text = "Use recommended bitrate", AutoSize = true, Enabled = false };
        HostProcess bench;
        int recommended;

        static NumericUpDown Num(int min, int max, int inc) => new NumericUpDown { Minimum = min, Maximum = max, Increment = inc, Width = 80 };
        static NumericUpDown Dec(decimal min, decimal max) => new NumericUpDown { Minimum = min, Maximum = max, Increment = 0.05m, DecimalPlaces = 2, Width = 80 };

        public ConfigureForm(Dictionary<string, object> check)
        {
            this.check = check;
            Text = "VisionALVR - configure";
            Icon = Ui.AppIcon();
            AutoScaleMode = AutoScaleMode.Dpi;
            Font = Ui.Body;
            BackColor = Color.White;
            Width = 920;
            Height = 860;
            var tabs = new TabControl { Dock = DockStyle.Fill };
            // workflow: pair, then the benchmark (it fills the settings), then review the settings
            tabs.TabPages.Add(HeadsetTab());
            tabs.TabPages.Add(BenchTab());
            tabs.TabPages.Add(SettingsTab());
            var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, AutoSize = true, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(8) };
            var save = new Button { Text = "Save settings", AutoSize = true, Font = Ui.Bold };
            save.Click += (s, e) => SaveSettings(true);
            var launch = new Button { Text = "Save and start VisionALVR", AutoSize = true };
            launch.Click += (s, e) => { if (SaveSettings(false)) { System.Diagnostics.Process.Start(Paths.Runtime); Close(); } };
            bottom.Controls.Add(launch);
            bottom.Controls.Add(save);
            Controls.Add(tabs);
            Controls.Add(bottom);
            Controls.Add(Ui.Header("Configuration"));
            LoadSettings();
            Shown += (s, e) => StartDiscovery();
            var shot = Ui.ScreenshotPath();
            if (shot != null && shot.Contains("settings")) Shown += (s, e) => tabs.SelectedIndex = 2;
            if (shot != null && shot.Contains("bench")) Shown += (s, e) => tabs.SelectedIndex = 1;
            Ui.ScreenshotWhenShown(this, 4000);
            FormClosing += (s, e) => { discovery?.Dispose(); bench?.Stop(1000); };
        }

        // ---------------------------------------------------------------- 1. headset
        TabPage HeadsetTab()
        {
            var t = new TabPage("1. Headset") { BackColor = Color.White, Padding = new Padding(10) };
            var p = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true };
            p.Controls.Add(Ui.Line("Open the ALVR app on the Vision Pro (same network as this PC). It appears below within a few seconds."));
            found.Columns.Add("Name (hostname)", 260);
            found.Columns.Add("IP address", 140);
            found.Columns.Add("ALVR version", 170);
            p.Controls.Add(found);
            var pair = new Button { Text = "Pair the selected headset", AutoSize = true, Font = Ui.Bold };
            pair.Click += (s, e) => PairSelected();
            found.DoubleClick += (s, e) => PairSelected();
            p.Controls.Add(pair);
            p.Controls.Add(discStatus);
            p.Controls.Add(paired);
            p.Controls.Add(Ui.Line("When its address changes (DHCP), VisionALVR re-finds the paired headset by name on its own."));
            t.Controls.Add(p);
            ShowPaired();
            return t;
        }

        void ShowPaired()
        {
            var hs = Session.Headset(session);
            paired.Text = hs == null ? "Paired headset: none" : $"Paired headset: {hs.Item2} ({hs.Item1}) at {hs.Item3}";
            paired.ForeColor = hs == null ? Color.DarkOrange : Color.SeaGreen;
        }

        void StartDiscovery()
        {
            discovery?.Dispose();
            discovery = new Discovery();
            discovery.OnFound += f => BeginInvoke((Action)(() => AddFound(f)));
            var err = discovery.Start();
            discStatus.Text = err ?? "Listening for headsets (UDP 9943)...";
            discStatus.ForeColor = err == null ? Color.DimGray : Color.Firebrick;
        }

        void AddFound(Discovery.Found f)
        {
            var key = f.Hostname + "|" + f.Ip;
            if (seen.ContainsKey(key)) { seen[key].Seen = f.Seen; return; }
            seen[key] = f;
            var expected = check != null && check.TryGetValue("alvr_protocol_id", out var e) ? e as string : null;
            var compatible = expected == null || expected == f.Protocol.ToString(CultureInfo.InvariantCulture);
            var item = new ListViewItem(new[] { f.Hostname, f.Ip, compatible ? "compatible (ALVR " + (check != null && check.TryGetValue("alvr_protocol", out var pv) ? pv : "?") + ")" : "INCOMPATIBLE version" }) { Tag = f };
            if (!compatible) item.ForeColor = Color.Firebrick;
            found.Items.Add(item);
            if (found.Items.Count == 1) item.Selected = true;
        }

        void PairSelected()
        {
            if (found.SelectedItems.Count == 0) { MessageBox.Show(this, "Select a headset first (open the ALVR app on the Vision Pro).", Text); return; }
            var f = (Discovery.Found)found.SelectedItems[0].Tag;
            var name = f.Hostname.EndsWith(".client.alvr") ? "Apple Vision Pro" : f.Hostname;
            Session.Pair(session, f.Hostname, name, f.Ip);
            Json.Save(Paths.Session, session);
            settings.HeadsetHost = f.Hostname; settings.HeadsetName = name; settings.HeadsetIp = f.Ip;
            settings.Save();
            Log.Write("INFO", $"paired headset {name} ({f.Hostname}) at {f.Ip}");
            ShowPaired();
        }

        // ---------------------------------------------------------------- 2. settings
        TabPage SettingsTab()
        {
            var t = new TabPage("3. Settings") { BackColor = Color.White, Padding = new Padding(10), AutoScroll = true };
            var g = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Top };
            void row(string label, Control c) { g.Controls.Add(Ui.Line(label)); g.Controls.Add(c); }
            void section(string title) { var l = Ui.Line(title, Ui.Big); l.ForeColor = Ui.Accent; l.Margin = new Padding(3, 12, 3, 3); g.Controls.Add(l); g.SetColumnSpan(l, 2); }
            Control flow(params Control[] cs) { var f = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0) }; f.Controls.AddRange(cs); return f; }

            section("Video");
            row("Resolution per eye", flow(eyeW, Ui.Line("x"), eyeH, Ui.Line("(Vision Pro default 3552 x 3200)")));
            fps.Items.AddRange(new object[] { "90", "96", "100" });
            row("Refresh rate (Hz)", fps);
            row("Bitrate (Mbps)", flow(bitrate, Ui.Line("constant; the Benchmark tab can find yours")));
            var prof = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, Margin = new Padding(0) };
            prof.Controls.Add(profFov);
            prof.Controls.Add(profFull);
            row("Encoding", prof);
            row("Foveation center size", flow(fcx, fcy));
            row("Foveation center shift", flow(fsx, fsy));
            row("Foveation edge ratio", flow(frx, fry, Ui.Line("(higher = fewer pixels at the edges)")));
            idle.Click += (s, e) =>
            {
                using (var cd = new ColorDialog { Color = idle.BackColor, FullOpen = true })
                    if (cd.ShowDialog(this) == DialogResult.OK) SetIdle(cd.Color);
            };
            var idleReset = new Button { Text = "Pure green (recommended)", AutoSize = true };
            idleReset.Click += (s, e) => SetIdle(Color.FromArgb(0, 255, 0));
            row("Colour when no game runs", flow(idle, idleReset));
            var remind = Ui.Line("Set the SAME colour as the chroma key in the ALVR app on the Vision Pro (Settings > Chroma key), so it shows your room.");
            remind.ForeColor = Color.DimGray;
            g.Controls.Add(new Label()); g.Controls.Add(remind);

            section("Audio");
            g.Controls.Add(new Label()); g.Controls.Add(audio);
            g.Controls.Add(new Label()); g.Controls.Add(mute);

            section("Network and controllers");
            proto.Items.AddRange(new object[] { "Udp", "Tcp" });
            row("Stream protocol", flow(proto, Ui.Line("UDP is what the headset normally uses")));
            row("Packet size (bytes)", packet);
            row("Haptics strength", haptics);

            section("Logging");
            g.Controls.Add(new Label()); g.Controls.Add(debugDefault);
            var note = Ui.Line("Everything else (ALVR's advanced options) stays in config\\session.json.");
            note.ForeColor = Color.DimGray;
            g.Controls.Add(new Label()); g.Controls.Add(note);
            t.Controls.Add(g);
            return t;
        }

        void SetIdle(Color c) { idle.BackColor = c; idle.Text = $"#{c.R:X2}{c.G:X2}{c.B:X2}"; idle.ForeColor = c.GetBrightness() > 0.5 ? Color.Black : Color.White; }

        const string V = "session_settings.video.";

        void LoadSettings()
        {
            var s = session;
            eyeW.Value = Clamp(eyeW, Json.Num(s, V + "transcoding_view_resolution.Absolute.width", 3552));
            eyeH.Value = Clamp(eyeH, Json.Num(s, V + "transcoding_view_resolution.Absolute.height.content", 3200));
            var f = ((int)Json.Num(s, V + "preferred_fps", 90)).ToString(CultureInfo.InvariantCulture);
            fps.SelectedItem = fps.Items.Contains(f) ? f : "90";
            bitrate.Value = Clamp(bitrate, Json.Num(s, V + "bitrate.mode.ConstantMbps", 250));
            (settings.EncodeProfile == "full-split" ? profFull : profFov).Checked = true;
            fcx.Value = Clamp(fcx, Json.Num(s, V + "foveated_encoding.content.center_size_x", 0.45));
            fcy.Value = Clamp(fcy, Json.Num(s, V + "foveated_encoding.content.center_size_y", 0.4));
            fsx.Value = Clamp(fsx, Json.Num(s, V + "foveated_encoding.content.center_shift_x", 0.4));
            fsy.Value = Clamp(fsy, Json.Num(s, V + "foveated_encoding.content.center_shift_y", 0.1));
            frx.Value = Clamp(frx, Json.Num(s, V + "foveated_encoding.content.edge_ratio_x", 4));
            fry.Value = Clamp(fry, Json.Num(s, V + "foveated_encoding.content.edge_ratio_y", 5));
            try { SetIdle(ColorTranslator.FromHtml("#" + settings.IdleRgb.TrimStart('#'))); } catch { SetIdle(Color.FromArgb(0, 255, 0)); }
            audio.Checked = Json.Bool(s, "session_settings.audio.game_audio.enabled", true);
            mute.Checked = Json.Bool(s, "session_settings.audio.game_audio.content.mute_when_streaming", true);
            proto.SelectedItem = Json.Str(s, "session_settings.connection.stream_protocol.variant", "Udp") == "Tcp" ? "Tcp" : "Udp";
            packet.Value = Clamp(packet, Json.Num(s, "session_settings.connection.packet_size", 1400));
            haptics.Value = Clamp(haptics, Json.Num(s, "session_settings.headset.controllers.content.haptics.content.intensity_multiplier", 1));
            debugDefault.Checked = settings.Debug;
        }

        static decimal Clamp(NumericUpDown n, double v) => Math.Max(n.Minimum, Math.Min(n.Maximum, (decimal)v));

        bool SaveSettings(bool confirm)
        {
            session = Session.Load(); // keep anything edited meanwhile (e.g. pairing)
            var s = session;
            int w = (int)eyeW.Value / 32 * 32, h = (int)eyeH.Value; // ALVR aligns widths to 32
            foreach (var k in new[] { "transcoding_view_resolution", "emulated_headset_view_resolution" })
            {
                Json.Set(s, V + k + ".variant", "Absolute");
                Json.Set(s, V + k + ".Absolute.width", w);
                Json.Set(s, V + k + ".Absolute.height.set", true);
                Json.Set(s, V + k + ".Absolute.height.content", h);
            }
            Json.Set(s, V + "preferred_fps", double.Parse((string)fps.SelectedItem, CultureInfo.InvariantCulture));
            Json.Set(s, V + "bitrate.mode.variant", "ConstantMbps");
            Json.Set(s, V + "bitrate.mode.ConstantMbps", (int)bitrate.Value);
            Json.Set(s, V + "foveated_encoding.enabled", profFov.Checked);
            Json.Set(s, V + "foveated_encoding.content.center_size_x", (double)fcx.Value);
            Json.Set(s, V + "foveated_encoding.content.center_size_y", (double)fcy.Value);
            Json.Set(s, V + "foveated_encoding.content.center_shift_x", (double)fsx.Value);
            Json.Set(s, V + "foveated_encoding.content.center_shift_y", (double)fsy.Value);
            Json.Set(s, V + "foveated_encoding.content.edge_ratio_x", (double)frx.Value);
            Json.Set(s, V + "foveated_encoding.content.edge_ratio_y", (double)fry.Value);
            Json.Set(s, "session_settings.audio.game_audio.enabled", audio.Checked);
            Json.Set(s, "session_settings.audio.game_audio.content.mute_when_streaming", mute.Checked);
            Json.Set(s, "session_settings.connection.stream_protocol.variant", (string)proto.SelectedItem);
            Json.Set(s, "session_settings.connection.packet_size", (int)packet.Value);
            Json.Set(s, "session_settings.headset.controllers.content.haptics.content.intensity_multiplier", (double)haptics.Value);
            try { Json.Save(Paths.Session, s); }
            catch (Exception e) { MessageBox.Show(this, "Cannot write config\\session.json: " + e.Message, Text); return false; }
            settings.EncodeProfile = profFull.Checked ? "full-split" : "foveated";
            settings.IdleRgb = $"{idle.BackColor.R:x2}{idle.BackColor.G:x2}{idle.BackColor.B:x2}";
            settings.Debug = debugDefault.Checked;
            settings.Save();
            Log.Write("INFO", $"settings saved: {w}x{h}/eye @ {fps.SelectedItem} Hz, {bitrate.Value} Mbps, {settings.EncodeProfile}, idle #{settings.IdleRgb}, audio {audio.Checked}, {proto.SelectedItem}, debug {settings.Debug}");
            if (confirm) MessageBox.Show(this, "Saved. The changes apply the next time VisionALVR starts.", Text);
            return true;
        }

        // ---------------------------------------------------------------- 3. benchmark
        TabPage BenchTab()
        {
            var t = new TabPage("2. Benchmark") { BackColor = Color.White, Padding = new Padding(10) };
            var p = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, AutoScroll = true };
            p.Controls.Add(Ui.Line("Encoder test (no headset needed): how long NVENC takes per frame with each encoding.", Ui.Bold));
            var enc = new Button { Text = "Run encoder test", AutoSize = true };
            enc.Click += (s, e) => RunEncoderTest();
            p.Controls.Add(enc);
            p.Controls.Add(Ui.Line("Quality benchmark (no headset, ~1.5 min): the benchmark scene through foveation + NVENC at ~30 settings, picture vs time.", Ui.Bold));
            var qb = new Button { Text = "Run quality benchmark", AutoSize = true };
            qb.Click += (s, e) => RunQualityTest();
            p.Controls.Add(qb);
            var credit = Ui.Line("Scene: \"Littlest Tokyo\" by Glen Fox (glenatron), CC-BY-4.0, shown in a room tiled with its own texture (see README.txt).");
            credit.ForeColor = Color.DimGray;
            p.Controls.Add(credit);
            p.Controls.Add(Ui.Line("Network test (headset paired and the ALVR app open): random frames at rising bitrates; the headset shows noise.", Ui.Bold));
            var f = new FlowLayoutPanel { AutoSize = true };
            f.Controls.Add(Ui.Line("Bitrates (Mbps):")); f.Controls.Add(steps);
            f.Controls.Add(Ui.Line("seconds per step:")); stepSecs.Value = 8; f.Controls.Add(stepSecs);
            p.Controls.Add(f);
            var net = new Button { Text = "Run network test", AutoSize = true };
            net.Click += (s, e) => RunNetworkTest();
            p.Controls.Add(net);
            benchList.Width = 840;
            foreach (var c in new[] { ("Test", 200), ("Mbps", 60), ("fps", 50), ("lost/s", 60), ("total ms", 70), ("encode ms", 75), ("network ms", 80), ("decode ms", 75), ("result", 160) })
                benchList.Columns.Add(c.Item1, c.Item2);
            p.Controls.Add(benchList);
            p.Controls.Add(benchStatus);
            applyBitrate.Click += (s, e) =>
            {
                bitrate.Value = Clamp(bitrate, recommended);
                SaveSettings(false);
                MessageBox.Show(this, $"Bitrate set to {recommended} Mbps and saved.", Text);
            };
            p.Controls.Add(applyBitrate);
            var help = Ui.Line("Latency = what ALVR measures: total (tracking sent -> frame shown) = game + encode + network + decode + headset queues.");
            help.ForeColor = Color.DimGray;
            p.Controls.Add(help);
            t.Controls.Add(p);
            return t;
        }

        static string Fmt(Dictionary<string, object> d, string k, string f)
        {
            try { return d != null && d.TryGetValue(k, out var v) && v != null ? Convert.ToDouble(v, CultureInfo.InvariantCulture).ToString(f, CultureInfo.InvariantCulture) : "-"; }
            catch { return "-"; }
        }

        bool BenchBusy() { if (bench != null && bench.Running) { MessageBox.Show(this, "A test is already running.", Text); return true; } return false; }

        void RunEncoderTest()
        {
            if (BenchBusy()) return;
            discovery?.Dispose();
            var profiles = new Queue<string>(new[] { "foveated", "full-split" });
            benchStatus.Text = "Encoder test running...";
            void next()
            {
                if (profiles.Count == 0) { benchStatus.Text = "Encoder test done. 'fits' = p95 under 90% of the frame time."; StartDiscovery(); return; }
                var prof = profiles.Dequeue();
                bench = new HostProcess();
                bench.OnEvent += (n, d) => BeginInvoke((Action)(() =>
                {
                    if (n != "bench" || d == null) return;
                    var fits = d.TryGetValue("fits", out var ff) && ff is bool fb && fb;
                    var item = new ListViewItem(new[] { $"encoder {prof} {d["width"]}x{d["height"]}", "", Fmt(d, "fps", "0"), "", "", $"{Fmt(d, "p50_ms", "0.0")}/{Fmt(d, "p95_ms", "0.0")}", "", "", fits ? "fits" : "too slow" });
                    item.ForeColor = fits ? Color.SeaGreen : Color.Firebrick;
                    benchList.Items.Add(item);
                }));
                bench.OnExit += c => BeginInvoke((Action)next);
                var err = bench.Start($"--install-dir \"{Paths.Dir}\" --live --bench 300 --noise-temporal --encode-profile {prof}");
                if (err != null) { benchStatus.Text = err; StartDiscovery(); }
            }
            next();
        }

        static Dictionary<string, object> Sub(Dictionary<string, object> d, string k) =>
            d != null && d.TryGetValue(k, out var v) ? v as Dictionary<string, object> : null;

        // benchmark phase 2 (alvr_host --benchmark-quality): rows per configuration, the three proposals at the end. The proposals
        // are saved in config\benchmark_quality.json; applying them to the settings comes with the headset phase.
        void RunQualityTest()
        {
            if (BenchBusy()) return;
            if (!File.Exists(Path.Combine(Paths.Dir, "bench", "littlest_tokyo.vab"))) { MessageBox.Show(this, "bench\\littlest_tokyo.vab is missing from this folder.", Text); return; }
            SaveSettings(false);
            discovery?.Dispose();
            benchStatus.Text = "Quality benchmark: starting...";
            int n = 0;
            bench = new HostProcess();
            bench.OnEvent += (ev, d) => BeginInvoke((Action)(() =>
            {
                switch (ev)
                {
                    case "bq_start": benchStatus.Text = $"Quality benchmark on {d?["gpu"]} at {Fmt(d, "bitrate_mbps", "0")} Mbps..."; break;
                    case "bq_result":
                        n++;
                        var ok = d != null && d.TryGetValue("realtime", out var rt) && rt is bool rb && rb;
                        var ps = Sub(d, "psnr"); var em = Sub(d, "encode_ms");
                        var err = d != null && d.TryGetValue("error", out var e2) ? e2 as string : "";
                        var item = new ListViewItem(new[] { $"{d?["label"]}", Fmt(d, "produced_mbps", "0"), "", "", "", $"{Fmt(em, "p50", "0.0")}/{Fmt(em, "p95", "0.0")}", "", "",
                            string.IsNullOrEmpty(err) ? $"{Fmt(ps, "weighted", "0.0")} dB {(ok ? "real time" : "too slow")}" : "not supported" });
                        item.ForeColor = ok ? Color.SeaGreen : Color.Firebrick;
                        benchList.Items.Add(item);
                        benchList.EnsureVisible(benchList.Items.Count - 1);
                        benchStatus.Text = $"Quality benchmark: {n} configurations measured...";
                        break;
                    case "bq_summary":
                        var pr = Sub(d, "proposals");
                        string line(string k, string name)
                        {
                            var v = Sub(pr, k);
                            return v == null ? $"{name}: none" : $"{name}: {v["label"]} ({Fmt(Sub(v, "psnr"), "weighted", "0.0")} dB, encode {Fmt(Sub(v, "encode_ms"), "p95", "0.0")} ms)";
                        }
                        var clk = Sub(d, "gpu_clocks");
                        var stable = clk == null || !(clk.TryGetValue("stable", out var st) && st is bool sb && !sb);
                        benchStatus.Text = string.Join("\n", line("lowest_latency", "Lowest latency"), line("recommended", "Recommended"), line("best_quality", "Best quality"),
                            stable ? "Saved to config\\benchmark_quality.json (applying a proposal comes with the next version)." : "WARNING: the GPU clock changed during the test (laptop on battery? another GPU app?): run it again.");
                        Log.Write("INFO", "quality benchmark: " + benchStatus.Text.Replace("\n", " | "));
                        break;
                    case "bq_error": benchStatus.Text = $"Quality benchmark failed: {d?["message"]}"; break;
                }
            }));
            bench.OnExit += c => BeginInvoke((Action)(() => { if (c != 0 && !benchStatus.Text.StartsWith("Quality benchmark failed")) benchStatus.Text = $"Quality benchmark ended (code {c}) - see logs"; StartDiscovery(); }));
            var r = bench.Start($"--install-dir \"{Paths.Dir}\" --benchmark-quality");
            if (r != null) { benchStatus.Text = r; StartDiscovery(); }
        }

        void RunNetworkTest()
        {
            if (BenchBusy()) return;
            if (Session.Headset(Session.Load()) == null) { MessageBox.Show(this, "Pair the headset first (tab 1).", Text); return; }
            var list = string.Join(",", steps.Text.Split(',').Select(x => x.Trim()).Where(x => int.TryParse(x, out _)));
            if (list.Length == 0) { MessageBox.Show(this, "Enter bitrates in Mbps, e.g. 100,200,300", Text); return; }
            SaveSettings(false);
            discovery?.Dispose();
            applyBitrate.Enabled = false;
            benchStatus.Text = "Waiting for the headset (open the ALVR app on the Vision Pro)...";
            bench = new HostProcess();
            bench.OnEvent += (n, d) => BeginInvoke((Action)(() =>
            {
                switch (n)
                {
                    case "client_connected": benchStatus.Text = "Headset connected; testing..."; break;
                    case "bench_step_start": benchStatus.Text = $"Testing {d?["target_mbps"]} Mbps (step {Convert.ToInt32(d?["step"]) + 1})..."; break;
                    case "bench_step":
                        benchList.Items.Add(new ListViewItem(new[] { $"network {Fmt(d, "target_mbps", "0")} Mbps", Fmt(d, "actual_mbps", "0"), Fmt(d, "client_fps", "0"),
                            Fmt(d, "packets_lost_per_s", "0.0"), Fmt(d, "total_latency_ms", "0.0"), Fmt(d, "encode_ms", "0.0"), Fmt(d, "network_ms", "0.0"), Fmt(d, "decode_ms", "0.0"), "" }));
                        break;
                    case "bench_result":
                        var rec = d != null && d.TryGetValue("recommended_mbps", out var r) && r != null ? Convert.ToInt32(r) : 0;
                        if (d != null && d.TryGetValue("steps", out var st) && st is object[] arr)
                            for (int i = 0; i < arr.Length && i < benchList.Items.Count; i++)
                            {
                                var row = benchList.Items[benchList.Items.Count - arr.Length + i];
                                var ok = (arr[i] as Dictionary<string, object>)?["ok"] is bool b && b;
                                row.SubItems[8].Text = ok ? "ok" : string.Join(", ", ((arr[i] as Dictionary<string, object>)?["why"] as object[] ?? new object[0]).Cast<string>());
                                row.ForeColor = ok ? Color.SeaGreen : Color.Firebrick;
                            }
                        recommended = rec;
                        applyBitrate.Enabled = rec > 0;
                        benchStatus.Text = rec > 0 ? $"Recommended bitrate: {rec} Mbps (highest clean step minus 10%)." : "No step passed: " + (d?["note"] ?? "");
                        Log.Write("INFO", "network benchmark: " + benchStatus.Text);
                        break;
                }
            }));
            bench.OnExit += c => BeginInvoke((Action)(() => { if (!applyBitrate.Enabled && !benchStatus.Text.StartsWith("No step")) benchStatus.Text = $"Network test ended (code {c}) - see logs"; StartDiscovery(); }));
            var err = bench.Start($"--install-dir \"{Paths.Dir}\" --benchmark-network {list} --bench-step-s {stepSecs.Value} --connect-timeout 120 --seconds 900 --encode-profile {(profFull.Checked ? "full-split" : "foveated")}");
            if (err != null) { benchStatus.Text = err; StartDiscovery(); }
        }

        [STAThread]
        static void Main()
        {
            Log.Component = "configure";
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Log.Write("INFO", $"configure.exe {Version.Text} start ({Paths.Dir})");
            var check = File.Exists(Paths.Host) ? SystemCheck.Run() : null;
            if (check != null && !(check.TryGetValue("gpu_supported", out var g) && g is bool b && b) && Ui.ScreenshotPath() == null)
                MessageBox.Show("Note: " + (check.TryGetValue("reason", out var r) ? r : "unsupported GPU") + ". VisionALVR.exe will refuse to stream.", "VisionALVR");
            Application.Run(new ConfigureForm(check));
        }
    }
}
