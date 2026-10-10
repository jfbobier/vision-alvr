// Shared by VisionALVR.exe (runtime) and configure.exe: install layout, JSON, the general log, headset discovery, the host
// process, the system check, OpenXR registration and the common window header. .NET Framework 4.8 (part of Windows 10/11).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Win32;

namespace VisionALVR
{
    public static class Version
    {
        public const string Text = "0.2 alpha";
    }

    /// The portable folder: everything next to the exe, config/ and logs/ inside it.
    public static class Paths
    {
        public static readonly string Dir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
        public static string Config => Path.Combine(Dir, "config");
        public static string Logs => Path.Combine(Dir, "logs");
        public static string Session => Path.Combine(Config, "session.json");
        public static string SessionDefault => Path.Combine(Config, "session.default.json");
        public static string Settings => Path.Combine(Config, "visionalvr.json");
        public static string Host => Path.Combine(Dir, "alvr_host.exe");
        public static string Manifest => Path.Combine(Dir, "virtualdesktop-openxr.json");
        public static string RegisterScript => Path.Combine(Dir, "register_openxr_runtime.ps1");
        public static string Configure => Path.Combine(Dir, "configure.exe");
        public static string Runtime => Path.Combine(Dir, "VisionALVR.exe");
    }

    /// JSON as Dictionary / object[] trees (JavaScriptSerializer), written back indented so session.json stays hand-editable.
    public static class Json
    {
        static readonly JavaScriptSerializer Ser = new JavaScriptSerializer { MaxJsonLength = int.MaxValue, RecursionLimit = 1000 };

        public static Dictionary<string, object> Parse(string text) => Ser.DeserializeObject(text) as Dictionary<string, object> ?? new Dictionary<string, object>();

        public static Dictionary<string, object> Load(string path)
        {
            try { return File.Exists(path) ? Parse(File.ReadAllText(path)) : new Dictionary<string, object>(); }
            catch (Exception e) { Log.Write("WARN", $"cannot read {path}: {e.Message}"); return new Dictionary<string, object>(); }
        }

