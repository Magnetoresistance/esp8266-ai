using System.Net;
using System.Net.Http.Headers;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json;

namespace AIClockBridge;

// Talks to the ESP8266 clock's own HTTP API, so everything the device's web
// page can do (switch display, set bridge host, upload/reset pet GIFs) is
// available straight from the tray. Device address persists in Settings.

class DeviceInfo
{
    public string Ip = "";
    public string Ssid = "";
    public string Bridge = "";
    public string Mode = "auto";       // configured: auto | claude | codex | net | music
    public string Effective = "auto";  // what's actually on screen (AUTO may promote to music)
    public string Showing = "";
    public int LastUpdateS = -1;       // seconds since the device last got /status data, -1 = never
    public int SpriteRev;              // bumped by the device on animation change
    public int Brightness = 100;       // backlight 0-100 (0 = off)
    public int SpectrumType;           // spectrum type 0 bars / 1 wave / 2 radial
    public int SpectrumEffect;         // per-type effect index
    public int SpectrumColor;          // palette 0..7
    public int SpectrumColor2;         // secondary palette 0..7 (combo styles)
    public int SpectrumPeak = 1;       // peak dots 0/1
    public int SpectrumSmooth = 3;     // time smoothing 0..10
    public int SpectrumWidth = 3;      // bar width 1..5
    public int SpectrumRainbow;        // 0=solid 1=spectrum by value
    public int SpectrumGap = 1;        // bars: bar gap 0..2
    public int SpectrumDecay = 5;      // bars: peak-hold decay 1..10
    public int SpectrumLineW = 1;      // wave: line thickness 1..5
    public int SpectrumFill;           // wave: fill under line 0/1
    public int SpectrumFillColor;      // wave: fill palette 0..11 (incl. rainbow)
    public int SpectrumRingW = 2;      // radial: ring line thickness 1..8
    public int SpectrumRingGap = 2;    // radial: gap between spokes 0..10
    public int SpectrumRingInner = 12; // radial: inner circle radius 2..60
    public int SpectrumRingOuter = 58; // radial: outer circle radius 20..64
    public int SpectrumRingInColor = 1;// radial: inner ring color 0..8 (8=black)
    public int SpectrumRingFill = 1;   // radial: ring polyline fill 0/1
    public int SpectrumGradRange = 100;// 色谱行: gradient sweep height 0..100%
    public int SpectrumGradReverse;    // 色谱行: 0=正序 1=倒序
    public int SpectrumAutoRange;      // bars: 1=normalize to live min/max
    public int SpectrumOffset;         // bars: -100..100 height offset
    public int SpectrumSilence = 6;    // silence gate 0..50
    public int SpectrumMirror;         // 1=mirror bars/wave vertically
    public int SpectrumDualRing;       // 1=dual-ring for 环形/扇形 radial styles
    public int SpectrumDualInner = 100;// dual-ring inner sweep 0..100%
    public int SpectrumDualOuter = 100;// dual-ring outer reach 0..100%
    public bool ClaudeCustomSprite;
    public bool CodexCustomSprite;
    public int ClaudeW = 111, ClaudeH = 120;
    public int CodexW = 120, CodexH = 120;
}

class DeviceException : Exception
{
    public DeviceException(string message) : base(message) { }
}

static class DeviceClient
{
    const string HostKey = "device_host";
    const string LastSeenKey = "device_last_seen";

    // The ESP8266's lightweight HTTP server closes keep-alive connections
    // after a short idle period, but .NET's connection pool doesn't learn
    // the socket is dead until it tries to reuse it — so every ~10s a
    // pooled connection goes stale and the next request fails with a
    // "connection reset" that the preview reads as "无法连接设备".  Fix:
    // send Connection: close on every request so each gets a fresh TCP
    // connection.  The handshake to a LAN device is <1ms, negligible vs
    // the 1s poll interval, and the ESP8266's tiny PCB table is never
    // stranded by half-open sockets.
    static readonly HttpClient Http = CreateHttp();
    static HttpClient CreateHttp()
    {
        var h = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        h.DefaultRequestHeaders.ConnectionClose = true;
        return h;
    }

    public static string Host
    {
        get => Settings.Get(HostKey);
        set => Settings.Set(HostKey, value);
    }

    /// Last LAN address that polled our /status — i.e. the clock itself.
    public static string LastSeenIp
    {
        get => Settings.Get(LastSeenKey);
        set => Settings.Set(LastSeenKey, value);
    }

