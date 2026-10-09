// VisionALVR.exe: the runtime window. Checks the GPU and the OpenXR registration, starts the streamer host as soon as it opens
// (no start button), shows the headset / game state and the key numbers every 10 s, and offers the live settings (display gamma,
// debug logging). Pairing and the one-time settings live in configure.exe.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Windows.Forms;

namespace VisionALVR
{
    public class RuntimeForm : Form
    {
        readonly AppSettings settings = new AppSettings();
        readonly HostProcess host = new HostProcess();
        readonly Label headset = Ui.Line("Headset: -", Ui.Big);
        readonly Label game = Ui.Line("Game: none", Ui.Bold);
        readonly Label runtime = Ui.Line("");
        readonly Label stats = Ui.Line("Waiting for the first statistics (every 10 s)...");
        // display controls: (label, min, max, default, settings getter/setter), values x100 on the track bars
        class Knob { public string Name; public TrackBar Bar; public Label Value; public double Def; public Func<double> Get; public Action<double> Set; }
        readonly List<Knob> knobs = new List<Knob>();
        readonly CheckBox debug = new CheckBox { Text = "Debug logging (verbose files in logs\\debug\\)", AutoSize = true, Font = Ui.Body };
        readonly Label footer = Ui.Line("", new Font("Segoe UI", 8.5f));
        string pairedName = "", pairedIp = "";
        bool closing;
        int restarts;
        string setupError;   // the streamer's `setup_error` message: a problem to fix, restarting it would not help

        public RuntimeForm(Dictionary<string, object> check)
        {
            Text = "VisionALVR";
            Icon = Ui.AppIcon();
            AutoScaleMode = AutoScaleMode.Dpi;
            Font = Ui.Body;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            BackColor = Color.White;

            var body = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1, Padding = new Padding(12, 4, 12, 12) };
            var gpu = check != null && check.TryGetValue("gpu", out var g) ? g as Dictionary<string, object> : null;
            runtime.Text = $"GPU: {S(gpu, "name")} ({S(gpu, "arch_name")}, driver {S(gpu, "driver")})   ·   OpenXR runtime: {(OpenXr.IsOurs() ? "VisionALVR (this folder)" : "NOT registered")}";
            runtime.ForeColor = Color.DimGray;
            body.Controls.Add(headset);
            body.Controls.Add(game);
            body.Controls.Add(runtime);

            var box = new GroupBox { Text = "Streaming (updated every 10 s)", AutoSize = true, Dock = DockStyle.Fill, Padding = new Padding(8), Font = Ui.Bold };
            stats.Font = new Font("Consolas", 9.5f);
            box.Controls.Add(stats);
            stats.Location = new Point(10, 22);
            body.Controls.Add(box);

            var live = new GroupBox { Text = "Display (applied live)", AutoSize = true, Dock = DockStyle.Fill, Padding = new Padding(8), Font = Ui.Bold };
            var lt = new TableLayoutPanel { AutoSize = true, ColumnCount = 3, Location = new Point(8, 20), Font = Ui.Body };
            if (!settings.HasDisplay) { settings.Gamma = AppSettings.DefaultGamma; settings.Save(); } // the streamer reads it at start
            AddKnob(lt, "Gamma (>1 brighter)", 50, 200, AppSettings.DefaultGamma, () => settings.Gamma, v => settings.Gamma = v);
            AddKnob(lt, "Brightness", -30, 30, 0, () => settings.Brightness, v => settings.Brightness = v);
            AddKnob(lt, "Contrast", -30, 30, 0, () => settings.Contrast, v => settings.Contrast = v);
            AddKnob(lt, "Saturation (<0 less vivid)", -50, 50, 0, () => settings.Saturation, v => settings.Saturation = v);
            AddKnob(lt, "Sharpening", 0, 100, 0, () => settings.Sharpening, v => settings.Sharpening = v);
            var reset = new Button { Text = "Defaults (gamma 1.2, rest neutral)", AutoSize = true };
            reset.Click += (s, e) => { foreach (var k in knobs) k.Bar.Value = (int)Math.Round(k.Def * 100); };
            lt.Controls.Add(reset);
            lt.SetColumnSpan(reset, 3);
            lt.Controls.Add(debug);
            lt.SetColumnSpan(debug, 3);
            live.Controls.Add(lt);
            body.Controls.Add(live);

            var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill };
            var logs = new Button { Text = "Open logs folder", AutoSize = true };
            logs.Click += (s, e) => { Directory.CreateDirectory(Paths.Logs); Process.Start("explorer.exe", Paths.Logs); };
            var conf = new Button { Text = "Configure... (stops streaming)", AutoSize = true };
            conf.Click += (s, e) => OpenConfigure();
            buttons.Controls.Add(logs);
            buttons.Controls.Add(conf);
            body.Controls.Add(buttons);
            footer.ForeColor = Color.Gray;
            body.Controls.Add(footer);

