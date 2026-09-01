using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Text.Json;

namespace AIClockBridge;

// Serves the photo-album endpoints the ESP8266 polls for MODE_ALBUM.
// The photo source directory, accepted file formats and play order are
// user-configurable (AlbumSettingsDialog). Auto-play is driven by an
// internal timer: it advances the slide and re-encodes the cached frame on
// its own clock, so /album/raw (device poll) and the mirror popup preview
// both simply *read* the same cached frame  they can never drift apart.
// Manual stepping (next/previous/random) forces a frame update instead.
sealed class AlbumService
{
    const int PanelW = 128;   // must match firmware SCREEN_W
    const int PanelH = 128;   // must match firmware SCREEN_H

    public enum PlayOrder { Sequential, Shuffle }

    /// How the sequential list is ordered before playback (used when
    /// Order == Sequential; ignored when shuffling).
    public enum SortBy { Name, Date, Type, Size }

    /// How each photo is placed on the 128x128 panel.
    public enum FitMode
    {
        Fill,   // stretch to fill the whole panel (default, current behavior)
        Fit,    // keep aspect ratio, letterboxed on black
    }

    /// Where the fitted photo sits inside the panel when FitMode == Fit.
    public enum Align { Center, Top, Bottom, Left, Right }

    public string DirectoryPath { get; set; } = Path.Combine(AppContext.BaseDirectory, "album");
    public bool IncludeJpg { get; set; } = true;
    public bool IncludePng { get; set; } = true;
    public bool IncludeBmp { get; set; } = true;
    public PlayOrder Order { get; set; } = PlayOrder.Sequential;
    /// Sequential ordering key and direction.
    public SortBy SortKey { get; set; } = SortBy.Name;
    public bool SortAscending { get; set; } = true;

    int _intervalMs = 4000;
    /// Auto-play slide interval; a background timer advances the slide on
    /// this cadence and both the device poll and the preview read the same
    /// cached frame, so they stay in sync. Min 1s.
    public int IntervalMs
    {
        get => _intervalMs;
        set { _intervalMs = Math.Max(1000, value); RestartTimer(); }
    }

    bool _manualMode;
    /// Manual mode: the auto-advance timer is suspended; only
    /// PrevRgb565/NextManualRgb565/RandomRgb565 change the photo.
    /// On automanual transition, _index is rewound by one so it points
    /// to the currently-displayed photo (in auto mode AutoAdvance encodes
    /// then advances, so _index is always one ahead of what's on screen).
    public bool ManualMode
    {
        get => _manualMode;
        set
        {
            if (!_manualMode && value && _photos.Count > 0)
                _index = (_index - 1 + _photos.Count) % _photos.Count;
            _manualMode = value;
            RestartTimer();
        }
    }

    /// Photo placement on the panel.
    public FitMode Fit { get; set; } = FitMode.Fill;
    public Align Alignment { get; set; } = Align.Center;

    readonly object _lock = new();
    readonly List<string> _photos = new();
    int _index;
    readonly Random _rng = new();
    byte[] _cachedFrame = Array.Empty<byte>();
    System.Threading.Timer _timer;

    public AlbumService()
    {
        Load(); // restore persisted config (album_config.json) or defaults
    }

    // ---- persistence ----

    static string ConfigPath =>
        Path.Combine(AppContext.BaseDirectory, "album_config.json");

    /// Persist the current config to album_config.json next to the exe.
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