    public static Uri BaseUrl
    {
        get
        {
            var h = Host.Trim();
            if (h.Length == 0) return null;
            return Uri.TryCreate(h.StartsWith("http") ? h : $"http://{h}", UriKind.Absolute,
                out var uri) ? uri : null;
        }
    }

    static Uri Resolve(string path)
    {
        var b = BaseUrl ?? throw new DeviceException("未设置设备地址，请先在菜单里填写设备 IP");
        return new Uri(b, path);
    }

    /// GET /api/info
    public static async Task<DeviceInfo> FetchInfo()
    {
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        string body;
        try
        {
            body = await Http.GetStringAsync(Resolve("api/info"), cts.Token);
        }
        catch (DeviceException) { throw; }
        catch (Exception e)
        {
            throw new DeviceException($"无法连接设备：{e.Message}");
        }
        try
        {
            using var doc = JsonDocument.Parse(body);
            var root = doc.RootElement;
            var info = new DeviceInfo
            {
                Ip = Str(root, "ip"),
                Ssid = Str(root, "ssid"),
                Bridge = Str(root, "bridge"),
                Mode = Str(root, "mode", "auto"),
                LastUpdateS = Int(root, "last_update_s", -1),
                SpriteRev = Int(root, "sprite_rev"),
                Brightness = Int(root, "brightness", 100),
                SpectrumType = Int(root, "spectrum_type", 0),
                SpectrumEffect = Int(root, "spectrum_effect", 0),
                SpectrumColor = Int(root, "spectrum_color", 0),
                SpectrumColor2 = Int(root, "spectrum_color2", 1),
                SpectrumPeak = Int(root, "spectrum_peak", 1),
                SpectrumSmooth = Int(root, "spectrum_smooth", 3),
                SpectrumWidth = Int(root, "spectrum_width", 3),
                SpectrumRainbow = Int(root, "spectrum_rainbow", 0),
                SpectrumGap = Int(root, "spectrum_gap", 1),
                SpectrumDecay = Int(root, "spectrum_decay", 5),
                SpectrumLineW = Int(root, "spectrum_linew", 1),
                SpectrumFill = Int(root, "spectrum_fill", 0),
                SpectrumFillColor = Int(root, "spectrum_fillcolor", 0),
                SpectrumRingW = Int(root, "spectrum_ringw", 2),
                SpectrumRingGap = Int(root, "spectrum_ringgap", 2),
                SpectrumRingInner = Int(root, "spectrum_ringinner", 12),
                SpectrumRingOuter = Int(root, "spectrum_ringouter", 58),
                SpectrumRingInColor = Int(root, "spectrum_ringincolor", 1),
                SpectrumRingFill = Int(root, "spectrum_ringfill", 1),
                SpectrumGradRange = Int(root, "spectrum_gradrange", 100),
                SpectrumGradReverse = Int(root, "spectrum_gradreverse", 0),
                SpectrumAutoRange = Int(root, "spectrum_autorange", 0),
                SpectrumOffset = Int(root, "spectrum_offset", 0),
                SpectrumSilence = Int(root, "spectrum_silence", 6),
                SpectrumMirror = Int(root, "spectrum_mirror", 0),
                SpectrumDualRing = Int(root, "spectrum_dualring", 0),
                SpectrumDualInner = Int(root, "spectrum_dualin", 100),
                SpectrumDualOuter = Int(root, "spectrum_dualout", 100),
                Showing = Str(root, "showing"),
            };
            info.Effective = Str(root, "effective", info.Mode);
            if (root.TryGetProperty("claude", out var claude))
            {
                info.ClaudeCustomSprite = Bool(claude, "custom_sprite");
                info.ClaudeW = Int(claude, "w", 111);
                info.ClaudeH = Int(claude, "h", 120);
            }
            if (root.TryGetProperty("codex", out var codex))
            {
                info.CodexCustomSprite = Bool(codex, "custom_sprite");
                info.CodexW = Int(codex, "w", 120);
                info.CodexH = Int(codex, "h", 120);
            }
            return info;
        }
        catch (Exception)
        {
            throw new DeviceException("设备响应解析失败");
        }
    }

    /// POST /api/display  mode=auto|claude|codex|net|music
    public static Task SetDisplayMode(string mode) =>
        PostForm("api/display", new() { ["mode"] = mode });

    /// POST /api/bridge  host=ip:port
    public static Task SetBridgeHost(string bridgeHost) =>
        PostForm("api/bridge", new() { ["host"] = bridgeHost });

