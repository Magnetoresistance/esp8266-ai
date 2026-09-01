using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;

namespace AIClockBridge;

// Live "mirror" of the ESP8266 screen, shown as a popup near the tray icon.
// Not a video stream: the PC re-renders the same scene from the same data —
// /api/info says which app the device is showing (and a sprite_rev that bumps
// when animations change), /sprite/<app>/raw provides the exact frames the
// device draws (custom upload or built-in), and the local StatusService
// supplies the quota numbers the device gets from /status. Result: what you
// see here is what the panel shows, including the walk cycle animating only
// while that app is "working".

// MARK: - the 240x240 replica control

sealed class MirrorControl : Control
{
    // scene state, all in the device's 240x240 logical coordinates
    public List<Bitmap> Frames = new();
    public int FrameIdx;
    public int SpriteW = 120, SpriteH = 120;
    public double RingPct;
    public bool NeedsInput; // shown app waiting on approval -> red border flash
    public bool FlashOn;
    public string Line1 = "5h -";
    public string Line2 = "Weekly -";
    public bool ShowingClaude = true;
    public bool DeviceOK;
    // net-mode mirror: same scrolling area-chart model as the firmware —
    // one column per 250ms sample, 224-column (56s) window, shared "nice"
    // full-scale, dim-green download area + yellow upload line.
    public bool NetMode;
    public int NetCPU = -1; // -1 = hidden (CPU/MEM row disabled in the menu)
    public int NetMem = -1;
    public string NetHeaderDL = "0B";
    public string NetHeaderUL = "0B";
    const int NetCols = 224; // NET_CHART_W
    double[] _histRx = new double[NetCols];
    double[] _histTx = new double[NetCols];

    public bool StockMode;
    public StockMonitor.Row[] StockRows = Array.Empty<StockMonitor.Row>();

    public bool MusicMode;
    public string MusicTitle = "";
    public string MusicArtist = "";
    public double MusicElapsed;
    public double MusicDuration;
    public bool MusicPlaying;
    public Bitmap MusicCover;

    // desktop-mirror / photo-album scene: a full-panel frame from the bridge
    // (ScreenMirrorService / AlbumService), shown scaled onto the 240x240
    // replica. Set to null to blank the scene.
    public bool MirrorMode;
    public bool AlbumMode;
    public Bitmap SceneFrame;

    // music-spectrum scene: live 24-bar magnitudes (0-255) from the bridge's
    // SpectrumService, drawn in the current device style so the preview
    // matches the panel.
    public bool SpectrumMode;
    public byte[] SpectrumBars = new byte[24];
    public int SpectrumType;
    public int SpectrumEffect;
    public int SpectrumColor;
    public int SpectrumColor2 = 1;
    public int SpectrumRainbow;
    public int SpectrumGap = 1;
    public int SpectrumDecay = 5;
    public int SpectrumLineW = 1;
    public int SpectrumFill;
    public int SpectrumFillColor;
    public int SpectrumRingW = 2;
    public int SpectrumRingGap = 2;
    public int SpectrumRingInner = 12;
    public int SpectrumRingOuter = 58;
    public int SpectrumRingInColor = 1;
    public int SpectrumRingFill = 1;
    public int SpectrumGradRange = 100;
    public int SpectrumGradReverse;
    public int SpectrumAutoRange;
    public int SpectrumOffset;
    public int SpectrumSilence = 6;
    public int SpectrumMirror;
    public int SpectrumDualRing;
    public int SpectrumDualInner = 100;
    public int SpectrumDualOuter = 100;
    float _specMin;                 // adaptive rainbow range (preview side)
    float _specMax = 255;

    static readonly Image ClaudeLogo = LoadAsset("claude-logo.png");
    static readonly Image CodexLogo = LoadAsset("codex-logo.png");

    static readonly Color Green = Color.FromArgb(0, 217, 51);
    static readonly Color Yellow = Color.FromArgb(255, 204, 0);

    public MirrorControl()
    {
        DoubleBuffered = true;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint
            | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }

    internal static Image LoadAsset(string name)
    {
        var asm = typeof(MirrorControl).Assembly;
        var resource = asm.GetManifestResourceNames()
            .FirstOrDefault(n => n.EndsWith(name, StringComparison.OrdinalIgnoreCase));
        if (resource == null) return new Bitmap(1, 1);
        using var stream = asm.GetManifestResourceStream(resource);
        return Image.FromStream(stream);
    }

    public void ResetNetSweep()
    {
        _histRx = new double[NetCols];
        _histTx = new double[NetCols];
    }

    public void PushNetSample(double rx, double tx)
    {
        Array.Copy(_histRx, 1, _histRx, 0, NetCols - 1);
        _histRx[NetCols - 1] = rx;
        Array.Copy(_histTx, 1, _histTx, 0, NetCols - 1);
        _histTx[NetCols - 1] = tx;
        Invalidate();
    }

    /// Firmware's adaptiveNetScale: the window peak sits at ~87% of the chart.
    static double AdaptiveNetScale(double maxV) => Math.Max(maxV * 1.15, 10240);

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        var scale = Width / 240f;
        g.ScaleTransform(scale, scale);

        // panel background
        using (var panel = RoundedRect(new RectangleF(0, 0, 240, 240), 10))
        {
            g.FillPath(Brushes.Black, panel);
            g.SetClip(panel);
        }

        if (NetMode)
        {
            DrawNetScene(g);
            return;
        }
        if (MusicMode)
        {
            DrawMusicScene(g);
            return;
        }
        if (MirrorMode || AlbumMode)
        {
            DrawSceneFrame(g);
            return;
        }
        if (SpectrumMode)
        {
            DrawSpectrumScene(g);
            return;
        }
        if (StockMode)
        {
            DrawStockScene(g);
            return;
        }

        // square quota ring: margin 4, thickness 10, clockwise from top-left
        const float m = 4, t = 10;
        const float side = 240 - 2 * m;
        using (var ring = new SolidBrush(DeviceOK ? Green : Color.FromArgb(90, 90, 90)))
        {
            var remaining = side * 4 * (float)(Math.Clamp(RingPct, 0, 100) / 100);
            const float x0 = m, y0 = m, x1 = 240 - m;
            var seg = Math.Min(remaining, side);
            if (seg > 0) g.FillRectangle(ring, x0, y0, seg, t);                    // top
            remaining -= side;
            seg = Math.Min(remaining, side);
            if (seg > 0) g.FillRectangle(ring, x1 - t, y0, t, seg);                // right
            remaining -= side;
            seg = Math.Min(remaining, side);
            if (seg > 0) g.FillRectangle(ring, x1 - seg, 240 - m - t, seg, t);     // bottom
            remaining -= side;
            seg = Math.Min(remaining, side);
            if (seg > 0) g.FillRectangle(ring, x0, 240 - m - seg, t, seg);         // left
        }

        // sprite, centered, pixel-crisp
        if (Frames.Count > 0)
        {
            var img = Frames[Math.Min(FrameIdx, Frames.Count - 1)];
            var state = g.Save();
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.DrawImage(img, new Rectangle(120 - SpriteW / 2, 120 - SpriteH / 2, SpriteW, SpriteH));
            g.Restore(state);
        }

        // app logo, top-left inside the ring (firmware draws it at 14,18 @40px)
        g.DrawImage(ShowingClaude ? ClaudeLogo : CodexLogo, new Rectangle(14, 18, 40, 40));

        // quota text
        using (var font = new Font("Consolas", 13, FontStyle.Bold, GraphicsUnit.Pixel))
        using (var fmt = new StringFormat { Alignment = StringAlignment.Center })
        {
            g.DrawString(Line1, font, Brushes.White, new RectangleF(0, 188, 240, 18), fmt);
            g.DrawString(Line2, font, Brushes.White, new RectangleF(0, 206, 240, 18), fmt);
        }

        if (!DeviceOK)
        {
            using var font = new Font("Microsoft YaHei UI", 14, FontStyle.Bold, GraphicsUnit.Pixel);
            using var fmt = new StringFormat { Alignment = StringAlignment.Center };
            using var red = new SolidBrush(Color.FromArgb(255, 69, 58));
            g.DrawString("设备离线", font, red, new RectangleF(0, 60, 240, 20), fmt);
        }