            Controls.Add(body);
            Controls.Add(Ui.Header("Streamer for Apple Vision Pro (ALVR visionOS app)"));


            debug.Checked = settings.Debug;
            debug.CheckedChanged += (s, e) =>
            {
                host.Send("debug " + (debug.Checked ? "on" : "off"));
                settings.Debug = debug.Checked;
                settings.Save();
            };

            host.OnEvent += (name, data) => BeginInvoke((Action)(() => HostEvent(name, data)));
            host.OnExit += code => BeginInvoke((Action)(() => HostExited(code)));
            Shown += (s, e) => StartHost();
            Ui.ScreenshotWhenShown(this, 12000);
            FormClosing += (s, e) => { closing = true; footer.Text = "Stopping the streamer..."; Refresh(); host.Stop(); Log.Write("INFO", "VisionALVR.exe closed"); };
        }

        void AddKnob(TableLayoutPanel t, string name, int min, int max, double def, Func<double> get, Action<double> set)
        {
            var k = new Knob { Name = name, Def = def, Get = get, Set = set, Value = Ui.Line("0.00"),
                               Bar = new TrackBar { Minimum = min, Maximum = max, TickFrequency = (max - min) / 6, SmallChange = 5, LargeChange = 10, Width = 280, AutoSize = false, Height = 32 } };
            k.Bar.Value = (int)Math.Max(min, Math.Min(max, Math.Round(get() * 100)));
            k.Value.Text = (k.Bar.Value / 100.0).ToString("0.00", CultureInfo.InvariantCulture);
            k.Bar.ValueChanged += (s, e) =>
            {
                var v = k.Bar.Value / 100.0;
                k.Value.Text = v.ToString("0.00", CultureInfo.InvariantCulture);
                k.Set(v);
                settings.Save();
                SendDisplay();
            };
            t.Controls.Add(Ui.Line(name + ":"));
            t.Controls.Add(k.Bar);
            t.Controls.Add(k.Value);
            knobs.Add(k);
        }

        void SendDisplay()
        {
            string f(double v) => v.ToString("0.00", CultureInfo.InvariantCulture);
            host.Send("gamma " + f(settings.Gamma));
            host.Send($"color {f(settings.Brightness)} {f(settings.Contrast)} {f(settings.Saturation)} {f(settings.Sharpening)}");
        }

        void StartHost()
        {
            var hs = Session.Headset(Session.Load());
            pairedName = hs?.Item2 ?? "";
            pairedIp = hs?.Item3 ?? "";
            headset.Text = hs == null ? "Headset: none paired - use Configure..." : $"Headset: searching for {pairedName} ({pairedIp}) ...";
            headset.ForeColor = Color.DarkOrange;
            var args = $"--install-dir \"{Paths.Dir}\" --shim --daemon --gui" + (debug.Checked ? " --debug" : "");
            var err = host.Start(args);
            if (err != null)
            {
                MessageBox.Show(this, err, "VisionALVR", MessageBoxButtons.OK, MessageBoxIcon.Error);
                Log.Write("ERROR", err);
                return;
            }
            footer.Text = "Streamer running. Open the ALVR app on the Vision Pro; it connects by itself. Close this window to stop.";
            Log.Write("INFO", "streamer started: alvr_host " + args);
        }

        void HostExited(int code)
        {
            if (closing) return;
            Log.Write("ERROR", $"alvr_host.exe exited unexpectedly (code {code})");
            headset.Text = "Streamer stopped (see logs)";
            headset.ForeColor = Color.Firebrick;
            if (code == 7)   // setup problem (e.g. config\session.json missing or broken): say what to do, do not restart
            {
                headset.Text = "Setup problem: the streamer cannot start";
                footer.Text = setupError ?? "The streamer reported a setup problem. Check logs\\VisionALVR.log.";
                footer.ForeColor = Color.Firebrick;
                return;
            }
            if (restarts++ < 3)
            {
                footer.Text = $"The streamer exited (code {code}); restarting ({restarts}/3)...";
                var t = new Timer { Interval = 2000 };
                t.Tick += (s, e) => { t.Stop(); t.Dispose(); StartHost(); };
                t.Start();
            }
            else footer.Text = $"The streamer exited (code {code}) several times. Check logs\\VisionALVR.log.";
        }

        static string S(Dictionary<string, object> d, string k) => d != null && d.TryGetValue(k, out var v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : "";
        static double D(Dictionary<string, object> d, string k) { try { return d != null && d.TryGetValue(k, out var v) && v != null ? Convert.ToDouble(v, CultureInfo.InvariantCulture) : double.NaN; } catch { return double.NaN; } }
        static string F(double v, string fmt) => double.IsNaN(v) ? "  -" : v.ToString(fmt, CultureInfo.InvariantCulture);

        void HostEvent(string name, Dictionary<string, object> d)
        {
            switch (name)
            {
                case "setup_error":
                    setupError = S(d, "message");
                    break;
                case "client_connected":
                    var n = S(d, "name");
                    var ip = S(d, "ip");
                    headset.Text = $"Headset: connected - {(n.Length > 0 ? n : pairedName)} ({(ip.Length > 0 ? ip : pairedIp)})";
                    headset.ForeColor = Color.SeaGreen;
                    if (ip.Length > 0 && ip != settings.HeadsetIp) { settings.HeadsetIp = ip; settings.Save(); }
                    break;
                case "client_disconnected":
                    headset.Text = $"Headset: searching for {pairedName} ({pairedIp}) ...";
                    headset.ForeColor = Color.DarkOrange;
                    break;
                case "game_started":
                    game.Text = "Game: " + S(d, "exe");
                    break;
                case "game_ended":
                    game.Text = $"Game: none (last: {S(d, "exe")}, {F(D(d, "seconds") / 60, "0.0")} min)";
                    break;
                case "status":
                    var c = d != null && d.TryGetValue("client", out var cc) ? cc as Dictionary<string, object> : null;
                    stats.Text =
                        $"Game fps {F(D(d, "app_fps"), "0.0"),6}    streamed fps {F(D(d, "stream_fps"), "0.0"),6}    headset fps {F(D(c, "client_fps"), "0.0"),6}\n" +
                        $"Bitrate  {F(D(d, "mbps"), "0"),5} Mbps   frames not streamed {S(d, "frames_not_streamed"),4}   packets lost {S(d, "packets_lost"),4}\n" +
                        $"Latency  total {F(D(c, "total_latency_ms"), "0.0"),5} ms = encode {F(D(c, "encode_ms"), "0.0")} + network {F(D(c, "network_ms"), "0.0")} + decode {F(D(c, "decode_ms"), "0.0")} + other";
                    break;
                case "encode_error":
                    footer.Text = "Encode error: " + S(d, "message");
                    break;
            }
        }

        void OpenConfigure()
        {
            if (MessageBox.Show(this, "configure.exe needs the network port the streamer uses: streaming stops while it is open. Continue?",
                    "VisionALVR", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            closing = true;
            host.Stop();
            Process.Start(Paths.Configure);
            Close();
        }

        [STAThread]
        static void Main()
        {
            Log.Component = "gui";
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            using (new System.Threading.Mutex(true, "VisionALVR_Runtime_Window", out bool first))
            {
                if (!first) { MessageBox.Show("VisionALVR is already running.", "VisionALVR"); return; }
                Log.Write("INFO", $"VisionALVR.exe {Version.Text} start ({Paths.Dir})");
                if (!File.Exists(Paths.Host))
                {
                    MessageBox.Show("alvr_host.exe was not found next to VisionALVR.exe. Unzip the whole VisionALVR folder.", "VisionALVR", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                var check = SystemCheck.Run();
                if (check == null || !(check.TryGetValue("ok", out var ok) && ok is bool b && b))
                {
                    var why = check != null && check.TryGetValue("reason", out var r) ? r as string : "the system check failed (see logs)";
                    Log.Write("ERROR", "refusing to start: " + why);
                    MessageBox.Show("VisionALVR cannot run on this PC: " + why, "VisionALVR", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                var testing = Ui.ScreenshotPath() != null;
                if (!OpenXr.IsOurs() && !testing)
                {
                    var cur = OpenXr.ActiveRuntime();
                    var q = MessageBox.Show($"VisionALVR is not the Windows OpenXR runtime (currently: {(cur.Length > 0 ? cur : "none")}).\n\n" +
                                            "Register this folder now? (asks for administrator rights; unregister_openxr_runtime.bat puts the previous one back)",
                                            "VisionALVR", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
                    if (q == DialogResult.Yes && !OpenXr.Register(null))
                        MessageBox.Show("Registration did not take effect. Run register_openxr_runtime.bat from this folder.", "VisionALVR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
                if (Session.Headset(Session.Load()) == null && !testing)
                {
                    if (MessageBox.Show("No headset is paired yet. Open configure.exe to pair your Vision Pro?", "VisionALVR", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                    {
                        Process.Start(Paths.Configure);
                        return;
                    }
                }
                Application.Run(new RuntimeForm(check));
            }
        }
    }
}
