using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace AIClockBridge;

// Serves the desktop-mirror endpoint the ESP8266 polls for MODE_MIRROR.
// The capture source is configurable: the whole primary screen, a specific
// top-level window (PrintWindow so it works even when occluded), or a
// custom screen region. The chosen area is downscaled to the panel size and
// encoded as big-endian RGB565 (Rgb565.Encode) — the exact wire format the
// firmware streams row-by-row into pushImage(). Capture is throttled; the
// latest frame is cached so the device's /mirror/raw GET is cheap.
sealed class ScreenMirrorService
{
    public enum CaptureTarget { FullScreen, Window, Region }

    /// How the captured frame is placed on the 128x128 panel.
    public enum FitMode
    {
        Fill,   // stretch to fill the whole panel (default)
        Fit,    // keep aspect ratio, letterboxed on black
    }

    /// Where the fitted frame sits inside the panel when FitMode == Fit.
    public enum Align { Center, Top, Bottom, Left, Right }

    const int PanelW = 128;   // must match firmware SCREEN_W
    const int PanelH = 128;   // must match firmware SCREEN_H
    const long CaptureMs = 400; // ~2.5 fps capture ceiling, plenty for a 128px panel

    long _lastCaptureAt;
    byte[] _cachedFrame = Array.Empty<byte>();

    CaptureTarget _target = CaptureTarget.FullScreen;
    IntPtr _windowHwnd = IntPtr.Zero;
    Rectangle _region = new(0, 0, 128, 128);

    /// Changing the capture source invalidates the cached frame so the next
    /// /mirror/raw request captures the new source immediately (the 400ms
    /// throttle must not keep serving the previous full-screen frame).
    public CaptureTarget Target
    {
        get => _target;
        set { _target = value; InvalidateCache(); }
    }
    public IntPtr WindowHwnd
    {
        get => _windowHwnd;
        set { _windowHwnd = value; InvalidateCache(); }
    }
    public Rectangle Region
    {
        get => _region;
        set { _region = value; InvalidateCache(); }
    }
    /// Frame placement on the panel.
    public FitMode Fit { get; set; } = FitMode.Fill;
    public Align Alignment { get; set; } = Align.Center;

    void InvalidateCache()
    {
        _cachedFrame = Array.Empty<byte>();
        _lastCaptureAt = 0;
    }

    // ---- persistence (mirror_config.json next to the exe) ----

    static string ConfigPath =>
        Path.Combine(AppContext.BaseDirectory, "mirror_config.json");

    public ScreenMirrorService()
    {
        Load(); // restore persisted capture config or keep defaults
    }

    /// Persist the current capture config to mirror_config.json.
    public void Save()
    {
        try
        {
            File.WriteAllBytes(ConfigPath, ConfigJson());
        }
        catch
        {
            // disk read-only / locked: skip silently
        }
    }