        // approval pending: blink the whole border red over everything else
        if (NeedsInput && FlashOn)
        {
            using var red = new SolidBrush(Color.FromArgb(255, 59, 48));
            g.FillRectangle(red, m, m, side, t);
            g.FillRectangle(red, m, 240 - m - t, side, t);
            g.FillRectangle(red, m, m, t, side);
            g.FillRectangle(red, 240 - m - t, m, t, side);
        }
    }

    void DrawMusicScene(Graphics g)
    {
        var coverRect = new Rectangle(56, 16, 128, 128);
        if (MusicCover != null)
        {
            var state = g.Save();
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.PixelOffsetMode = PixelOffsetMode.Half;
            g.DrawImage(MusicCover, coverRect);
            g.Restore(state);
        }
        else
        {
            using var dark = new SolidBrush(Color.FromArgb(64, 64, 64));
            g.FillRectangle(dark, coverRect);
            using var font = new Font("Consolas", 13, FontStyle.Bold, GraphicsUnit.Pixel);
            using var fmt = new StringFormat { Alignment = StringAlignment.Center };
            g.DrawString("No Art", font, Brushes.LightGray, new RectangleF(56, 72, 128, 20), fmt);
        }

        using var titleFmt = new StringFormat
        {
            Alignment = StringAlignment.Center,
            Trimming = StringTrimming.EllipsisCharacter,
            FormatFlags = StringFormatFlags.NoWrap,
        };
        var title = MusicTitle.Length == 0 ? "No Music" : MusicTitle;
        using (var font = new Font("Microsoft YaHei UI", 15, FontStyle.Bold, GraphicsUnit.Pixel))
        {
            g.DrawString(title, font, Brushes.White, new RectangleF(12, 154, 216, 24), titleFmt);
        }
        using (var font = new Font("Microsoft YaHei UI", 12, FontStyle.Regular, GraphicsUnit.Pixel))
        {
            g.DrawString(MusicArtist, font, Brushes.LightGray,
                         new RectangleF(12, 178, 216, 20), titleFmt);
        }

        var bar = new RectangleF(20, 210, 200, 8);
        using var barBg = new SolidBrush(Color.FromArgb(64, 64, 64));
        g.FillRectangle(barBg, bar);
        var frac = MusicDuration > 0 ? (float)Math.Clamp(MusicElapsed / MusicDuration, 0, 1) : 0;
        using var barFill = new SolidBrush(MusicPlaying ? Green : Color.Gray);
        g.FillRectangle(barFill, bar.X, bar.Y, bar.Width * frac, bar.Height);
    }

    /// Replica of the firmware's net-speed screen v2: header readouts, then
    /// a 224x128 area chart at (8,60) — dim-green DL fill with bright top
    /// edge, 2px yellow UL line, quarter gridlines, shared nice scale.
    void DrawNetScene(Graphics g)
    {
        var grey = Color.FromArgb(140, 140, 140);
        using var greyBrush = new SolidBrush(grey);
        using var labelFont = new Font("Consolas", 8, FontStyle.Regular, GraphicsUnit.Pixel);

        g.DrawString("DOWN", labelFont, greyBrush, 14, 8);
        g.DrawString("UP", labelFont, greyBrush, 134, 8);
        using (var valueFont = new Font("Consolas", 19, FontStyle.Bold, GraphicsUnit.Pixel))
        using (var greenBrush = new SolidBrush(Green))
        using (var yellowBrush = new SolidBrush(Yellow))
        {
            g.DrawString(NetHeaderDL + "/s", valueFont, greenBrush, 12, 19);
            g.DrawString(NetHeaderUL + "/s", valueFont, yellowBrush, 132, 19);
        }

        const float cx = 8, cy = 60, cw = 224, ch = 128;
        var scale = AdaptiveNetScale(Math.Max(_histRx.Max(), _histTx.Max()));

        // quarter gridlines
        using (var grid = new Pen(Color.FromArgb(41, 41, 41), 1))
        {
            for (int q = 1; q <= 3; q++)
            {
                var y = cy + ch * q / 4;
                g.DrawLine(grid, cx, y, cx + cw, y);
            }
        }

        // 3-tap smoothed points, one per column (matches the device)
        PointF[] Points(double[] vals)
        {
            var pts = new PointF[NetCols];
            for (int i = 0; i < NetCols; i++)
            {
                var lo = Math.Max(0, i - 1);
                var hi = Math.Min(NetCols - 1, i + 1);
                var v = (vals[lo] + vals[i] + vals[hi]) / 3;
                var hgt = (float)Math.Min(v / scale, 1) * (ch - 2);
                pts[i] = new PointF(cx + i, cy + ch - 1 - hgt);
            }
            return pts;
        }

        // download: filled area + bright top edge
        var dl = Points(_histRx);
        using (var path = new GraphicsPath())
        {
            path.AddLine(cx, cy + ch - 1, dl[0].X, dl[0].Y);
            path.AddLines(dl);
            path.AddLine(dl[^1].X, dl[^1].Y, cx + cw - 1, cy + ch - 1);
            path.CloseFigure();
            using var fill = new SolidBrush(Color.FromArgb(0, 84, 0));
            g.FillPath(fill, path);
        }
        // NOT the firmware's LINE_T: the mirror window is ~4x the panel's
        // physical size, so a thin stroke here matches the device visually.
        using (var pen = new Pen(Green, 3) { LineJoin = LineJoin.Round })
        {
            g.DrawLines(pen, dl);
        }

        // upload: yellow line
        var ul = Points(_histTx);
        using (var pen = new Pen(Yellow, 3) { LineJoin = LineJoin.Round })
        {
            g.DrawLines(pen, ul);
        }

        // axis + footer labels
        using (var right = new StringFormat { Alignment = StringAlignment.Far })
        {
            g.DrawString(DeviceSpeedText(scale), labelFont, greyBrush,
                         new RectangleF(120, 46, 112, 12), right);
        }
        using (var center = new StringFormat { Alignment = StringAlignment.Center })
        {
            if (NetCPU >= 0)
            {
                // fixed-x label + value columns, so a value width change (5%
                // -> 30%) never shifts the rest of the row (matches firmware)
                using var sysLabelFont = new Font("Consolas", 7f);
                using var sysValueFont = new Font("Consolas", 11.5f, FontStyle.Bold);
                g.DrawString("CPU", sysLabelFont, greyBrush, 28, 196);
                g.DrawString($"{NetCPU}%", sysValueFont, Brushes.White, 62, 189);
                g.DrawString("MEM", sysLabelFont, greyBrush, 130, 196);
                g.DrawString($"{NetMem}%", sysValueFont, Brushes.White, 164, 189);
            }
            g.DrawString("PC NET  -  56s", labelFont, greyBrush,
                         new RectangleF(0, 212, 240, 12), center);
        }
    }

    // Stock watchlist, same 54px rows as the firmware: grey code (the mirror
    // can render the CJK name next to it), big white price, colored change.
    void DrawStockScene(Graphics g)
    {
        using var greyBrush = new SolidBrush(Color.FromArgb(140, 140, 140));
        using var codeFont = new Font("Microsoft YaHei UI", 7.5f);
        using var valueFont = new Font("Consolas", 13f, FontStyle.Bold);
        using var labelFont = new Font("Consolas", 6.5f);
        if (StockRows.Length == 0)
        {
            using var center0 = new StringFormat { Alignment = StringAlignment.Center };
            using var hintFont = new Font("Microsoft YaHei UI", 8.5f);
            g.DrawString("未配置自选股\n右键托盘 → 设置自选股…", hintFont, greyBrush,
                         new RectangleF(0, 104, 240, 40), center0);
            return;
        }
        for (int i = 0; i < Math.Min(StockRows.Length, 4); i++)
        {
            var row = StockRows[i];
            float y0 = 10 + i * 54;
            var label = row.Name.Length == 0 ? row.Code : $"{row.Code}  {row.Name}";
            g.DrawString(label, codeFont, greyBrush, 14, y0);
            g.DrawString(row.Price, valueFont, Brushes.White, 12, y0 + 16);
            using var pctBrush = new SolidBrush(row.Up > 0 ? Color.FromArgb(255, 59, 48)
                : (row.Up < 0 ? Green : Color.LightGray));
            using var right = new StringFormat { Alignment = StringAlignment.Far };
            g.DrawString(row.Pct, valueFont, pctBrush, new RectangleF(120, y0 + 16, 106, 22), right);
        }
        using var center = new StringFormat { Alignment = StringAlignment.Center };
        g.DrawString("STOCKS", labelFont, greyBrush, new RectangleF(0, 226, 240, 12), center);
    }

    /// Same compact unit strings the firmware prints ("2.3M", "480K").
    public static string DeviceSpeedText(double bps)
    {
        if (bps >= 1_000_000) return $"{bps / 1_000_000:F1}M";
        if (bps >= 1_000) return $"{bps / 1_000:F0}K";
        return $"{bps:F0}B";
    }

    // Desktop-mirror / photo-album scene: the bridge's full-panel RGB565
    // frame (ScreenMirrorService / AlbumService) as a 240x240 replica.
    void DrawSceneFrame(Graphics g)
    {
        if (SceneFrame == null)
        {
            using var greyBrush = new SolidBrush(Color.FromArgb(80, 80, 80));
            using var hintFont = new Font("Microsoft YaHei UI", 9f);
            using var center = new StringFormat { Alignment = StringAlignment.Center };
            g.DrawString(MirrorMode ? "等待投屏画面…" : "相册为空\n在 exe 旁建 album 文件夹\n放入图片后重启", 
                         hintFont, greyBrush, new RectangleF(0, 100, 240, 40), center);
            return;
        }
        // frame is 128x128 panel pixels; upscale to the 240x240 replica
        g.InterpolationMode = InterpolationMode.HighQualityBilinear;
        g.DrawImage(SceneFrame, 0, 0, 240, 240);
    }

    // Music-spectrum scene: draw the 24 live bar magnitudes on the 240x240
    // replica in the same style the device is showing (styles 0-4), so the
    // popup preview matches the panel.
    static Color PaletteColor(int idx) => idx switch
    {
        1 => Color.FromArgb(0, 200, 200),     // cyan
        2 => Color.FromArgb(255, 204, 0),     // yellow
        3 => Color.FromArgb(255, 140, 0),     // orange
        4 => Color.FromArgb(230, 60, 60),     // red
        5 => Color.FromArgb(230, 60, 230),    // magenta
        6 => Color.FromArgb(230, 230, 230),   // white
        7 => Color.FromArgb(180, 220, 60),    // green-yellow
        _ => Green,                           // green
    };
    Color PaletteSpectrum() => PaletteColor(SpectrumColor);

    /// Hue sweep matching the firmware's spectrumRainbowColor: 0 (low) →
    /// blue … green … yellow … red (255 high), so the preview glows across
    /// the spectrum with the audio magnitude.
    static Color RainbowColor(int v)
    {
        int h = v * 300 / 255;
        int sector = h / 60, f = h % 60;
        int r, g, b;
        switch (sector)
        {
            case 0: r = 255; g = f * 255 / 60; b = 0; break;
            case 1: r = 255 - f * 255 / 60; g = 255; b = 0; break;
            case 2: r = 0; g = 255; b = f * 255 / 60; break;
            case 3: r = 0; g = 255 - f * 255 / 60; b = 255; break;
            case 4: r = f * 255 / 60; g = 0; b = 255; break;
            default: r = 255; g = 0; b = 255 - f * 255 / 60; break;
        }
        return Color.FromArgb(r, g, b);
    }

    // 纵向彩虹带/色谱行 for wave & radial types: color from a screen row
    // y (vertical sweep) so lines & radial bars show the gradient too.
    Color GradRow(int y)
    {
        if (SpectrumColor == 10)
        {
            int v = (239 - y) * 255 / 239;
            if (SpectrumGradReverse == 1) v = 255 - v;
            return RainbowColor(v);
        }
        if (SpectrumColor == 11)
        {
            // 色谱行: SCREEN-ANCHORED, same as the bars (identical direction
            // & past-threshold hue as the firmware's fillBar 色谱行)
            int thrPx = 240 * SpectrumGradRange / 100;
            if (thrPx < 1) thrPx = 1;
            int v;
            if (y < thrPx)
            {
                int ratio = y * 255 / thrPx; // 0 (top) .. 255 (threshold)
                v = SpectrumGradReverse == 1 ? ratio : 255 - ratio;
            }
            else
            {
                v = SpectrumGradReverse == 1 ? 255 : 0; // past threshold
            }
            return RainbowColor(v);
        }
        return PaletteColor(SpectrumColor);
    }
    // wave lines: vertical-gradient when color 10/11, else primary palette
    Color WaveCol(int i, int y) =>
        SpectrumColor == 10 || SpectrumColor == 11 ? GradRow(y) : PaletteColor(SpectrumColor);
    // radial: radius-gradient when color 10/11, else primary palette
    Color RadCol(int i, int r)
    {
        if (SpectrumColor == 10 || SpectrumColor == 11)
        {
            int span = Math.Max(8, SpectrumRingOuter - SpectrumRingInner);
            int v = (r - SpectrumRingInner) * 255 / span;
            v = Math.Clamp(v, 0, 255);
            if (SpectrumGradReverse == 1) v = 255 - v;
            return RainbowColor(v);
        }
        return PaletteColor(SpectrumColor);
    }

    void DrawSpectrumScene(Graphics g)
    {
        var bars = SpectrumBars ?? Array.Empty<byte>();
        const int n = 24;
        int bw = 240 / n;                  // 10px per bar slot on the replica
        const int maxH = 228;              // leave a bottom margin
        var barColor = PaletteSpectrum();

        // adaptive rainbow range (mirrors the firmware): only relevant for
        // 色谱列 (color 9, hue follows magnitude); the max decays slowly, the
        // min rises slowly, so the hue sweep tracks the current audio
        // envelope instead of a fixed 0-255 span.
        if (SpectrumColor == 9 && bars.Length > 0)
        {
            byte curMin = 255, curMax = 0;
            foreach (var b in bars) { if (b < curMin) curMin = b; if (b > curMax) curMax = b; }
            _specMax = Math.Max(curMax, _specMax * 0.985f);
            _specMin = Math.Min(curMin, _specMin * 1.02f + 0.5f);
            if (_specMax - _specMin < 20) _specMax = _specMin + 20;
        }

        // per-bar brush: 横向彩虹带 (color 8) = horizontal sweep by bar index,
        // 色谱列 (color 9) = magnitude sweep over the live range, else solid.
        SolidBrush BarBrush(int i)
        {
            if (SpectrumColor == 8)
            {
                int v = i * 255 / (n > 1 ? n - 1 : 1);
                if (SpectrumGradReverse == 1) v = 255 - v;
                return new SolidBrush(RainbowColor(v));
            }
            if (SpectrumColor == 9)
            {
                float span = _specMax - _specMin;
                if (span <= 0) span = 1;
                int v = (int)((bars[i] - _specMin) * 255 / span);
                v = Math.Clamp(v, 0, 255);
                if (SpectrumGradReverse == 1) v = 255 - v;
                return new SolidBrush(RainbowColor(v));
            }
            return new SolidBrush(barColor);
        }

        // bars-type dynamic range (mirrors the firmware): when AutoRange is on,
        // normalize each bar to the live [min,max] envelope so the spectrum
        // always fills the panel; Offset shifts it up/down (% of 255). Shared
        // by all types (bars / wave / radial).
        int BarV(int i)
        {
            int v = Math.Clamp(bars.Length > i ? bars[i] : 0, 0, 255);
            if (SpectrumAutoRange == 1 && bars.Length > 0)
            {
                byte mn = 255, mx = 0;
                foreach (var b in bars) { if (b < mn) mn = b; if (b > mx) mx = b; }
                if (mx - mn < 8) mx = (byte)(mn + 8);
                v = (v - mn) * 255 / (mx - mn);
                v = Math.Clamp(v, 0, 255);
            }
            v += SpectrumOffset * 255 / 100; // 抬高/降低
            v = Math.Clamp(v, 0, 255);
            return v;
        }
        int BarH(int i) => Math.Clamp(BarV(i) * maxH / 255, 2, maxH);

        // fill color (填充色): 0-7 solid palette, 8 = 横向彩虹带 (index sweep),
        // 9+ = magnitude sweep over the live range — mirrors firmware.
        Color FillColorFor(int i)
        {
            if (SpectrumFillColor == 8)
            {
                int v = i * 255 / (n > 1 ? n - 1 : 1);
                return RainbowColor(v);
            }
            if (SpectrumFillColor >= 9)
            {
                float span = _specMax - _specMin;
                if (span <= 0) span = 1;
                int v = (int)((bars[i] - _specMin) * 255 / span);
                v = Math.Clamp(v, 0, 255);
                return RainbowColor(v);
            }
            return PaletteColor(SpectrumFillColor);
        }

        // 纵向彩虹带 (color 10): bar painted with a fixed bottom→top hue
        // gradient. 色谱行 (color 11): gradient anchored to the SCREEN, not
        // the bar — hue at any pixel row is fixed (bottom = hue 0, top = hue
        // 255), every bar shows the same standing gradient; a taller bar
        // reaches the next gradient color further up. Falls back otherwise.
        void FillBar(int i, int x, int y, int w, int h)
        {
            if ((SpectrumColor == 10 || SpectrumColor == 11) && h > 1)
            {
                const int segs = 8;
                // 色谱行 threshold: the hue sweep occupies only [0 .. range%]
                // of the panel height (from the top); rows below are painted
                // with the sweep's final hue.
                int thrPx = 240 * SpectrumGradRange / 100;
                if (thrPx < 1) thrPx = 1;
                for (int s = 0; s < segs; s++)
                {
                    int y0 = y + h * s / segs;
                    int y1 = y + h * (s + 1) / segs;
                    if (y1 <= y0) continue;
                    int v;
                    if (SpectrumColor == 11)
                    {
                        // 色谱行: SCREEN-ANCHORED fixed gradient. Inside the
                        // threshold hue depends only on the pixel row
                        // (identical for every bar); beyond it the final hue
                        // is used. SpectrumGradReverse flips the sweep.
                        if (y0 < thrPx)
                        {
                            int ratio = y0 * 255 / thrPx; // 0 (top) .. 255
                            v = SpectrumGradReverse == 1 ? ratio : 255 - ratio;
                        }
                        else
                        {
                            v = SpectrumGradReverse == 1 ? 255 : 0;
                        }
                    }
                    else
                    {
                        // 纵向彩虹带: bar-anchored gradient (bottom → top)
                        v = s * 255 / (segs - 1);
                        if (SpectrumGradReverse == 1) v = 255 - v;
                    }
                    using var segBrush = new SolidBrush(RainbowColor(v));
                    g.FillRectangle(segBrush, x, y0, w, y1 - y0);
                }
            }
            else
            {
                using var b = BarBrush(i);
                g.FillRectangle(b, x, y, w, h);
            }
        }

        // vertical mirror for bars: with SpectrumMirror ON the bar grows
        // symmetrically from the horizontal center line (y=120) — the center
        // line is the spectrum's 0 point, bar extends +h/2 up and -h/2 down
        // (oscilloscope style). With it OFF the bar grows from the bottom.
        void FillBarM(int i, int x, int w, int h)
        {
            if (SpectrumMirror == 1)
            {
                int h2 = (h + 1) / 2;
                FillBar(i, x, 120 - h2, w, h2 * 2); // centered on the axis
            }
            else
            {
                FillBar(i, x, 240 - h, w, h); // bottom bar
            }
        }

        // secondary color (辅助色): solid palette, 8 = 横向彩虹带 (index
        // sweep), 9+ = magnitude sweep — mirrors the firmware's col2For.
        Color Col2Color(int i)
        {
            if (SpectrumColor2 == 8)
            {
                int v = i * 255 / (n > 1 ? n - 1 : 1);
                return RainbowColor(v);
            }
            if (SpectrumColor2 >= 9)
            {
                float span = _specMax - _specMin;
                if (span <= 0) span = 1;
                int v = (int)((bars[i] - _specMin) * 255 / span);
                v = Math.Clamp(v, 0, 255);
                return RainbowColor(v);
            }
            return PaletteColor(SpectrumColor2);
        }

        using var green = new SolidBrush(barColor);
        using var yellow = new SolidBrush(Yellow);
        using var grey = new SolidBrush(Color.FromArgb(90, 90, 90));

        if (SpectrumType == 0 && SpectrumEffect == 1)
        {
            // mirrored: left half grows rightward from center, right half leftward
            g.Clear(Color.Black);
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                int x = i * bw;
                int col = 119;             // center column (240/2 - 1)
                if (x < col)
                    FillBar(i, x, 240 - h, col - x, h);
                else
                    FillBar(i, col, 240 - h, x + bw - col, h);
            }
            return;
        }
        if (SpectrumType == 1 && SpectrumEffect == 0)
        {
            // waveform: polyline through bar tops
            g.Clear(Color.Black);
            // fill under the wave when SpectrumFill is on
            if (SpectrumFill == 1)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    int y1 = 240 - BarV(i) * maxH / 255;
                    int y2 = 240 - BarV(i + 1) * maxH / 255;
                    int yTop = Math.Min(y1, y2);
                    using var fill = new SolidBrush(FillColorFor(i));
                    g.FillRectangle(fill, i * bw, yTop, bw, 240 - yTop);
                }
            }
            using var pen = new Pen(Green, 2f);
            var pts = new Point[n];
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                // 中轴 0 点模式：波形从中轴向上生长（120-h），镜像自动向下
                pts[i] = new Point(i * bw, SpectrumMirror == 1 ? 120 - h : 240 - h);
            }
            // vertical mirror: wave mirrored across the horizontal center line
            var mir = SpectrumMirror == 1 ? pts.Select(p => new Point(p.X, 240 - p.Y)).ToArray() : null;
            // gradient sweep: draw segment-by-segment with per-row color
            if (SpectrumColor == 10 || SpectrumColor == 11)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    using var segPen = new Pen(WaveCol(i, pts[i].Y), 2f);
                    g.DrawLine(segPen, pts[i], pts[i + 1]);
                    if (mir != null)
                    {
                        using var mPen = new Pen(WaveCol(i, mir[i].Y), 2f);
                        g.DrawLine(mPen, mir[i], mir[i + 1]);
                    }
                }
            }
            else
            {
                g.DrawLines(pen, pts);
                if (mir != null) g.DrawLines(pen, mir);
            }
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 2)
        {
            // peak-hold: bars + a fixed peak line (decay handled per-frame)
            g.Clear(Color.Black);
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                FillBarM(i, i * bw, bw - 2, h);
                g.FillRectangle(yellow, i * bw, 240 - h - 3, bw - 2, 2);
            }
            return;
        }
        if (SpectrumType == 2 && SpectrumEffect == 0)
        {
            // radial: trapezoid slices from ringInner outward; width from
            // ringW, gap from ringGap, radius from ringInner/ringOuter. With
            // dual-ring each slice is inner [ringInner, r1] + outer [r1, r2].
            g.Clear(Color.Black);
            int cx = 120, cy = 120;
            float step = 360.0f / n;
            float halfW = step * 0.35f * SpectrumRingW / 2.0f;
            float gapDeg = SpectrumRingGap * 0.4f;
            int rSpan = Math.Max(8, SpectrumRingOuter - SpectrumRingInner);
            for (int i = 0; i < n; i++)
            {
                float a0 = (i * step + gapDeg) * 0.0174533f;
                float a1 = (i * step + gapDeg + halfW * 2) * 0.0174533f;
                int r1 = SpectrumRingInner + BarV(i) * (SpectrumDualRing == 1 ? rSpan * SpectrumDualInner / 100 : rSpan) / 255;
                // dual-ring mirrors the dedicated 双环 style: each slice is a
                // spoke from r1 (inner tip) to r2 (outer tip), both following
                // the magnitude. dualInner scales r1, dualOuter scales r2.
                int r2 = SpectrumDualRing == 1 ? r1 + BarV(i) * (24 * SpectrumDualOuter / 100) / 255 : r1;
                // keep the outer ring inside the panel (center 120 → 119 max)
                if (SpectrumDualRing == 1 && r2 > 119) r2 = 119;
                int outer = SpectrumDualRing == 1 ? r2 : r1;
                var x0 = cx + (int)(Math.Cos(a0) * SpectrumRingInner);
                var y0 = cy + (int)(Math.Sin(a0) * SpectrumRingInner);
                var x1 = cx + (int)(Math.Cos(a1) * SpectrumRingInner);
                var y1 = cy + (int)(Math.Sin(a1) * SpectrumRingInner);
                // normal = [ringInner, r1]; dual-ring = spoke [r1, r2]
                int rIn = SpectrumDualRing == 1 ? r1 : SpectrumRingInner;
                var xa = cx + (int)(Math.Cos(a0) * rIn);
                var ya = cy + (int)(Math.Sin(a0) * rIn);
                var xb = cx + (int)(Math.Cos(a1) * rIn);
                var yb = cy + (int)(Math.Sin(a1) * rIn);
                var xc = cx + (int)(Math.Cos(a1) * outer);
                var yc = cy + (int)(Math.Sin(a1) * outer);
                var xd = cx + (int)(Math.Cos(a0) * outer);
                var yd = cy + (int)(Math.Sin(a0) * outer);
                using var slice = new SolidBrush(RadCol(i, outer));
                g.FillPolygon(slice, new[] { new Point(xa, ya), new Point(xb, yb), new Point(xc, yc), new Point(xd, yd) });
            }
            var ringInPen = SpectrumRingInColor == 8 ? Pens.Black : new Pen(PaletteColor(SpectrumRingInColor));
            if (SpectrumRingInColor != 9) // 9 = off: skip the inner ring
                g.DrawEllipse(ringInPen, cx - SpectrumRingInner, cy - SpectrumRingInner,
                              SpectrumRingInner * 2, SpectrumRingInner * 2);
            if (SpectrumRingInColor != 8) ringInPen.Dispose();
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 3)
        {
            // twin: two thin bars per slot (main + half-height companion)
            g.Clear(Color.Black);
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                int h2 = BarV(i) * maxH / 2 / 255;
                FillBarM(i, i * bw, 4, h);
                g.FillRectangle(grey, i * bw + 6, 240 - h2, 4, h2);
            }
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 4)
        {
            // dotted: bar height as a column of 2x2 dots; with mirror the
            // dot columns grow symmetrically from the center line (120)
            g.Clear(Color.Black);
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                if (SpectrumMirror == 1)
                {
                    int h2 = (h + 1) / 2;
                    for (int y = 118; y > 120 - h2; y -= 4)
                        FillBar(i, i * bw + 1, y - 2, 6, 2);
                    for (int y = 122; y < 120 + h2; y += 4)
                        FillBar(i, i * bw + 1, y - 2, 6, 2);
                }
                else
                {
                    for (int y = 238; y > 240 - h; y -= 4)
                        FillBar(i, i * bw + 1, y - 2, 6, 2);
                }
            }
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 5)
        {
            // glow: bar with a bright 2px cap and darker body; with mirror
            // the bar grows symmetrically from the center line (120)
            g.Clear(Color.Black);
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                if (SpectrumMirror == 1)
                {
                    int h2 = (h + 1) / 2;
                    g.FillRectangle(grey, i * bw, 120 - h2, bw - 2, h2 * 2 - 2);
                    FillBar(i, i * bw, 120 - h2, bw - 2, 2);   // top cap
                    FillBar(i, i * bw, 120 + h2 - 2, bw - 2, 2); // bottom cap
                }
                else
                {
                    g.FillRectangle(grey, i * bw, 240 - h, bw - 2, h - 2);
                    FillBar(i, i * bw, 240 - h, bw - 2, 2);
                }
            }
            return;
        }
        if (SpectrumType == 1 && SpectrumEffect == 1)
        {
            // mirror-wave: two mirrored polylines around the vertical center
            g.Clear(Color.Black);
            int midY = 120;
            // fill toward the center line when SpectrumFill is on
            if (SpectrumFill == 1)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    int y1 = midY - BarV(i) * (midY - 6) / 255;
                    int y2 = midY - BarV(i + 1) * (midY - 6) / 255;
                    int yTop = Math.Min(y1, y2);
                    using var fill = new SolidBrush(FillColorFor(i));
                    g.FillRectangle(fill, i * bw, yTop, bw, midY - yTop);
                    g.FillRectangle(fill, i * bw, midY, bw, midY - yTop);
                }
            }
            using var pen = new Pen(Green, 2f);
            var pts = new Point[n];
            for (int i = 0; i < n; i++)
            {
                int h = BarV(i) * (midY - 6) / 255;
                pts[i] = new Point(i * bw, midY - h);
            }
            var mirrored = pts.Select(p => new Point(p.X, 2 * midY - p.Y)).ToArray();
            if (SpectrumColor == 10 || SpectrumColor == 11)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    using var segPen = new Pen(WaveCol(i, pts[i].Y), 2f);
                    g.DrawLine(segPen, pts[i], pts[i + 1]);
                    using var segPen2 = new Pen(WaveCol(i, mirrored[i].Y), 2f);
                    g.DrawLine(segPen2, mirrored[i], mirrored[i + 1]);
                }
            }
            else
            {
                g.DrawLines(pen, pts);
                g.DrawLines(pen, mirrored);
            }
            g.DrawLine(pen, 0, midY, 240, midY);
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 6)
        {
            // fire: three-tier gradient bar (red/orange/yellow)
            g.Clear(Color.Black);
            using var red = new SolidBrush(Color.FromArgb(255, 60, 30));
            using var orange = new SolidBrush(Color.FromArgb(255, 150, 40));
            using var yellowBrush = new SolidBrush(Color.FromArgb(255, 230, 90));
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                int redH = h * 2 / 3;
                g.FillRectangle(red, i * bw, 240 - redH, bw - 2, redH);
                g.FillRectangle(orange, i * bw, 240 - redH, bw - 2, h / 3);
                g.FillRectangle(yellowBrush, i * bw, 240 - h, bw - 2, h - redH);
            }
            return;
        }
        if (SpectrumType == 1 && SpectrumEffect == 2)
        {
            // aurora: bright polyline + dimmer echo above it
            g.Clear(Color.Black);
            // fill under the wave when SpectrumFill is on
            if (SpectrumFill == 1)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    int y1 = 240 - BarV(i) * maxH / 255;
                    int y2 = 240 - BarV(i + 1) * maxH / 255;
                    int yTop = Math.Min(y1, y2);
                    using var fill = new SolidBrush(FillColorFor(i));
                    g.FillRectangle(fill, i * bw, yTop, bw, 240 - yTop);
                }
            }
            using var pen = new Pen(Green, 2f);
            using var echoPen = new Pen(Color.FromArgb(90, 90, 90), 2f);
            var pts = new Point[n];
            var echoes = new Point[n];
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                // 中轴 0 点模式：主波/回波从中轴向上生长，镜像自动向下
                pts[i] = new Point(i * bw, SpectrumMirror == 1 ? 120 - h : 240 - h);
                echoes[i] = new Point(i * bw, SpectrumMirror == 1 ? 120 - h - 10 : 240 - h - 10);
            }
            if (SpectrumColor == 10 || SpectrumColor == 11)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    using var segPen = new Pen(WaveCol(i, pts[i].Y), 2f);
                    g.DrawLine(segPen, pts[i], pts[i + 1]);
                }
            }
            else
            {
                g.DrawLines(echoPen, echoes);
                g.DrawLines(pen, pts);
            }
            // vertical mirror: aurora mirrored across the horizontal center line
            if (SpectrumMirror == 1)
            {
                var mPts = pts.Select(p => new Point(p.X, 240 - p.Y)).ToArray();
                var mEchoes = echoes.Select(p => new Point(p.X, 240 - p.Y)).ToArray();
                if (SpectrumColor == 10 || SpectrumColor == 11)
                {
                    for (int i = 0; i < n - 1; i++)
                    {
                        using var segPen = new Pen(WaveCol(i, mPts[i].Y), 2f);
                        g.DrawLine(segPen, mPts[i], mPts[i + 1]);
                    }
                }
                else
                {
                    g.DrawLines(echoPen, mEchoes);
                    g.DrawLines(pen, mPts);
                }
            }
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 8)
        {
            // starry (bars type): dotted bars with white twinkle pixels; with
            // mirror the dot columns grow symmetrically from the center line
            g.Clear(Color.Black);
            var rnd = new Random();
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                if (SpectrumMirror == 1)
                {
                    int h2 = (h + 1) / 2;
                    for (int y = 118; y > 120 - h2; y -= 4)
                        FillBar(i, i * bw + 1, y - 2, 6, 2);
                    for (int y = 122; y < 120 + h2; y += 4)
                        FillBar(i, i * bw + 1, y - 2, 6, 2);
                    if (h > 8 && rnd.Next(100) < 30)
                    {
                        g.FillRectangle(Brushes.White, i * bw + 2, 120 - h2 - 3, 3, 3);
                        g.FillRectangle(Brushes.White, i * bw + 2, 120 + h2 + 1, 3, 3);
                    }
                }
                else
                {
                    for (int y = 238; y > 240 - h; y -= 4)
                        FillBar(i, i * bw + 1, y - 2, 6, 2);
                    if (h > 8 && rnd.Next(100) < 30)
                        g.FillRectangle(Brushes.White, i * bw + 2, 240 - h - 3, 3, 3);
                }
            }
            return;
        }
        if (SpectrumType == 0 && SpectrumEffect == 7)
        {
            // bars+wave: classic bars (primary color) with a polyline (color2)
            g.Clear(Color.Black);
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i);
                FillBarM(i, i * bw, bw - 2, h);
            }
            using var wavePen = new Pen(Col2Color(0), 2f);
            var pts = new Point[n];
            for (int i = 0; i < n; i++)
                pts[i] = new Point(i * bw, 240 - BarV(i) * maxH / 255);
            g.DrawLines(wavePen, pts);
            return;
        }
        if (SpectrumType == 1 && SpectrumEffect == 3)
        {
            // wave+bars: half-height bars (color2) under a bright wave (primary)
            g.Clear(Color.Black);
            // fill under the wave when SpectrumFill is on (between bars and line)
            if (SpectrumFill == 1)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    int y1 = 240 - BarV(i) * maxH / 255;
                    int y2 = 240 - BarV(i + 1) * maxH / 255;
                    int yTop = Math.Min(y1, y2);
                    using var fill = new SolidBrush(FillColorFor(i));
                    g.FillRectangle(fill, i * bw, yTop, bw, 240 - yTop);
                }
            }
            using var bar2 = new SolidBrush(Col2Color(0));
            for (int i = 0; i < n; i++)
            {
                int h = BarH(i) / 2;
                g.FillRectangle(bar2, i * bw, 240 - h, bw - 2, h);
                if (SpectrumMirror == 1) // top mirror bar
                    g.FillRectangle(bar2, i * bw, 0, bw - 2, h);
            }
            using var wavePen = new Pen(Green, 2f);
            var pts = new Point[n];
            for (int i = 0; i < n; i++)
            {
                int v = BarV(i) * maxH / 255;
                // 中轴 0 点模式：波形从中轴向上生长，镜像自动向下
                pts[i] = new Point(i * bw, SpectrumMirror == 1 ? 120 - v : 240 - v);
            }
            if (SpectrumColor == 10 || SpectrumColor == 11)
            {
                for (int i = 0; i < n - 1; i++)
                {
                    using var segPen = new Pen(WaveCol(i, pts[i].Y), 2f);
                    g.DrawLine(segPen, pts[i], pts[i + 1]);
                }
            }
            else
            {
                g.DrawLines(wavePen, pts);
            }
            if (SpectrumMirror == 1) // vertical mirror: wave across center line
            {
                var mir = pts.Select(p => new Point(p.X, 240 - p.Y)).ToArray();
                if (SpectrumColor == 10 || SpectrumColor == 11)
                {
                    for (int i = 0; i < n - 1; i++)
                    {
                        using var mPen = new Pen(WaveCol(i, mir[i].Y), 2f);
                        g.DrawLine(mPen, mir[i], mir[i + 1]);
                    }
                }
                else
                {
                    g.DrawLines(wavePen, mir);
                }
            }
            return;
        }
        if (SpectrumType == 2 && SpectrumEffect == 1)
        {
            // double-ring radial: two concentric radiating rings
            g.Clear(Color.Black);
            int cx = 120, cy = 120;
            using var pen = new Pen(Green, 3f);
            for (int i = 0; i < n; i++)
            {
                float ang = (float)(i * 360.0 / n) * 0.0174533f;
                int r1 = 30 + BarV(i) * 48 / 255;
                int r2 = r1 + BarV(i) * 30 / 255;
                g.DrawLine(pen,
                           cx + (int)(Math.Cos(ang) * r1), cy + (int)(Math.Sin(ang) * r1),
                           cx + (int)(Math.Cos(ang) * r2), cy + (int)(Math.Sin(ang) * r2));
            }
            g.FillEllipse(grey, cx - 6, cy - 6, 12, 12);
            return;
        }
        if (SpectrumType == 2 && SpectrumEffect == 2)
        {
            // ring polyline: bar tips connected into a closed wavy line with
            // the band between the inner circle and the line filled.
            g.Clear(Color.Black);
            int cx = 120, cy = 120;
            int rIn = SpectrumRingInner;
            int rSpan = Math.Max(8, SpectrumRingOuter - SpectrumRingInner);
            var px = new int[n];
            var py = new int[n];
            for (int i = 0; i < n; i++)
            {
                float ang = (float)(i * 360.0 / n) * 0.0174533f;
                int r = rIn + BarV(i) * rSpan / 255;
                px[i] = cx + (int)(Math.Cos(ang) * r);
                py[i] = cy + (int)(Math.Sin(ang) * r);
            }
            // filled ribbon: triangle per segment from inner circle to the line
            if (SpectrumRingFill == 1)
            {
                for (int i = 0; i < n; i++)
                {
                    int j = (i + 1) % n;
                    float ang = (float)(i * 360.0 / n) * 0.0174533f;
                    int ix = cx + (int)(Math.Cos(ang) * rIn);
                    int iy = cy + (int)(Math.Sin(ang) * rIn);
                    using var fill = new SolidBrush(FillColorFor(i));
                    g.FillPolygon(fill, new[] { new Point(ix, iy), new Point(px[i], py[i]), new Point(px[j], py[j]) });
                }
            }
            // the wavy closed line itself
            using var ringPen = new Pen(RadCol(0, SpectrumRingInner), SpectrumRingW);
            for (int i = 0; i < n; i++)
            {
                int j = (i + 1) % n;
                g.DrawLine(ringPen, px[i], py[i], px[j], py[j]);
            }
            var ringInPen = SpectrumRingInColor == 8 ? Pens.Black : new Pen(PaletteColor(SpectrumRingInColor));
            if (SpectrumRingInColor != 9) // 9 = off: skip the inner ring
                g.DrawEllipse(ringInPen, cx - rIn, cy - rIn, rIn * 2, rIn * 2);
            if (SpectrumRingInColor != 8) ringInPen.Dispose();
            return;
        }
        if (SpectrumType == 2 && SpectrumEffect == 3)
        {
            // fan: each spectrum line becomes a small pie slice from the
            // inner to the outer radius. With dual-ring each slice is inner
            // [rIn, r] + outer [r, r2].
            g.Clear(Color.Black);
            int cx = 120, cy = 120;
            int rIn = SpectrumRingInner;
            int rSpan = Math.Max(8, SpectrumRingOuter - SpectrumRingInner);
            float step = 360.0f / n;
            for (int i = 0; i < n; i++)
            {
                float a0 = (i * step) * 0.0174533f;
                float a1 = ((i + 1) * step) * 0.0174533f;
                int r = rIn + BarV(i) * (SpectrumDualRing == 1 ? rSpan * SpectrumDualInner / 100 : rSpan) / 255;
                // dual-ring mirrors the dedicated 双环 style: each slice is a
                // spoke from r (inner tip) to r2 (outer tip), both following
                // the magnitude. dualInner scales r, dualOuter scales r2.
                int r2 = SpectrumDualRing == 1 ? r + BarV(i) * (24 * SpectrumDualOuter / 100) / 255 : r;
                // keep the outer ring inside the panel (center 120 → 119 max)
                if (SpectrumDualRing == 1 && r2 > 119) r2 = 119;
                int outer = SpectrumDualRing == 1 ? r2 : r;
                var x0 = cx + (int)(Math.Cos(a0) * rIn);
                var y0 = cy + (int)(Math.Sin(a0) * rIn);
                var x1 = cx + (int)(Math.Cos(a1) * rIn);
                var y1 = cy + (int)(Math.Sin(a1) * rIn);
                // normal = [rIn, r]; dual-ring = spoke [r, r2]
                int rIn2 = SpectrumDualRing == 1 ? r : rIn;
                var xa = cx + (int)(Math.Cos(a0) * rIn2);
                var ya = cy + (int)(Math.Sin(a0) * rIn2);
                var xb = cx + (int)(Math.Cos(a1) * rIn2);
                var yb = cy + (int)(Math.Sin(a1) * rIn2);
                var xc = cx + (int)(Math.Cos(a1) * outer);
                var yc = cy + (int)(Math.Sin(a1) * outer);
                var xd = cx + (int)(Math.Cos(a0) * outer);
                var yd = cy + (int)(Math.Sin(a0) * outer);
                using var slice = new SolidBrush(RadCol(i, outer));
                g.FillPolygon(slice, new[] { new Point(xa, ya), new Point(xb, yb), new Point(xc, yc), new Point(xd, yd) });
            }
            var ringInPen = SpectrumRingInColor == 8 ? Pens.Black : new Pen(PaletteColor(SpectrumRingInColor));
            if (SpectrumRingInColor != 9) // 9 = off: skip the inner ring
                g.DrawEllipse(ringInPen, cx - rIn, cy - rIn, rIn * 2, rIn * 2);
            if (SpectrumRingInColor != 8) ringInPen.Dispose();
            return;
        }
        // style 0 (default): classic bottom-up bars
        g.Clear(Color.Black);
        for (int i = 0; i < n; i++)
        {
            int h = BarH(i);
            FillBarM(i, i * bw, bw - 2, h);
        }
    }

    static GraphicsPath RoundedRect(RectangleF r, float radius)
    {
        var path = new GraphicsPath();
        var d = radius * 2;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }
}