    /// Restore config from album_config.json if it exists; otherwise keep
    /// defaults and scan the default directory. Deleting the file resets to
    /// defaults (like a fresh install).
    void Load()
    {
        try
        {
            if (File.Exists(ConfigPath))
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(ConfigPath));
                ApplyConfig(doc.RootElement); // includes Refresh()
                return;
            }
        }
        catch
        {
            // corrupt config: fall through to defaults
        }
        Refresh();
    }

    /// The set of extensions currently enabled by the format toggles.
    IEnumerable<string> ActiveExtensions()
    {
        if (IncludeJpg) { yield return "*.jpg"; yield return "*.jpeg"; }
        if (IncludePng) yield return "*.png";
        if (IncludeBmp) yield return "*.bmp";
    }

    /// Rescan the configured directory with the current format filter.
    public void Refresh()
    {
        lock (_lock)
        {
            _photos.Clear();
            try
            {
                if (Directory.Exists(DirectoryPath))
                {
                    foreach (var ext in ActiveExtensions())
                        _photos.AddRange(Directory.GetFiles(DirectoryPath, ext));
                }
            }
            catch
            {
                // folder locked / unreadable: serve empty album
            }
            // deterministic order by the configured key, then shuffle if configured
            SortPhotoList();
            if (Order == PlayOrder.Shuffle) Shuffle();
            _index = 0;
            _cachedFrame = _photos.Count > 0 ? EncodePhoto(_photos[0]) : Array.Empty<byte>();
        }
        RestartTimer();
        Console.Error.WriteLine($"[album] {_photos.Count} photos in {DirectoryPath} ({Order}, {IntervalMs}ms)");
    }

    /// Order the photo list by the configured key and direction.
    void SortPhotoList()
    {
        var desc = !SortAscending;
        switch (SortKey)
        {
            case SortBy.Date:
                _photos.Sort((a, b) =>
                {
                    int c = File.GetLastWriteTime(a).CompareTo(File.GetLastWriteTime(b));
                    return desc ? -c : c;
                });
                break;
            case SortBy.Type:
                _photos.Sort((a, b) =>
                {
                    int c = StringComparer.OrdinalIgnoreCase.Compare(
                        Path.GetExtension(a), Path.GetExtension(b));
                    if (c == 0) c = StringComparer.OrdinalIgnoreCase.Compare(a, b);
                    return desc ? -c : c;
                });
                break;
            case SortBy.Size:
                _photos.Sort((a, b) =>
                {
                    long ca = SafeSize(a), cb = SafeSize(b);
                    int c = ca.CompareTo(cb);
                    return desc ? -c : c;
                });
                break;
            default: // Name
                _photos.Sort((a, b) =>
                {
                    int c = StringComparer.OrdinalIgnoreCase.Compare(
                        Path.GetFileName(a), Path.GetFileName(b));
                    return desc ? -c : c;
                });
                break;
        }
    }

    static long SafeSize(string path)
    {
        try { return new FileInfo(path).Length; }
        catch { return 0; }
    }

    void Shuffle()
    {
        for (int i = _photos.Count - 1; i > 0; i--)
        {
            int j = _rng.Next(i + 1);
            (_photos[i], _photos[j]) = (_photos[j], _photos[i]);
        }
    }

    /// (Re)start the auto-advance timer. Suspended while in manual mode or
    /// when the album is empty.
    void RestartTimer()
    {
        lock (_lock)
        {
            _timer?.Dispose();
            _timer = null;
            if (ManualMode || _photos.Count == 0) return;
            _timer = new System.Threading.Timer(_ => AutoAdvance(), null, IntervalMs, IntervalMs);
        }
    }

    /// Timer tick: advance to the next photo and re-encode the cached frame
    /// so every reader (device poll + preview) sees the same updated frame.
    void AutoAdvance()
    {
        lock (_lock)
        {
            if (ManualMode || _photos.Count == 0) return;
            _cachedFrame = EncodePhoto(_photos[_index]);
            Advance();
        }
    }

    /// Current config as JSON (GET /album/config).
    public byte[] ConfigJson()
    {
        var json = JsonSerializer.Serialize(new
        {
            directory = DirectoryPath,
            jpg = IncludeJpg,
            png = IncludePng,
            bmp = IncludeBmp,
            interval_ms = IntervalMs,
            order = Order.ToString().ToLowerInvariant(),
            sort_key = SortKey.ToString().ToLowerInvariant(),
            sort_asc = SortAscending,
            manual = ManualMode,
            fit = Fit.ToString().ToLowerInvariant(),
            align = Alignment.ToString().ToLowerInvariant(),
        });
        return System.Text.Encoding.UTF8.GetBytes(json);
    }

    /// Apply config from a JSON object (POST /album/config), then rescan.
    public void ApplyConfig(System.Text.Json.JsonElement root)
    {
        if (root.TryGetProperty("directory", out var d) && d.ValueKind == JsonValueKind.String)
            DirectoryPath = d.GetString();
        if (root.TryGetProperty("jpg", out var j) && j.ValueKind is JsonValueKind.True or JsonValueKind.False)
            IncludeJpg = j.GetBoolean();
        if (root.TryGetProperty("png", out var p) && p.ValueKind is JsonValueKind.True or JsonValueKind.False)
            IncludePng = p.GetBoolean();
        if (root.TryGetProperty("bmp", out var b) && b.ValueKind is JsonValueKind.True or JsonValueKind.False)
            IncludeBmp = b.GetBoolean();
        if (root.TryGetProperty("interval_ms", out var i) && i.ValueKind == JsonValueKind.Number)
            IntervalMs = Math.Clamp(i.GetInt32(), 1000, 600000);
        if (root.TryGetProperty("order", out var o) && o.ValueKind == JsonValueKind.String)
            Order = o.GetString() == "shuffle" ? PlayOrder.Shuffle : PlayOrder.Sequential;
        if (root.TryGetProperty("sort_key", out var sk) && sk.ValueKind == JsonValueKind.String)
        {
            SortKey = sk.GetString() switch
            {
                "date" => SortBy.Date,
                "type" => SortBy.Type,
                "size" => SortBy.Size,
                _ => SortBy.Name,
            };
        }
        if (root.TryGetProperty("sort_asc", out var sa) && sa.ValueKind is JsonValueKind.True or JsonValueKind.False)
            SortAscending = sa.GetBoolean();
        if (root.TryGetProperty("manual", out var m) && m.ValueKind is JsonValueKind.True or JsonValueKind.False)
            ManualMode = m.GetBoolean();
        if (root.TryGetProperty("fit", out var f) && f.ValueKind == JsonValueKind.String)
            Fit = f.GetString() == "fit" ? FitMode.Fit : FitMode.Fill;
        if (root.TryGetProperty("align", out var a) && a.ValueKind == JsonValueKind.String)
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
        Refresh();
        Save(); // persist every config change (API or dialog)
    }

    /// JSON {"count":N, ...} for GET /album/list.
    public byte[] ListJson()
    {
        var json = JsonSerializer.Serialize(new
        {
            count = _photos.Count,
            directory = DirectoryPath,
            order = Order.ToString().ToLowerInvariant(),
            interval_ms = IntervalMs,
            manual = ManualMode,
        });
        return System.Text.Encoding.UTF8.GetBytes(json);
    }

    /// Current photo as big-endian RGB565  a pure read of the frame the
    /// background timer (or a manual step) last produced. Both the device's
    /// /album/raw poll and the mirror popup preview call this and get the
    /// exact same bytes, so the two can never drift out of sync.
    public byte[] NextRgb565()
    {
        lock (_lock)
        {
            return _cachedFrame.Length > 0 ? _cachedFrame : Array.Empty<byte>();
        }
    }

    /// Manual stepping: force-show the previous photo (stays current until
    /// the next manual step, since manual mode suspends the timer).
    public byte[] PrevRgb565()
    {
        lock (_lock)
        {
            if (_photos.Count == 0) return Array.Empty<byte>();
            _index = (_index - 1 + _photos.Count) % _photos.Count;
            _cachedFrame = EncodePhoto(_photos[_index]);
            return _cachedFrame;
        }
    }

    public byte[] NextManualRgb565()
    {
        lock (_lock)
        {
            if (_photos.Count == 0) return Array.Empty<byte>();
            Advance();
            _cachedFrame = EncodePhoto(_photos[_index]);
            return _cachedFrame;
        }
    }

    public byte[] RandomRgb565()
    {
        lock (_lock)
        {
            if (_photos.Count == 0) return Array.Empty<byte>();
            int idx = _rng.Next(_photos.Count);
            _index = idx;
            _cachedFrame = EncodePhoto(_photos[idx]);
            return _cachedFrame;
        }
    }

    void Advance()
    {
        if (_photos.Count == 0) return;
        _index = (_index + 1) % _photos.Count;
        if (_index == 0 && Order == PlayOrder.Shuffle) Shuffle(); // new lap
    }

    byte[] EncodePhoto(string path)
    {
        try
        {
            using var full = new Bitmap(path);
            using var scaled = new Bitmap(PanelW, PanelH, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(scaled))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                if (Fit == FitMode.Fill)
                {
                    // stretch to fill the whole panel
                    g.DrawImage(full, 0, 0, PanelW, PanelH);
                }
                else
                {
                    // Fit: keep aspect ratio, place on black with alignment.
                    // (panel background is already black from new Bitmap)
                    double scale = Math.Min((double)PanelW / full.Width, (double)PanelH / full.Height);
                    int w = Math.Max(1, (int)Math.Round(full.Width * scale));
                    int h = Math.Max(1, (int)Math.Round(full.Height * scale));
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
                    g.DrawImage(full, x, y, w, h);
                }
            }
            return Rgb565.Encode(scaled);
        }
        catch
        {
            return Array.Empty<byte>(); // corrupt image: skip it this round
        }
    }
}