    /// Restore config from mirror_config.json if it exists. Deleting the
    /// file resets to defaults (full screen).
    void Load()
    {
        try
        {
            if (File.Exists(ConfigPath))
            {
                using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(ConfigPath));
                ApplyConfig(doc.RootElement);
                return;
            }
        }
        catch
        {
            // corrupt config: keep defaults
        }
    }

    /// Latest capture as big-endian RGB565, throttled + cached.
    public byte[] FrameRgb565
    {
        get
        {
            long now = Environment.TickCount64;
            if (now - _lastCaptureAt < CaptureMs && _cachedFrame.Length > 0)
                return _cachedFrame;
            _lastCaptureAt = now;

            try
            {
                using var src = CaptureSource();
                if (src == null) return _cachedFrame;

                using var scaled = new Bitmap(PanelW, PanelH, PixelFormat.Format32bppArgb);
                using (var g = Graphics.FromImage(scaled))
                {
                    g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                    if (Fit == FitMode.Fill)
                    {
                        // stretch to fill the whole panel
                        g.DrawImage(src, 0, 0, PanelW, PanelH);
                    }
                    else
                    {
                        // Fit: keep aspect ratio, place on black with alignment.
                        // (panel background is already black from new Bitmap)
                        double scale = Math.Min((double)PanelW / src.Width, (double)PanelH / src.Height);
                        int w = Math.Max(1, (int)Math.Round(src.Width * scale));
                        int h = Math.Max(1, (int)Math.Round(src.Height * scale));
                        int x = Alignment switch
                        {
                            Align.Center => (PanelW - w) / 2,
                            Align.Left => 0,
                            Align.Right => PanelW - w,
                            _ => (PanelW - w) / 2, // Top/Bottom: center horizontally
                        };
                        int y = Alignment switch
                        {
                            Align.Center => (PanelH - h) / 2,
                            Align.Top => 0,
                            Align.Bottom => PanelH - h,
                            _ => (PanelH - h) / 2, // Left/Right: center vertically
                        };
                        g.DrawImage(src, x, y, w, h);
                    }
                }
                _cachedFrame = Rgb565.Encode(scaled);
            }
            catch
            {
                // capture glitch (screen locked, window closed, etc.)
            }
            return _cachedFrame;
        }
    }

    Bitmap CaptureSource()
    {
        switch (Target)
        {
            case CaptureTarget.Window when WindowHwnd != IntPtr.Zero:
                return CaptureWindow(WindowHwnd);
            case CaptureTarget.Region when Region.Width > 0 && Region.Height > 0:
                return CaptureScreenRegion(Region);
            default:
                return CapturePrimaryScreen();
        }
    }

    static Bitmap CapturePrimaryScreen()
    {
        var bounds = System.Windows.Forms.Screen.PrimaryScreen?.Bounds;
        if (bounds == null) return null;
        var bmp = new Bitmap(bounds.Value.Width, bounds.Value.Height, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);
        g.CopyFromScreen(bounds.Value.X, bounds.Value.Y, 0, 0, bounds.Value.Size);
        return bmp;
    }

    static Bitmap CaptureScreenRegion(Rectangle r)
    {
        var bmp = new Bitmap(r.Width, r.Height, PixelFormat.Format32bppArgb);
        using var g = Graphics.FromImage(bmp);
        g.CopyFromScreen(r.X, r.Y, 0, 0, new Size(r.Width, r.Height));
        return bmp;
    }

    /// PrintWindow captures the window's own content even when it is covered
    /// by other windows; PW_RENDERFULLCONTENT asks DWM for the full frame.
    /// GPU-accelerated windows (Chrome/Edge/video/games) often return a fully
    /// black frame from PrintWindow, so if the result is (almost) all black
    /// we fall back to copying the window's on-screen rectangle instead.
    static Bitmap CaptureWindow(IntPtr hwnd)
    {
        if (!GetWindowRect(hwnd, out var rc)) return null;
        int w = rc.Right - rc.Left, h = rc.Bottom - rc.Top;
        if (w <= 0 || h <= 0) return null;

        var printBmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(printBmp))
        {
            var hdc = g.GetHdc();
            try
            {
                PrintWindow(hwnd, hdc, 2 /*PW_RENDERFULLCONTENT*/);
            }
            finally
            {
                g.ReleaseHdc(hdc);
            }
        }
        if (!IsMostlyBlack(printBmp)) return printBmp;
        printBmp.Dispose();

        // PrintWindow gave us nothing (GPU-composited window): grab what's on
        // screen at the window's position. The window is usually visible when
        // the user just picked it, so this shows the expected content.
        return CaptureScreenRegion(new Rectangle(rc.Left, rc.Top, w, h));
    }

    /// True when a sampled grid of pixels is (nearly) all black — the
    /// signature of a failed PrintWindow on a hardware-accelerated window.
    static bool IsMostlyBlack(Bitmap bmp)
    {
        int step = Math.Max(1, Math.Min(bmp.Width, bmp.Height) / 16);
        int black = 0, total = 0;
        for (int y = 0; y < bmp.Height; y += step)
        {
            for (int x = 0; x < bmp.Width; x += step)
            {
                total++;
                var c = bmp.GetPixel(x, y);
                if (c.R < 8 && c.G < 8 && c.B < 8) black++;
            }
        }
        return total > 0 && black * 100 / total > 95;
    }

    // ---- Win32 ----
    [DllImport("user32.dll")]
    static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")]
    static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")]
    static extern int GetWindowTextLength(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll")]
    static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    struct RECT { public int Left, Top, Right, Bottom; }

    /// Visible top-level windows with a non-empty title, for the mirror
    /// settings dialog ("mirror this window"). Sorted by title.
    public static List<(IntPtr Hwnd, string Title)> EnumerateWindows()
    {
        var windows = new List<(IntPtr Hwnd, string Title)>();
        EnumWindows((h, _) =>
        {
            if (IsWindowVisible(h) && GetWindowTextLength(h) > 0)
            {
                var sb = new System.Text.StringBuilder(256);
                GetWindowText(h, sb, sb.Capacity);
                var title = sb.ToString();
                if (!string.IsNullOrWhiteSpace(title))
                    windows.Add((Hwnd: h, Title: title));
            }
            return true; // keep enumerating
        }, IntPtr.Zero);
        return windows.OrderBy(w => w.Title).ToList();
    }

    /// Current capture config as JSON (GET /mirror/config).
    public byte[] ConfigJson()
    {
        string target = Target switch
        {
            CaptureTarget.Window => "window",
            CaptureTarget.Region => "region",
            _ => "fullscreen",
        };
        var json = System.Text.Json.JsonSerializer.Serialize(new
        {
            target,
            window_hwnd = WindowHwnd.ToInt64(),
            window_title = WindowHwnd != IntPtr.Zero ? WindowTitle(WindowHwnd) : "",
            region = new { x = Region.X, y = Region.Y, w = Region.Width, h = Region.Height },
            fit = Fit.ToString().ToLowerInvariant(),
            align = Alignment.ToString().ToLowerInvariant(),
        });
        return System.Text.Encoding.UTF8.GetBytes(json);
    }

    /// Apply capture config from a JSON object (POST /mirror/config).
    /// Accepts {"target":"fullscreen"} | {"target":"window","window_hwnd":N}
    /// | {"target":"region","region":{"x":..,"y":..,"w":..,"h":..}}.
    public void ApplyConfig(System.Text.Json.JsonElement root)
    {
        if (!root.TryGetProperty("target", out var t) || t.ValueKind != System.Text.Json.JsonValueKind.String)
            return;
        switch (t.GetString())
        {
            case "fullscreen":
                Target = CaptureTarget.FullScreen;
                break;
            case "window":
                if (root.TryGetProperty("window_hwnd", out var h) && h.ValueKind == System.Text.Json.JsonValueKind.Number)
                    WindowHwnd = new IntPtr(h.GetInt64());
                if (WindowHwnd != IntPtr.Zero) Target = CaptureTarget.Window;
                break;
            case "region":
                if (root.TryGetProperty("region", out var r) && r.ValueKind == System.Text.Json.JsonValueKind.Object)
                {
                    int X = 0, Y = 0, W = 128, H = 128;
                    if (r.TryGetProperty("x", out var px) && px.ValueKind == System.Text.Json.JsonValueKind.Number) X = px.GetInt32();
                    if (r.TryGetProperty("y", out var py) && py.ValueKind == System.Text.Json.JsonValueKind.Number) Y = py.GetInt32();
                    if (r.TryGetProperty("w", out var pw) && pw.ValueKind == System.Text.Json.JsonValueKind.Number) W = Math.Max(1, pw.GetInt32());
                    if (r.TryGetProperty("h", out var ph) && ph.ValueKind == System.Text.Json.JsonValueKind.Number) H = Math.Max(1, ph.GetInt32());
                    Region = new Rectangle(X, Y, W, H);
                    Target = CaptureTarget.Region;
                }
                break;
        }
        if (root.TryGetProperty("fit", out var f) && f.ValueKind == System.Text.Json.JsonValueKind.String)
            Fit = f.GetString() == "fit" ? FitMode.Fit : FitMode.Fill;
        if (root.TryGetProperty("align", out var a) && a.ValueKind == System.Text.Json.JsonValueKind.String)
        {
            Alignment = a.GetString() switch
            {
                "top" => Align.Top,
                "bottom" => Align.Bottom,
                "left" => Align.Left,
                "right" => Align.Right,
                _ => Align.Center,
            };
        }
        Save(); // persist every config change (API or dialog)
    }

    static string WindowTitle(IntPtr hwnd)
    {
        var sb = new System.Text.StringBuilder(256);
        GetWindowText(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    /// Public title lookup (used by the window picker for frameless windows).
    public static string WindowTitleOf(IntPtr hwnd) => WindowTitle(hwnd);
}