// MARK: - popup form (the popover)

sealed class MirrorForm : Form
{
    readonly StatusService _service;
    readonly NetSpeedMonitor _netMonitor;
    readonly NowPlayingMonitor _nowPlaying;
    readonly StockMonitor _stockMonitor;
    readonly ScreenMirrorService _mirrorService;
    readonly AlbumService _albumService;
    readonly SpectrumService _spectrumService;
    readonly MirrorControl _mirror = new();
    readonly RadioButton[] _modeButtons;
    static readonly string[] Modes = { "auto", "claude", "codex", "net", "music", "stock", "mirror", "album", "spectrum" };
    static readonly string[] ModeLabels = { "自动", "Claude", "Codex", "网速", "音乐", "股票", "投屏", "相册", "频谱" };
    readonly Label _statusLabel = new();
    readonly TrackBar _brightness = new() { Minimum = 0, Maximum = 100, TickStyle = TickStyle.None };
    readonly Label _brightnessValue = new();
    // Drag streams many scroll events; posts to the single-threaded ESP8266 web
    // server are throttled mid-drag and the final value always flushes on mouse-up.
    int? _pendingBrightness;
    DateTime _lastBrightnessSentAt = DateTime.MinValue;
    Button _albumModeBtn, _albumPrevBtn, _albumNextBtn, _albumRandomBtn;