    /// POST /api/brightness  level=0-100 (0 = backlight off); device persists it
    public static Task SetBrightness(int level) =>
        PostForm("api/brightness", new() { ["level"] = level.ToString() });

    /// POST /api/music-spectrum type=0..2 — switch the device's spectrum
    /// type (0 bars / 1 wave / 2 radial); effect clamps to the type range.
    public static Task SetMusicSpectrumType(int type) =>
        PostForm("api/music-spectrum", new() { ["type"] = type.ToString() });

    /// POST /api/music-spectrum with a query string like
    /// "style=2&color=3&peak=1&smooth=5&width=3" — apply spectrum settings.
    public static Task SetMusicSpectrum(string query) =>
        PostForm("api/music-spectrum?" + query, new());

    /// POST /sprite/{claude|codex}  multipart GIF upload — the device decodes
    /// and rescales the GIF on-board, then swaps the animation immediately.
    public static async Task UploadGif(byte[] gif, string slot)
    {
        var url = Resolve($"sprite/{slot}");
        using var content = new MultipartFormDataContent($"aiclock-{Guid.NewGuid()}");
        var filePart = new ByteArrayContent(gif);
        filePart.Headers.ContentType = new MediaTypeHeaderValue("image/gif");
        content.Add(filePart, "file", "pet.gif");
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60)); // on-device decode
        HttpResponseMessage resp;
        try
        {
            resp = await Http.PostAsync(url, content, cts.Token);
        }
        catch (Exception e)
        {
            throw new DeviceException($"上传失败：{e.Message}");
        }
        using (resp) await ThrowUnlessOk(resp);
    }

    /// POST /sprite/{claude|codex}/reset — back to the compiled-in animation.
    public static Task ResetSprite(string slot) => PostForm($"sprite/{slot}/reset", new());

    /// GET /sprite/{claude|codex}/raw — the animation the device is actually
    /// using, wire format [1 byte frame count][RGB565 big-endian frames...].
    public static async Task<byte[]> FetchSpriteRaw(string slot)
    {
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        byte[] data;
        try
        {
            data = await Http.GetByteArrayAsync(Resolve($"sprite/{slot}/raw"), cts.Token);
        }
        catch (DeviceException) { throw; }
        catch (Exception e)
        {
            throw new DeviceException($"拉取动画失败：{e.Message}");
        }
        if (data.Length <= 1) throw new DeviceException("设备响应解析失败");
        return data;
    }

    // MARK: - internals

    static async Task PostForm(string path, Dictionary<string, string> fields)
    {
        var url = Resolve(path);
        using var content = new FormUrlEncodedContent(fields);
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(8));
        HttpResponseMessage resp;
        try
        {
            resp = await Http.PostAsync(url, content, cts.Token);
        }
        catch (Exception e)
        {
            throw new DeviceException($"请求失败：{e.Message}");
        }
        using (resp) await ThrowUnlessOk(resp);
    }

    static async Task ThrowUnlessOk(HttpResponseMessage resp)
    {
        if (resp.IsSuccessStatusCode) return;
        var msg = "";
        try { msg = await resp.Content.ReadAsStringAsync(); } catch { }
        throw new DeviceException($"设备返回 HTTP {(int)resp.StatusCode} {msg}");
    }

    static string Str(JsonElement o, string k, string dflt = "")
        => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() : dflt;

    static int Int(JsonElement o, string k, int dflt = 0)
        => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number
            ? (int)v.GetDouble() : dflt;

    static bool Bool(JsonElement o, string k)
        => o.TryGetProperty(k, out var v)
            && (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False)
            && v.GetBoolean();

    // MARK: - discovery / pairing

    /// Checks whether `ip` answers like our clock (GET /api/info with the
    /// expected JSON shape).
    public static async Task<bool> VerifyDevice(string ip, double timeoutSeconds = 2)
    {
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(timeoutSeconds));
            var body = await Http.GetStringAsync($"http://{ip}/api/info", cts.Token);
            using var doc = JsonDocument.Parse(body);
            return doc.RootElement.TryGetProperty("mode", out var mode)
                && mode.ValueKind == JsonValueKind.String
                && doc.RootElement.TryGetProperty("sprite_rev", out _);
        }
        catch
        {
            return false;
        }
    }

    /// Finds the clock and pairs (sets Host). Strategy:
    ///  1. the address that most recently polled our /status (no scanning);
    ///  2. the currently configured host, re-verified;
    ///  3. sweep this PC's /24 subnet for /api/info (covers a factory-fresh
    ///     device that has WiFi but no bridge configured yet).
    public static async Task<string> AutoPair(Action<string> progress)
    {
        var candidates = new List<string>();
        if (LastSeenIp.Length > 0) candidates.Add(LastSeenIp);
        var configured = Host.Split(':')[0];
        if (configured.Length > 0 && !candidates.Contains(configured)) candidates.Add(configured);

        foreach (var ip in candidates)
        {
            progress($"验证 {ip}…");
            if (await VerifyDevice(ip))
            {
                Host = ip;
                return ip;
            }
        }
        return await ScanSubnet(progress);
    }

    /// Parallel sweep of the local /24 (254 hosts, ~0.8s timeout each,
    /// 32-wide). Only used when the passive route came up empty.
    static async Task<string> ScanSubnet(Action<string> progress)
    {
        var myIp = LocalIPv4();
        var dot = myIp?.LastIndexOf('.') ?? -1;
        if (myIp == null || dot < 0) return null;
        var prefix = myIp[..dot];
        progress($"扫描 {prefix}.1-254…");

        using var sem = new SemaphoreSlim(32);
        string found = null;
        var tasks = new List<Task>();
        for (int n = 1; n <= 254; n++)
        {
            var ip = $"{prefix}.{n}";
            if (ip == myIp) continue;
            tasks.Add(Task.Run(async () =>
            {
                await sem.WaitAsync();
                try
                {
                    if (Volatile.Read(ref found) != null) return;
                    if (await VerifyDevice(ip, 0.8))
                        Interlocked.CompareExchange(ref found, ip, null);
                }
                finally
                {
                    sem.Release();
                }
            }));
        }
        await Task.WhenAll(tasks);
        if (found != null) Host = found;
        return found;
    }

    // MARK: - pairing watchdog
    /// Stamped on every device poll of our /status|/net|/music (see Program.cs).
    public static DateTime DevicePollAt = DateTime.MinValue;

    static bool _healInFlight;
    static DateTime _lastHealAttempt = DateTime.MinValue;

    /// Self-healing for the fresh-device chicken-and-egg: after a full flash
    /// erase the clock knows no bridge host, so it never polls us and passive
    /// discovery never fires. When we haven't heard from the device for a few
    /// minutes, actively find it (last-seen IP, configured host, then /24
    /// scan) and, if its bridge is unset or it can't reach the one it has,
    /// point it at this PC. Called from a 60s timer; the /24 scan is
    /// rate-limited to once per 5 minutes.
    public static async Task HealPairingIfNeeded(int port)
    {
        if (DateTime.UtcNow - DevicePollAt < TimeSpan.FromMinutes(3)) return; // device is polling us
        if (_healInFlight || DateTime.UtcNow - _lastHealAttempt < TimeSpan.FromMinutes(5)) return;
        _healInFlight = true;
        _lastHealAttempt = DateTime.UtcNow;
        try
        {
            if (await AutoPair(_ => { }) == null) return;
            var info = await FetchInfo();
            var myIp = LocalIPv4();
            if (myIp == null) return;
            var stale = info.LastUpdateS < 0 || info.LastUpdateS > 60;
            if (info.Bridge.Length == 0 || stale)
            {
                await SetBridgeHost($"{myIp}:{port}");
                Console.Error.WriteLine($"[pair] pushed bridge {myIp}:{port} to {info.Ip}");
            }
        }
        catch (DeviceException)
        {
            // device vanished mid-heal; next tick retries
        }
        finally
        {
            _healInFlight = false;
        }
    }

    /// LAN IPv4 of this PC — used for one-click "point the device's bridge at
    /// this machine". Prefers an interface that has a default gateway.
    public static string LocalIPv4()
    {
        string best = null;
        try
        {
            foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (nic.OperationalStatus != OperationalStatus.Up) continue;
                if (nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                var props = nic.GetIPProperties();
                var hasGateway = props.GatewayAddresses
                    .Any(g => g.Address?.AddressFamily == AddressFamily.InterNetwork);
                foreach (var addr in props.UnicastAddresses)
                {
                    if (addr.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    if (IPAddress.IsLoopback(addr.Address)) continue;
                    var ip = addr.Address.ToString();
                    if (hasGateway) return ip;
                    best ??= ip;
                }
            }
        }
        catch
        {
            // fall through
        }
        return best;
    }
}