        public static void Save(string path, Dictionary<string, object> root)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            var sb = new StringBuilder();
            Write(sb, root, 0);
            var tmp = path + ".tmp";
            File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false)); // no BOM: ALVR's serde_json rejects it
            if (File.Exists(path)) File.Replace(tmp, path, null); else File.Move(tmp, path);
        }

        static void Write(StringBuilder sb, object v, int ind)
        {
            string pad(int n) => new string(' ', n * 2);
            switch (v)
            {
                case null: sb.Append("null"); break;
                case string s: sb.Append(Ser.Serialize(s)); break;
                case bool b: sb.Append(b ? "true" : "false"); break;
                case Dictionary<string, object> d:
                    if (d.Count == 0) { sb.Append("{}"); break; }
                    sb.Append("{\n");
                    int i = 0;
                    foreach (var kv in d)
                    {
                        sb.Append(pad(ind + 1)).Append(Ser.Serialize(kv.Key)).Append(": ");
                        Write(sb, kv.Value, ind + 1);
                        sb.Append(++i < d.Count ? ",\n" : "\n");
                    }
                    sb.Append(pad(ind)).Append('}');
                    break;
                case System.Collections.IEnumerable list:
                    var items = list.Cast<object>().ToList();
                    if (items.Count == 0) { sb.Append("[]"); break; }
                    sb.Append("[\n");
                    for (int k = 0; k < items.Count; k++)
                    {
                        sb.Append(pad(ind + 1));
                        Write(sb, items[k], ind + 1);
                        sb.Append(k + 1 < items.Count ? ",\n" : "\n");
                    }
                    sb.Append(pad(ind)).Append(']');
                    break;
                case double dd: sb.Append(Num(dd)); break;
                case float ff: sb.Append(Num(ff)); break;
                case decimal m: sb.Append(m.ToString(CultureInfo.InvariantCulture)); break;
                default: sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture)); break;
            }
        }

        static string Num(double d) => (d == Math.Floor(d) && Math.Abs(d) < 1e15) ? d.ToString("0.0", CultureInfo.InvariantCulture) : d.ToString("R", CultureInfo.InvariantCulture);

        public static object Get(Dictionary<string, object> root, string path)
        {
            object cur = root;
            foreach (var k in path.Split('.'))
            {
                if (!(cur is Dictionary<string, object> d) || !d.TryGetValue(k, out cur)) return null;
            }
            return cur;
        }

        public static void Set(Dictionary<string, object> root, string path, object value)
        {
            var keys = path.Split('.');
            var cur = root;
            for (int i = 0; i < keys.Length - 1; i++)
            {
                if (!(cur.TryGetValue(keys[i], out var next) && next is Dictionary<string, object> nd))
                {
                    nd = new Dictionary<string, object>();
                    cur[keys[i]] = nd;
                }
                cur = nd;
            }
            cur[keys[keys.Length - 1]] = value;
        }

        public static double Num(Dictionary<string, object> root, string path, double def)
        {
            var v = Get(root, path);
            try { return v == null || v is string || v is bool ? def : Convert.ToDouble(v, CultureInfo.InvariantCulture); } catch { return def; }
        }

        public static string Str(Dictionary<string, object> root, string path, string def) => Get(root, path) as string ?? def;
        public static bool Bool(Dictionary<string, object> root, string path, bool def) => Get(root, path) is bool b ? b : def;
        public static string Serialize(object o) => Ser.Serialize(o);
    }

    /// The general log logs\VisionALVR.log, shared with the host and the shim (one appended write per line).
    public static class Log
    {
        public static string Component = "gui";
        static readonly object Lock = new object();

        public static void Write(string level, string msg)
        {
            try
            {
                lock (Lock)
                {
                    Directory.CreateDirectory(Paths.Logs);
                    var line = $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{Component}] {level,-5} {msg}\r\n";
                    using (var fs = new FileStream(Path.Combine(Paths.Logs, "VisionALVR.log"), FileMode.Append, FileAccess.Write, FileShare.ReadWrite | FileShare.Delete))
                    {
                        var b = Encoding.UTF8.GetBytes(line);
                        fs.Write(b, 0, b.Length);
                    }
                }
            }
            catch { }
        }
    }

    /// config\visionalvr.json: VisionALVR's own settings (the ALVR ones stay in session.json).
    public class AppSettings
    {
        public Dictionary<string, object> Root = Json.Load(Paths.Settings);
        public string HeadsetHost { get => Json.Str(Root, "headset.hostname", ""); set => Json.Set(Root, "headset.hostname", value); }
        public string HeadsetName { get => Json.Str(Root, "headset.name", ""); set => Json.Set(Root, "headset.name", value); }
        public string HeadsetIp { get => Json.Str(Root, "headset.ip", ""); set => Json.Set(Root, "headset.ip", value); }
        public string EncodeProfile { get => Json.Str(Root, "video.encode_profile", "foveated"); set => Json.Set(Root, "video.encode_profile", value); }
        public string IdleRgb { get => Json.Str(Root, "video.idle_rgb", "00ff00"); set => Json.Set(Root, "video.idle_rgb", value); }
        public bool Debug { get => Json.Bool(Root, "debug", true); set => Json.Set(Root, "debug", value); }
        /// display (live in VisionALVR.exe): gamma 1.2 prefilled (users' AVP experience with ALVR), the rest neutral
        public const double DefaultGamma = 1.2;
        public double Gamma { get => Json.Num(Root, "display.gamma", DefaultGamma); set => Json.Set(Root, "display.gamma", value); }
        public double Brightness { get => Json.Num(Root, "display.brightness", 0); set => Json.Set(Root, "display.brightness", value); }
        public double Contrast { get => Json.Num(Root, "display.contrast", 0); set => Json.Set(Root, "display.contrast", value); }
        public double Saturation { get => Json.Num(Root, "display.saturation", 0); set => Json.Set(Root, "display.saturation", value); }
        public double Sharpening { get => Json.Num(Root, "display.sharpening", 0); set => Json.Set(Root, "display.sharpening", value); }
        public bool HasDisplay => Json.Get(Root, "display.gamma") != null;
        public void Save() => Json.Save(Paths.Settings, Root);
    }

    /// The session (ALVR settings, paired headset). Created from session.default.json on first use.
    public static class Session
    {
        public static Dictionary<string, object> Load()
        {
            if (!File.Exists(Paths.Session) && File.Exists(Paths.SessionDefault)) File.Copy(Paths.SessionDefault, Paths.Session);
            return Json.Load(Paths.Session);
        }

        /// (hostname, display name, ip) of the trusted headset, or null.
        public static Tuple<string, string, string> Headset(Dictionary<string, object> s)
        {
            if (!(Json.Get(s, "client_connections") is Dictionary<string, object> cc)) return null;
            foreach (var kv in cc)
            {
                if (!(kv.Value is Dictionary<string, object> c) || !(c.TryGetValue("trusted", out var t) && t is bool tb && tb)) continue;
                var ip = c.TryGetValue("current_ip", out var cur) && cur is string cs && cs.Length > 0 ? cs
                       : (c.TryGetValue("manual_ips", out var m) && m is object[] a && a.Length > 0 ? a[0] as string : "");
                return Tuple.Create(kv.Key, c.TryGetValue("display_name", out var n) ? n as string ?? kv.Key : kv.Key, ip ?? "");
            }
            return null;
        }

        /// Pairs exactly one headset: trusted, dialled at `ip` (ALVR still re-finds it by name if its address changes).
        public static void Pair(Dictionary<string, object> s, string hostname, string name, string ip)
        {
            s["client_connections"] = new Dictionary<string, object>
            {
                [hostname] = new Dictionary<string, object>
                {
                    ["display_name"] = name, ["current_ip"] = ip, ["manual_ips"] = new object[] { ip }, ["trusted"] = true,
                    ["connection_state"] = "Disconnected",
                },
            };
        }
    }

    /// Headset discovery, done by alvr_host.exe (`--discover`, ALVR's own code: mDNS/Bonjour for the Vision Pro, which never
    /// broadcasts on UDP 9943, and the UDP 9943 broadcast of Quest-style clients). The GUI only shows the `headset_seen` events.
    public class Discovery : IDisposable
    {
        public class Found { public string Hostname, Ip, Protocol, ServerProtocol, Via; public bool Compatible; public DateTime Seen; }
        public event Action<Found> OnFound;
        public event Action<string> OnError;
        HostProcess host;

        public string Start(double seconds = 3600)
        {
            host = new HostProcess();
            host.OnEvent += (name, d) =>
            {
                if (name == "headset_seen" && d != null)
                    OnFound?.Invoke(new Found
                    {
                        Hostname = Str(d, "hostname"), Ip = Str(d, "ip"), Protocol = Str(d, "protocol"), ServerProtocol = Str(d, "server_protocol"),
                        Via = Str(d, "via"), Compatible = d.TryGetValue("compatible", out var c) && c is bool b && b, Seen = DateTime.Now,
                    });
                else if (name == "discovery_error" && d != null) OnError?.Invoke(Str(d, "message"));
            };
            return host.Start($"--install-dir \"{Paths.Dir}\" --discover {seconds.ToString(CultureInfo.InvariantCulture)}");
        }

        static string Str(Dictionary<string, object> d, string k) => d.TryGetValue(k, out var v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : "";

        public void Dispose() { try { host?.Stop(1000); } catch { } }
    }

    /// alvr_host.exe as a child process: JSON events from its stdout, commands to its stdin (end of stdin = it quits).
    public class HostProcess
    {
        public event Action<string, Dictionary<string, object>> OnEvent;
        public event Action<int> OnExit;
        public event Action<string> OnStderr;
        Process p;
        public bool Running => p != null && !p.HasExited;

        public string Start(string args)
        {
            if (!File.Exists(Paths.Host)) return "alvr_host.exe is missing from " + Paths.Dir;
            var psi = new ProcessStartInfo(Paths.Host, args)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
                RedirectStandardInput = true, WorkingDirectory = Paths.Dir, StandardOutputEncoding = Encoding.UTF8,
            };
            p = new Process { StartInfo = psi, EnableRaisingEvents = true };
            p.OutputDataReceived += (s, e) =>
            {
                if (e.Data == null || !e.Data.StartsWith("{")) return;
                try
                {
                    var d = Json.Parse(e.Data);
                    OnEvent?.Invoke(d.TryGetValue("event", out var n) ? n as string : "", d.TryGetValue("data", out var x) ? x as Dictionary<string, object> : null);
                }
                catch { }
            };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) OnStderr?.Invoke(e.Data); };
            p.Exited += (s, e) => OnExit?.Invoke(SafeExitCode());
            try { p.Start(); } catch (Exception e) { return "cannot start alvr_host.exe: " + e.Message; }
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            return null;
        }

        int SafeExitCode() { try { return p.ExitCode; } catch { return -1; } }

        public void Send(string line) { try { if (Running) { p.StandardInput.WriteLine(line); p.StandardInput.Flush(); } } catch { } }

        /// Asks the host to quit (it un-mutes the PC speakers on the way out); kills it after the timeout.
        public void Stop(int timeoutMs = 6000)
        {
            if (!Running) return;
            Send("quit");
            try { p.StandardInput.Close(); } catch { }
            if (!p.WaitForExit(timeoutMs)) { try { p.Kill(); } catch { } }
        }
    }

    /// `alvr_host --system-check`: the GPU (NVML architecture: Ada = 8, Blackwell = 10) and ALVR's protocol id.
    public static class SystemCheck
    {
        public static Dictionary<string, object> Run()
        {
            try
            {
                var psi = new ProcessStartInfo(Paths.Host, "--system-check") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                using (var p = Process.Start(psi))
                {
                    var outp = p.StandardOutput.ReadToEnd();
                    p.WaitForExit(15000);
                    foreach (var line in outp.Split('\n'))
                    {
                        if (!line.TrimStart().StartsWith("{")) continue;
                        var d = Json.Parse(line);
                        if (d.TryGetValue("data", out var x) && x is Dictionary<string, object> data) return data;
                    }
                }
            }
            catch (Exception e) { Log.Write("ERROR", "system check failed: " + e.Message); }
            return null;
        }
    }

    /// HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime == this folder's manifest? Registration runs the script elevated.
    public static class OpenXr
    {
        public static string ActiveRuntime()
        {
            try
            {
                using (var k = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64).OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1"))
                    return k?.GetValue("ActiveRuntime") as string ?? "";
            }
            catch { return ""; }
        }

        public static bool IsOurs() => string.Equals(ActiveRuntime(), Paths.Manifest, StringComparison.OrdinalIgnoreCase);

        /// UAC prompt, then register_openxr_runtime.ps1. True if this folder is the active runtime afterwards.
        public static bool Register(IWin32Window owner)
        {
            try
            {
                var psi = new ProcessStartInfo("powershell.exe", $"-NoProfile -ExecutionPolicy Bypass -File \"{Paths.RegisterScript}\" -Quiet")
                { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
                using (var p = Process.Start(psi)) p.WaitForExit(60000);
            }
            catch (System.ComponentModel.Win32Exception) { return false; } // UAC declined
            var ok = IsOurs();
            Log.Write(ok ? "INFO" : "ERROR", ok ? "OpenXR runtime registered: " + Paths.Manifest : "OpenXR registration did not take effect");
            return ok;
        }
    }

    /// Window chrome shared by both tools: logo, version, the red alpha warning.
    public static class Ui
    {
        public static readonly Font Body = new Font("Segoe UI", 9.5f);
        public static readonly Font Bold = new Font("Segoe UI", 9.5f, FontStyle.Bold);
        public static readonly Font Big = new Font("Segoe UI", 11f, FontStyle.Bold);
        public static readonly Color Accent = Color.FromArgb(0x1E, 0x6B, 0xF0);

        public static Image Resource(string name)
        {
            using (var s = typeof(Ui).Assembly.GetManifestResourceStream(name)) return s == null ? null : Image.FromStream(s);
        }

        public static Icon AppIcon() { try { return Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { return null; } }

        public static Control Header(string subtitle)
        {
            var p = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, Padding = new Padding(12, 10, 12, 4), BackColor = Color.White };
            var logo = Resource("logo_640.png");
            if (logo != null) p.Controls.Add(new PictureBox { Image = logo, SizeMode = PictureBoxSizeMode.Zoom, Width = 360, Height = 172, Margin = new Padding(0) });
            p.Controls.Add(new Label { Text = $"{subtitle}   ·   version {Version.Text}", AutoSize = true, Font = Body, ForeColor = Color.DimGray });
            p.Controls.Add(new Label
            {
                Text = "ALPHA SOFTWARE - use at your own risk. For testing only.", AutoSize = true, Font = Bold, ForeColor = Color.White,
                BackColor = Color.FromArgb(0xC6, 0x28, 0x28), Padding = new Padding(6, 3, 6, 3), Margin = new Padding(0, 6, 0, 4),
            });
            return p;
        }

        /// Testing aid: `--screenshot <png>` renders the window into a file once it is shown, then closes it.
        public static string ScreenshotPath()
        {
            var a = Environment.GetCommandLineArgs();
            for (int i = 1; i + 1 < a.Length; i++) if (a[i] == "--screenshot") return a[i + 1];
            return null;
        }

        public static void ScreenshotWhenShown(Form f, int delayMs, Action before = null)
        {
            var path = ScreenshotPath();
            if (path == null) return;
            var t = new System.Windows.Forms.Timer { Interval = delayMs };
            t.Tick += (s, e) =>
            {
                t.Stop();
                before?.Invoke();
                using (var bmp = new Bitmap(f.Width, f.Height))
                {
                    f.DrawToBitmap(bmp, new Rectangle(0, 0, f.Width, f.Height));
                    bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
                }
                f.Close();
            };
            f.Shown += (s, e) => t.Start();
        }

        public static Label Line(string text, Font f = null) =>
            new Label { Text = text, AutoSize = true, Font = f ?? Body, Margin = new Padding(3, 3, 3, 3), MaximumSize = new Size(820, 0) }; // long lines wrap
    }
}