    readonly System.Windows.Forms.Timer _pollTimer = new() { Interval = 1000 };
    // WinForms Timer re-enters Tick while a previous async FetchInfo (5s
    // timeout) is still in flight — a rebooting device then piles up 5
    // concurrent connections that the ESP8266 can't sustain and the pool
    // can't clean.  Gate re-entry so at most one poll is outstanding.
    bool _tickInFlight;
    readonly System.Windows.Forms.Timer _animTimer = new() { Interval = 120 };
    readonly System.Windows.Forms.Timer _spectrumTimer = new() { Interval = 100 };
    readonly System.Windows.Forms.Timer _sweepTimer = new()
    {
        Interval = (int)(NetSpeedMonitor.SampleInterval * 1000),
    };

    readonly Dictionary<string, (int Rev, List<Bitmap> Frames, int W, int H)> _spriteCache = new();
    DeviceInfo _lastInfo;
    string _fetchingSlot;
    bool _applyingMode; // suppress CheckedChanged while reflecting device state

    public MirrorForm(StatusService service, NetSpeedMonitor netMonitor, NowPlayingMonitor nowPlaying,
                      StockMonitor stockMonitor, ScreenMirrorService mirrorService, AlbumService albumService,
                      SpectrumService spectrumService)
    {
        _service = service;
        _netMonitor = netMonitor;
        _nowPlaying = nowPlaying;
        _stockMonitor = stockMonitor;
        _mirrorService = mirrorService;
        _albumService = albumService;
        _spectrumService = spectrumService;

        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.Manual;
        ShowInTaskbar = false;
        TopMost = true;
        BackColor = SystemColors.Control;
        Padding = new Padding(1);

        ClientSize = new Size(Px(316), Px(490));

        _mirror.SetBounds(Px(14), Px(14), Px(288), Px(288));
        Controls.Add(_mirror);

        // 9 mode buttons in two rows of five (5+4) so each label fits.
        _modeButtons = new RadioButton[Modes.Length];
        int cols = 5;
        var btnW = Px(288) / cols;
        for (int i = 0; i < Modes.Length; i++)
        {
            int row = i / cols, col = i % cols;
            var btn = new RadioButton
            {
                Appearance = Appearance.Button,
                Text = ModeLabels[i],
                TextAlign = ContentAlignment.MiddleCenter,
                Tag = Modes[i],
                AutoSize = false,
                Margin = new Padding(1),
            };
            btn.SetBounds(Px(14) + col * btnW, Px(312) + row * Px(30), btnW - Px(2), Px(26));
            btn.CheckedChanged += ModeChanged;
            _modeButtons[i] = btn;
            Controls.Add(btn);
        }

        var sunLabel = new Label
        {
            Text = "\u2600",
            TextAlign = ContentAlignment.MiddleCenter,
            ForeColor = SystemColors.GrayText,
        };
        sunLabel.SetBounds(Px(12), Px(400), Px(24), Px(24));
        Controls.Add(sunLabel);
        _brightness.SetBounds(Px(36), Px(400), Px(216), Px(24));
        _brightness.Scroll += (_, _) => OnBrightnessInput(final: false);  
        _brightness.MouseUp += (_, _) => OnBrightnessInput(final: true);  
        Controls.Add(_brightness);
        _brightnessValue.SetBounds(Px(254), Px(400), Px(48), Px(24));     
        _brightnessValue.TextAlign = ContentAlignment.MiddleRight;        
        _brightnessValue.ForeColor = SystemColors.GrayText;
        _brightnessValue.Font = new Font("Microsoft YaHei UI", 8.5f);     
        _brightnessValue.Text = "100%";
        Controls.Add(_brightnessValue);

        _statusLabel.SetBounds(Px(10), Px(432), Px(296), Px(52));
        _statusLabel.TextAlign = ContentAlignment.MiddleCenter;
        _statusLabel.ForeColor = SystemColors.GrayText;
        _statusLabel.Font = new Font("Microsoft YaHei UI", 8.5f);
        _statusLabel.Text = "连接设备中…";
        _statusLabel.AutoEllipsis = true;
        Controls.Add(_statusLabel);

        // Album manual controls: symbol-style buttons between mode buttons
        // and brightness slider, only visible in album mode.
        var symFont = new Font("Segoe UI Symbol", 11f);
        int albumY = Px(374);
        int albumBtnW = Px(288) / 4;
        _albumModeBtn = new Button { Text = "\u23F8", FlatStyle = FlatStyle.Flat, Font = symFont, Visible = false, TextAlign = ContentAlignment.MiddleCenter, UseCompatibleTextRendering = true };
        _albumModeBtn.SetBounds(Px(14), albumY, albumBtnW - Px(2), Px(24));
        _albumModeBtn.Click += (_, _) => AlbumToggleMode();
        Controls.Add(_albumModeBtn);
        _albumPrevBtn = new Button { Text = "\u23EE", FlatStyle = FlatStyle.Flat, Font = symFont, Visible = false, TextAlign = ContentAlignment.MiddleCenter, UseCompatibleTextRendering = true };
        _albumPrevBtn.SetBounds(Px(14) + albumBtnW, albumY, albumBtnW - Px(2), Px(24));
        _albumPrevBtn.Click += (_, _) => AlbumStep(true);
        Controls.Add(_albumPrevBtn);
        _albumNextBtn = new Button { Text = "\u23ED", FlatStyle = FlatStyle.Flat, Font = symFont, Visible = false, TextAlign = ContentAlignment.MiddleCenter, UseCompatibleTextRendering = true };
        _albumNextBtn.SetBounds(Px(14) + albumBtnW * 2, albumY, albumBtnW - Px(2), Px(24));
        _albumNextBtn.Click += (_, _) => AlbumStep(false);
        Controls.Add(_albumNextBtn);
        _albumRandomBtn = new Button { Text = "\u21C4", FlatStyle = FlatStyle.Flat, Font = symFont, Visible = false, TextAlign = ContentAlignment.MiddleCenter, UseCompatibleTextRendering = true };
        _albumRandomBtn.SetBounds(Px(14) + albumBtnW * 3, albumY, albumBtnW - Px(2), Px(24));
        _albumRandomBtn.Click += (_, _) => AlbumRandom();
        Controls.Add(_albumRandomBtn);

        _pollTimer.Tick += async (_, _) =>
        {
            if (_tickInFlight) return;
            _tickInFlight = true;
            try { await Tick(); }
            finally { _tickInFlight = false; }
        };
        _animTimer.Tick += (_, _) => AnimTick();
        _sweepTimer.Tick += (_, _) => SweepTick();
        _spectrumTimer.Tick += (_, _) => SpectrumTick();
        Deactivate += (_, _) => HidePopup(); // transient, like NSPopover
    }

    float ScaleF() => DeviceDpi / 96f;
    int Px(int logical) => (int)Math.Round(logical * ScaleF());

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        using var pen = new Pen(Color.FromArgb(120, 120, 120));
        e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
    }

    public void Toggle()
    {
        if (Visible)
        {
            HidePopup();
            return;
        }
        // anchor to the tray corner of the primary screen, near the cursor
        var area = Screen.FromPoint(Cursor.Position).WorkingArea;
        var x = Math.Min(Math.Max(Cursor.Position.X - Width / 2, area.Left + 8),
                         area.Right - Width - 8);
        var y = area.Bottom - Height - 8;
        Location = new Point(x, y);
        Show();
        Activate();
        _pollTimer.Start();
        _animTimer.Start();
        _sweepTimer.Start();
        _spectrumTimer.Start();
        _ = Tick();
    }

    void HidePopup()
    {
        Hide();
        _pollTimer.Stop();
        _animTimer.Stop();
        _sweepTimer.Stop();
        _spectrumTimer.Stop();
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (e.CloseReason == CloseReason.UserClosing)
        {
            e.Cancel = true;
            HidePopup();
        }
        base.OnFormClosing(e);
    }

    void OnBrightnessInput(bool final)
    {
        var level = _brightness.Value;
        _brightnessValue.Text = $"{level}%";
        _pendingBrightness = level;
        if (!final && (DateTime.Now - _lastBrightnessSentAt).TotalMilliseconds < 250) return;
        FlushBrightness();
    }

    void FlushBrightness()
    {
        if (_pendingBrightness is not int level) return;
        _pendingBrightness = null;
        _lastBrightnessSentAt = DateTime.Now;
        _ = DeviceClient.SetBrightness(level);
    }

    /// Follow the device's reported brightness (changed via its web page or
    /// another client) — but never while the user is mid-adjustment here.
    void SyncBrightness(DeviceInfo info)
    {
        if (_pendingBrightness != null ||
            (DateTime.Now - _lastBrightnessSentAt).TotalSeconds < 2) return;
        var level = Math.Clamp(info.Brightness, 0, 100);
        _brightness.Value = level;
        _brightnessValue.Text = $"{level}%";
    }

    /// Manual album stepping (tray menu): repaint the replica with the
    /// album's current photo right away, without waiting for the next Tick.
    public void RefreshAlbumNow()
    {
        if (!_mirror.AlbumMode || _albumService == null) return;
        var frame = _albumService.NextRgb565();
        _mirror.SceneFrame?.Dispose();
        _mirror.SceneFrame = frame.Length > 0 ? Rgb565.Decode(frame, 0, 128, 128) : null;
        _mirror.Invalidate();
    }

    void AlbumToggleMode()
    {
        if (_albumService == null) return;
        _albumService.ManualMode = !_albumService.ManualMode;
        _albumModeBtn.Text = _albumService.ManualMode ? "\u25B6" : "\u23F8";
        // manual -> auto: timer restarts and continues from current index
        RefreshAlbumNow();
    }

    void AlbumStep(bool prev)
    {
        if (_albumService == null) return;
        _albumService.ManualMode = true;
        _albumModeBtn.Text = "\u25B6";
        var frame = prev ? _albumService.PrevRgb565() : _albumService.NextManualRgb565();
        _mirror.SceneFrame?.Dispose();
        _mirror.SceneFrame = frame.Length > 0 ? Rgb565.Decode(frame, 0, 128, 128) : null;
        _mirror.Invalidate();
    }

    void AlbumRandom()
    {
        if (_albumService == null) return;
        _albumService.ManualMode = true;
        _albumModeBtn.Text = "\u25B6";
        var frame = _albumService.RandomRgb565();
        _mirror.SceneFrame?.Dispose();
        _mirror.SceneFrame = frame.Length > 0 ? Rgb565.Decode(frame, 0, 128, 128) : null;
        _mirror.Invalidate();
    }

    /// One sweep step: push the newest 4Hz sample, refresh the DL/UL readout.
    void SweepTick()
    {
        if (!_mirror.NetMode || !Visible) return;
        var cur = _netMonitor.Current;
        var smoothed = _netMonitor.CurrentSmoothed;
        _mirror.NetHeaderDL = MirrorControl.DeviceSpeedText(smoothed.Rx);
        _mirror.NetHeaderUL = MirrorControl.DeviceSpeedText(smoothed.Tx);
        var (cpu, mem) = SystemStatsMonitor.Snapshot(); // internally 1s-cached
        _mirror.NetCPU = cpu;
        _mirror.NetMem = mem;
        _mirror.PushNetSample(cur.Rx, cur.Tx);
    }

    /// High-frequency spectrum refresh (~10x/s) while the spectrum scene is
    /// showing: pull the live bars from the bridge and repaint the preview,
    /// independent of the 1s /api/info Tick.
    byte[] _smoothedBars = new byte[24];

    void SpectrumTick()
    {
        if (!_mirror.SpectrumMode || !Visible || _spectrumService == null) return;
        // blend toward the target like the firmware's time-smoothing
        // (27%/frame) so the silence fade-out matches the device instead
        // of popping from the last frame straight to zero
        var target = _spectrumService.Bars;
        if (_smoothedBars.Length != target.Length) _smoothedBars = new byte[target.Length];
        for (int i = 0; i < target.Length; i++)
        {
            int diff = target[i] - _smoothedBars[i];
            int step = diff * 27 / 100;
            if (step == 0 && diff != 0) step = diff > 0 ? 1 : -1; // converge fully
            _smoothedBars[i] = (byte)(_smoothedBars[i] + step);
        }
        _mirror.SpectrumBars = _smoothedBars;
        if (_lastInfo != null) SyncSpectrumType(_lastInfo);
        _mirror.Invalidate();
    }

    /// Right-click style switch: apply the new spectrum style to the preview
    /// immediately (the device applies it too via /api/music-spectrum, but
    /// the popup should follow the click without waiting for the 1s Tick).
    public void ApplySpectrumStyle(int type)
    {
        _mirror.SpectrumType = Math.Clamp(type, 0, 2);
        if (_mirror.SpectrumMode) _mirror.Invalidate();
    }

    /// Copy the device's spectrum type+effect+color+params into the preview.
    void SyncSpectrumType(DeviceInfo info)
    {
        _mirror.SpectrumType = Math.Clamp(info.SpectrumType, 0, 2);
        int maxEffect = _mirror.SpectrumType switch { 0 => 8, 1 => 3, _ => 3 };
        _mirror.SpectrumEffect = Math.Clamp(info.SpectrumEffect, 0, maxEffect);
        _mirror.SpectrumColor = Math.Clamp(info.SpectrumColor, 0, 11);
        _mirror.SpectrumColor2 = Math.Clamp(info.SpectrumColor2, 0, 11);
        _mirror.SpectrumRainbow = info.SpectrumRainbow == 1 ? 1 : 0;
        _mirror.SpectrumGap = Math.Clamp(info.SpectrumGap, 0, 2);
        _mirror.SpectrumDecay = Math.Clamp(info.SpectrumDecay, 1, 10);
        _mirror.SpectrumLineW = Math.Clamp(info.SpectrumLineW, 1, 5);
        _mirror.SpectrumFill = info.SpectrumFill == 1 ? 1 : 0;
        _mirror.SpectrumFillColor = Math.Clamp(info.SpectrumFillColor, 0, 7);
        _mirror.SpectrumRingW = Math.Clamp(info.SpectrumRingW, 1, 8);
        _mirror.SpectrumRingGap = Math.Clamp(info.SpectrumRingGap, 0, 10);
        _mirror.SpectrumRingInner = Math.Clamp(info.SpectrumRingInner, 2, 60);
        _mirror.SpectrumRingOuter = Math.Clamp(info.SpectrumRingOuter, 20, 64);
        _mirror.SpectrumRingInColor = Math.Clamp(info.SpectrumRingInColor, 0, 8);
        _mirror.SpectrumRingFill = info.SpectrumRingFill == 1 ? 1 : 0;
        _mirror.SpectrumGradRange = Math.Clamp(info.SpectrumGradRange, 0, 100);
        _mirror.SpectrumGradReverse = info.SpectrumGradReverse == 1 ? 1 : 0;
        _mirror.SpectrumAutoRange = info.SpectrumAutoRange == 1 ? 1 : 0;
        _mirror.SpectrumOffset = Math.Clamp(info.SpectrumOffset, -100, 100);
        _mirror.SpectrumSilence = Math.Clamp(info.SpectrumSilence, 0, 50);
        _mirror.SpectrumMirror = info.SpectrumMirror == 1 ? 1 : 0;
        _mirror.SpectrumDualRing = info.SpectrumDualRing == 1 ? 1 : 0;
        _mirror.SpectrumDualInner = Math.Clamp(info.SpectrumDualInner, 0, 100);
        _mirror.SpectrumDualOuter = Math.Clamp(info.SpectrumDualOuter, 0, 100);
    }

    async Task Tick()
    {
        DeviceInfo info;
        try
        {
            info = await DeviceClient.FetchInfo();
        }
        catch (Exception)
        {
            if (!Visible) return;
            _mirror.DeviceOK = false;
            _mirror.Invalidate();
            _statusLabel.Text = DeviceClient.Host.Length == 0
                ? "未设置设备地址（右键托盘 → 设置设备地址）" : $"无法连接 {DeviceClient.Host}";
            return;
        }
        if (!Visible) return;
        _lastInfo = info;
        _mirror.DeviceOK = true;
        ApplyScene(info);
        EnsureSprite(info);
        SyncBrightness(info);
        var modeIdx = Math.Max(0, Array.IndexOf(Modes, info.Mode));
        _applyingMode = true;
        _modeButtons[modeIdx].Checked = true;
        _applyingMode = false;
        var modeText = info.Mode == "auto" ? "自动切换"
            : info.Mode == "net" ? "网速曲线"
            : info.Mode == "music" ? "音乐播放"
            : info.Mode == "mirror" ? "投屏"
            : info.Mode == "album" ? "相册"
            : info.Mode == "spectrum" ? "音乐频谱" : "固定显示";
        _statusLabel.Text = $"{info.Ip} · {modeText} · 数据 {info.Bridge}";
    }

    /// Quota lines & ring exactly as the firmware computes them from /status.
    void ApplyScene(DeviceInfo info)
    {
        // mirror what's actually on the device screen (effective), so an
        // AUTO device that auto-switched to music shows music here too
        var enteringNet = info.Effective == "net" && !_mirror.NetMode;
        _mirror.NetMode = info.Effective == "net";
        _mirror.MusicMode = info.Effective == "music";
        _mirror.StockMode = info.Effective == "stock";
        _mirror.MirrorMode = info.Effective == "mirror";
        _mirror.AlbumMode = info.Effective == "album";
        _mirror.SpectrumMode = info.Effective == "spectrum";
        bool album = _mirror.AlbumMode;
        _albumModeBtn.Visible = album;
        _albumPrevBtn.Visible = album;
        _albumNextBtn.Visible = album;
        _albumRandomBtn.Visible = album;
        if (album) _albumModeBtn.Text = _albumService?.ManualMode == true ? "\u25B6" : "\u23F8";
        if (_mirror.SpectrumMode)
        {
            // live bars from the bridge's audio capture, same style as the device
            _mirror.SpectrumBars = _spectrumService?.Bars ?? Array.Empty<byte>();
            SyncSpectrumType(info);
            _mirror.Invalidate();
            return;
        }
        if (_mirror.MirrorMode)
        {
            // show the same full-panel frame the device just drew
            var frame = _mirrorService.FrameRgb565;
            _mirror.SceneFrame?.Dispose();
            _mirror.SceneFrame = frame.Length > 0 ? Rgb565.Decode(frame, 0, 128, 128) : null;
            _mirror.Invalidate();
            return;
        }
        if (_mirror.AlbumMode)
        {
            var frame = _albumService.NextRgb565();
            _mirror.SceneFrame?.Dispose();
            _mirror.SceneFrame = frame.Length > 0 ? Rgb565.Decode(frame, 0, 128, 128) : null;
            _mirror.Invalidate();
            return;
        }
        _mirror.SceneFrame?.Dispose();
        _mirror.SceneFrame = null;
        if (_mirror.StockMode)
        {
            _mirror.StockRows = _stockMonitor.Snapshot;
            _mirror.Invalidate();
            return;
        }
        if (_mirror.NetMode)
        {
            if (enteringNet) _mirror.ResetNetSweep(); // fresh sweep, like the device
            _mirror.Invalidate();
            return;
        }
        if (_mirror.MusicMode)
        {
            var s = _nowPlaying.Snapshot;
            _mirror.MusicTitle = s.Title;
            _mirror.MusicArtist = s.Artist;
            _mirror.MusicElapsed = s.Elapsed;
            _mirror.MusicDuration = s.Duration;
            _mirror.MusicPlaying = s.Playing;
            _mirror.MusicCover?.Dispose();
            var cover = _nowPlaying.CoverRgb565;
            _mirror.MusicCover = cover.Length > 0 ? Rgb565.Decode(cover, 0, 128, 128) : null;
            _mirror.Invalidate();
            return;
        }
        var snap = _service.Snapshot();
        _mirror.ShowingClaude = info.Showing != "codex";
        if (_mirror.ShowingClaude)
        {
            var pct = snap.Claude.FiveHourPct
                ?? (snap.Claude.SessionWindowMin > 0
                    ? 100.0 * snap.Claude.SessionMin / snap.Claude.SessionWindowMin : 0);
            _mirror.RingPct = pct;
            _mirror.Line1 = "5h " + PctText(pct);
            _mirror.Line2 = "Weekly " + PctText(snap.Claude.SevenDayPct);
            _mirror.NeedsInput = snap.Claude.NeedsInput;
        }
        else
        {
            // Codex may only have a weekly window now (5h limit dropped):
            // ring + single line follow whatever windows actually exist.
            _mirror.RingPct = snap.Codex.PrimaryPct ?? snap.Codex.WeeklyPct ?? 0;
            if (!snap.Codex.PrimaryPct.HasValue && snap.Codex.WeeklyPct.HasValue)
            {
                _mirror.Line1 = "Weekly " + PctText(snap.Codex.WeeklyPct);
                _mirror.Line2 = "";
            }
            else
            {
                _mirror.Line1 = "5h " + PctText(snap.Codex.PrimaryPct);
                _mirror.Line2 = "Weekly " + PctText(snap.Codex.WeeklyPct);
            }
            _mirror.NeedsInput = snap.Codex.NeedsInput;
        }
        _mirror.Invalidate();
    }

    static string PctText(double? pct) =>
        pct.HasValue && pct.Value >= 0 ? $"{(int)pct.Value}%" : "-";

    void EnsureSprite(DeviceInfo info)
    {
        var slot = info.Showing == "codex" ? "codex" : "claude";
        var w = slot == "claude" ? info.ClaudeW : info.CodexW;
        var h = slot == "claude" ? info.ClaudeH : info.CodexH;
        if (_spriteCache.TryGetValue(slot, out var cached) && cached.Rev == info.SpriteRev)
        {
            _mirror.Frames = cached.Frames;
            _mirror.SpriteW = cached.W;
            _mirror.SpriteH = cached.H;
            return;
        }
        if (_fetchingSlot == slot) return;
        _fetchingSlot = slot;
        _ = FetchSprite(slot, info.SpriteRev, w, h);
    }

    async Task FetchSprite(string slot, int rev, int w, int h)
    {
        try
        {
            var data = await DeviceClient.FetchSpriteRaw(slot);
            var frames = Rgb565.DecodeSpriteFrames(data, w, h);
            if (frames.Count == 0) return;
            if (_spriteCache.TryGetValue(slot, out var old))
                foreach (var f in old.Frames) f.Dispose();
            _spriteCache[slot] = (rev, frames, w, h);
            if ((_lastInfo?.Showing == "codex" ? "codex" : "claude") == slot)
            {
                _mirror.Frames = frames;
                _mirror.SpriteW = w;
                _mirror.SpriteH = h;
                _mirror.Invalidate();
            }
        }
        catch (Exception)
        {
            // device unreachable / mid-upload: next tick retries
        }
        finally
        {
            _fetchingSlot = null;
        }
    }

    int _flashCounter;

    void AnimTick()
    {
        if (_lastInfo == null || _mirror.NetMode) return;

        // ~400ms red-border flash while an approval is pending (device cadence)
        if (_mirror.NeedsInput)
        {
            _flashCounter++;
            if (_flashCounter >= 3) // 3 * 0.12s ≈ 0.36s
            {
                _flashCounter = 0;
                _mirror.FlashOn = !_mirror.FlashOn;
                _mirror.Invalidate();
            }
        }
        else if (_mirror.FlashOn)
        {
            _mirror.FlashOn = false;
            _mirror.Invalidate();
        }

        if (_mirror.Frames.Count == 0) return;
        var snap = _service.Snapshot();
        var working = _lastInfo.Showing == "codex"
            ? snap.Codex.Status == "working" : snap.Claude.Status == "working";
        if (working)
        {
            _mirror.FrameIdx = (_mirror.FrameIdx + 1) % _mirror.Frames.Count;
        }
        else if (_mirror.FrameIdx != 0)
        {
            _mirror.FrameIdx = 0;
        }
        _mirror.Invalidate();
    }

    async void ModeChanged(object sender, EventArgs e)
    {
        if (_applyingMode || sender is not RadioButton { Checked: true, Tag: string mode }) return;
        try
        {
            await DeviceClient.SetDisplayMode(mode);
        }
        catch (Exception)
        {
            // Tick() below re-syncs the buttons to the device's real state
        }
        await Tick();
    }
}
