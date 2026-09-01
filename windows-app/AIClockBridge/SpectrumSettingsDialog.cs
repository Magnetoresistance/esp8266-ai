using System.Drawing;

namespace AIClockBridge;

// Modal dialog configuring the music spectrum — opens like the mirror
// settings window. Two-column layout: the left column holds the common
// settings (type / effect / color / smooth / width), the right column holds
// the type-specific settings that show/hide dynamically:
//   combo styles → 辅助色
//   bars type    → peak / gap / decay
//   wave type    → linew / fill
//   radial type  → ringw / ringgap / ringinner / ringouter
// Each row is an AutoSize row, so hiding a row collapses it to zero height
// and nothing is ever clipped (a single long column previously overflowed).
// Reads the device's current settings first, then every change POSTs to
// /api/music-spectrum live (all params persist on the device).
sealed class SpectrumSettingsDialog
{
    readonly ComboBox _typeBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly ComboBox _effectBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly ComboBox _colorBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly ComboBox _color2Box = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly CheckBox _peakCk = new() { Text = "峰值保持点", Checked = true };
    readonly CheckBox _fillCk = new() { Text = "波形填充", Checked = false };
    readonly CheckBox _ringFillCk = new() { Text = "环形折线填充", Checked = true };
    readonly ComboBox _fillColorBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly ComboBox _ringInColorBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly NumericUpDown _gradRange = new() { Minimum = 0, Maximum = 100, Value = 100 };
    readonly CheckBox _gradReverseCk = new() { Text = "倒序渐变", Checked = false };
    readonly CheckBox _autoRangeCk = new() { Text = "实时动态范围（min/max 归一化）", Checked = false };
    readonly NumericUpDown _offset = new() { Minimum = -100, Maximum = 100, Value = 0 };
    readonly NumericUpDown _silence = new() { Minimum = 0, Maximum = 50, Value = 6 };
    readonly CheckBox _mirrorCk = new() { Text = "上下镜像（中心横轴）", Checked = false };
    readonly CheckBox _dualRingCk = new() { Text = "双环（内外双圈切片）", Checked = false };
    readonly NumericUpDown _dualIn = new() { Minimum = 0, Maximum = 100, Value = 100 };
    readonly NumericUpDown _dualOut = new() { Minimum = 0, Maximum = 100, Value = 100 };
    readonly NumericUpDown _smooth = new() { Minimum = 0, Maximum = 10, Value = 3 };
    readonly NumericUpDown _width = new() { Minimum = 1, Maximum = 5, Value = 3 };
    readonly NumericUpDown _gap = new() { Minimum = 0, Maximum = 4, Value = 1 };
    readonly NumericUpDown _decay = new() { Minimum = 1, Maximum = 20, Value = 5 };
    readonly NumericUpDown _linew = new() { Minimum = 1, Maximum = 5, Value = 1 };
    readonly NumericUpDown _ringw = new() { Minimum = 1, Maximum = 8, Value = 2 };
    readonly NumericUpDown _ringgap = new() { Minimum = 0, Maximum = 10, Value = 2 };
    readonly NumericUpDown _ringinner = new() { Minimum = 2, Maximum = 60, Value = 12 };
    readonly NumericUpDown _ringouter = new() { Minimum = 20, Maximum = 64, Value = 58 };

    // right-column controls (type-specific) and their labels, for show/hide
    readonly (Label Label, Control Ctrl)[] _specificRows;

    static readonly string[] TypeLabels = { "柱状", "波形", "放射" };
    static readonly string[][] EffectLabels =
    {
        new[] { "经典", "镜像", "峰值保持", "双子", "点阵", "发光", "火焰", "柱+波形", "星光" },
        new[] { "折线", "镜像", "极光", "波形+柱" },
        new[] { "环形", "双环", "环形折线", "扇形" },
    };
    static readonly string[] ColorLabels =
    {
        "绿色", "青色", "黄色", "橙色", "红色", "品红", "白色", "黄绿",
        "横向彩虹带", "色谱列（随幅值渐变）", "纵向彩虹带", "色谱行（幅值→上限）",
    };
    static readonly string[] PlainColorLabels =
    {
        "绿色", "青色", "黄色", "橙色", "红色", "品红", "白色", "黄绿", "黑色", "关闭",
    };

    public SpectrumSettingsDialog()
    {
        var peakL = new Label { Text = "峰值细节", AutoSize = true };
        var gapL = new Label { Text = "柱间距（0-4）", AutoSize = true };
        var decayL = new Label { Text = "峰值衰减（1-20）", AutoSize = true };
        var autoRangeL = new Label { Text = "实时动态范围", AutoSize = true };
        var offsetL = new Label { Text = "幅值偏移（-100~100）", AutoSize = true };
        var linewL = new Label { Text = "线宽（1-5）", AutoSize = true };
        var fillL = new Label { Text = "波形填充", AutoSize = true };
        var fillColL = new Label { Text = "填充色", AutoSize = true };
        var ringwL = new Label { Text = "环形线宽（1-8）", AutoSize = true };
        var ringgapL = new Label { Text = "环形间距（0-10）", AutoSize = true };
        var ringinL = new Label { Text = "内圆半径（2-60）", AutoSize = true };
        var ringoutL = new Label { Text = "外圆半径（20-64）", AutoSize = true };
        var ringInColL = new Label { Text = "内圆环色", AutoSize = true };
        var ringFillL = new Label { Text = "环形折线填充", AutoSize = true };
        var color2L = new Label { Text = "辅助色（组合样式）", AutoSize = true };
        _specificRows = new (Label, Control)[]
        {
            (color2L, _color2Box),     // combo styles only
            (peakL, _peakCk),          // bars
            (gapL, _gap),              // bars
            (decayL, _decay),          // bars
            (fillL, _fillCk),          // wave
            (fillColL, _fillColorBox), // wave
            (linewL, _linew),          // wave
            (ringwL, _ringw),          // radial
            (ringgapL, _ringgap),      // radial
            (ringinL, _ringinner),     // radial
            (ringoutL, _ringouter),    // radial
            (ringInColL, _ringInColorBox), // radial
            (ringFillL, _ringFillCk),  // radial
        };
        // 色谱行 (color 11) only: gradient range + direction
        _gradRangeL = new Label { Text = "渐变范围（0-100%）", AutoSize = true };
        _gradReverseL = new Label { Text = "渐变顺序", AutoSize = true };
    }

    Label _gradRangeL, _gradReverseL, _mirrorLabel, _dualRingLabel, _dualInLabel, _dualOutLabel;

    /// Build a 2-column param table (label + control), one AutoSize row per
    /// param. Hiding a control collapses its row to zero height.
    static TableLayoutPanel MakeTable()
    {
        var t = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 2,
            RowCount = 0,
            Dock = DockStyle.Fill,
        };
        // NOTE: never set ColumnStyles in the object initializer — ColumnCount
        // already appends default styles, so the initializer *adds* two more
        // (4 styles for 2 columns) and the control column gets zero width.
        t.ColumnStyles.Clear();
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 130));
        t.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        return t;
    }

    static Label AddRow(TableLayoutPanel t, string label, Control control)
    {
        t.RowCount++;
        t.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        var l = new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left };
        l.Margin = new Padding(3, 8, 6, 4);
        control.Margin = new Padding(3, 4, 3, 4);
        t.Controls.Add(l, 0, t.RowCount - 1);
        t.Controls.Add(control, 1, t.RowCount - 1);
        return l;
    }

    /// Show the dialog. Every change is POSTed to the device immediately so
    /// the panel responds live; nothing extra to "save". The device's current
    /// settings are fetched asynchronously — never block the UI thread
    /// waiting on HTTP (a synchronous wait deadlocks because the await
    /// continuation needs the UI SynchronizationContext).
    public async void Show()
    {
        var form = new Form
        {
            Text = "音乐频谱设置",
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterScreen,
            MinimizeBox = false,
            MaximizeBox = false,
            ShowInTaskbar = false,
            Font = new Font("Microsoft YaHei UI", 9f),
            ClientSize = new Size(700, 680),
            AutoScroll = true, // safety net if rows ever exceed the height
            TopMost = true,
        };

        // default values shown immediately; replaced when the device answers
        var cur = new DeviceInfo
        {
            SpectrumType = 0, SpectrumEffect = 0, SpectrumColor = 0,
            SpectrumPeak = 1, SpectrumSmooth = 3, SpectrumWidth = 3,
        };
        try
        {
            cur = await DeviceClient.FetchInfo();
        }
        catch
        {
            // device unreachable: keep defaults
        }

        // ---- left column: common settings (always visible) ----
        var left = MakeTable();
        foreach (var l in TypeLabels) _typeBox.Items.Add(l);
        _typeBox.SelectedIndex = Math.Clamp(cur.SpectrumType, 0, 2);
        AddRow(left, "类型", _typeBox);
        AddRow(left, "效果", _effectBox);
        foreach (var l in ColorLabels) _colorBox.Items.Add(l);
        _colorBox.SelectedIndex = Math.Clamp(cur.SpectrumColor, 0, 11);
        AddRow(left, "颜色", _colorBox);
        _smooth.Value = Math.Clamp(cur.SpectrumSmooth, 0, 10);
        AddRow(left, "平滑度（0-10）", _smooth);
        _width.Value = Math.Clamp(cur.SpectrumWidth, 1, 5);
        AddRow(left, "柱宽（1-5）", _width);
        // dynamic range / offset / silence apply to ALL types (bars/wave/radial)
        _autoRangeCk.Checked = cur.SpectrumAutoRange == 1;
        AddRow(left, "实时动态范围", _autoRangeCk);
        _offset.Value = Math.Clamp(cur.SpectrumOffset, -100, 100);
        AddRow(left, "幅值偏移（-100~100）", _offset);
        _silence.Value = Math.Clamp(cur.SpectrumSilence, 0, 50);
        AddRow(left, "静音阈值（0-50）", _silence);
        // vertical mirror: bars & wave only (radial is naturally symmetric)
        _mirrorCk.Checked = cur.SpectrumMirror == 1;
        _mirrorLabel = AddRow(left, "上下镜像", _mirrorCk);
        // dual-ring: 环形/扇形 radial styles only
        _dualRingCk.Checked = cur.SpectrumDualRing == 1;
        _dualRingLabel = AddRow(left, "双环切片", _dualRingCk);
        _dualIn.Value = Math.Clamp(cur.SpectrumDualInner, 0, 100);
        _dualInLabel = AddRow(left, "内环幅度（0-100%）", _dualIn);
        _dualOut.Value = Math.Clamp(cur.SpectrumDualOuter, 0, 100);
        _dualOutLabel = AddRow(left, "外环增量（0-100%）", _dualOut);

        // ---- right column: type-specific settings (dynamic) ----
        var right = MakeTable();
        foreach (var l in ColorLabels) _color2Box.Items.Add(l);
        _color2Box.SelectedIndex = Math.Clamp(cur.SpectrumColor2, 0, 11);
        foreach (var l in ColorLabels) _fillColorBox.Items.Add(l);
        _fillColorBox.SelectedIndex = Math.Clamp(cur.SpectrumFillColor, 0, 11);
        foreach (var l in PlainColorLabels) _ringInColorBox.Items.Add(l);
        _ringInColorBox.SelectedIndex = Math.Clamp(cur.SpectrumRingInColor, 0, 9);
        _peakCk.Checked = cur.SpectrumPeak == 1;
        _gap.Value = Math.Clamp(cur.SpectrumGap, 0, 4);
        _decay.Value = Math.Clamp(cur.SpectrumDecay, 1, 20);
        _linew.Value = Math.Clamp(cur.SpectrumLineW, 1, 5);
        _fillCk.Checked = cur.SpectrumFill == 1;
        _ringw.Value = Math.Clamp(cur.SpectrumRingW, 1, 8);
        _ringgap.Value = Math.Clamp(cur.SpectrumRingGap, 0, 10);
        _ringinner.Value = Math.Clamp(cur.SpectrumRingInner, 2, 60);
        _ringouter.Value = Math.Clamp(cur.SpectrumRingOuter, 20, 64);
        _ringFillCk.Checked = cur.SpectrumRingFill == 1;
        _autoRangeCk.Checked = cur.SpectrumAutoRange == 1;
        _offset.Value = Math.Clamp(cur.SpectrumOffset, -100, 100);
        foreach (var (label, ctrl) in _specificRows)
        {
            right.RowCount++;
            right.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            label.Margin = new Padding(3, 8, 6, 4);
            ctrl.Margin = new Padding(3, 4, 3, 4);
            right.Controls.Add(label, 0, right.RowCount - 1);
            right.Controls.Add(ctrl, 1, right.RowCount - 1);
        }
        // 色谱行 (color 11) only: gradient range + direction
        _gradRange.Value = Math.Clamp(cur.SpectrumGradRange, 0, 100);
        _gradReverseCk.Checked = cur.SpectrumGradReverse == 1;
        _gradRangeL = AddRow(right, "渐变范围（0-100%）", _gradRange);
        _gradReverseL = AddRow(right, "渐变顺序", _gradReverseCk);

        // ---- snapshot / restore ----
        var snapshot = string.Join("&", new[]
        {
            $"type={cur.SpectrumType}",
            $"effect={cur.SpectrumEffect}",
            $"color={cur.SpectrumColor}",
            $"color2={cur.SpectrumColor2}",
            $"peak={cur.SpectrumPeak}",
            $"smooth={cur.SpectrumSmooth}",
            $"width={cur.SpectrumWidth}",
            $"gap={cur.SpectrumGap}",
            $"decay={cur.SpectrumDecay}",
            $"linew={cur.SpectrumLineW}",
            $"fill={cur.SpectrumFill}",
            $"fillcolor={cur.SpectrumFillColor}",
            $"ringw={cur.SpectrumRingW}",
            $"ringgap={cur.SpectrumRingGap}",
            $"ringinner={cur.SpectrumRingInner}",
            $"ringouter={cur.SpectrumRingOuter}",
            $"ringincolor={cur.SpectrumRingInColor}",
            $"ringfill={cur.SpectrumRingFill}",
            $"gradrange={cur.SpectrumGradRange}",
            $"gradreverse={cur.SpectrumGradReverse}",
            $"autorange={cur.SpectrumAutoRange}",
            $"offset={cur.SpectrumOffset}",
            $"silence={cur.SpectrumSilence}",
            $"mirror={cur.SpectrumMirror}",
            $"dualring={cur.SpectrumDualRing}",
            $"dualin={cur.SpectrumDualInner}",
            $"dualout={cur.SpectrumDualOuter}",
        });

        var save = new Button { Text = "保存", Width = 90 };
        var cancel = new Button { Text = "取消", Width = 90 };
        save.Click += (_, _) => form.Close(); // already applied live + persisted
        cancel.Click += (_, _) =>
        {
            _ = DeviceClient.SetMusicSpectrum(snapshot); // restore opening state
            form.Close();
        };
        var buttons = new FlowLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            FlowDirection = FlowDirection.RightToLeft,
            Anchor = AnchorStyles.Right,
        };
        buttons.Controls.Add(cancel);
        buttons.Controls.Add(save);
        form.CancelButton = cancel; // Esc = 取消 (restores snapshot)

        // ---- master 2-col layout: [left | right] + buttons spanning ----
        var master = new TableLayoutPanel
        {
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 2,
            RowCount = 2,
            Dock = DockStyle.Fill,
            Padding = new Padding(10),
        };
        master.ColumnStyles.Clear();
        master.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 330));
        master.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 330));
        master.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        master.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        master.Controls.Add(left, 0, 0);
        master.Controls.Add(right, 1, 0);
        master.Controls.Add(buttons, 1, 1); // buttons bottom-right

        // ---- dynamic effect list per type + show/hide specific rows ----
        void FillEffectList()
        {
            int t = _typeBox.SelectedIndex;
            _effectBox.Items.Clear();
            foreach (var l in EffectLabels[t]) _effectBox.Items.Add(l);
            _effectBox.SelectedIndex = Math.Clamp(cur.SpectrumEffect, 0, EffectLabels[t].Length - 1);
        }
        void UpdateTypeRows()
        {
            int t = _typeBox.SelectedIndex;
            bool bars = t == 0;
            bool wave = t == 1;
            bool radial = t == 2;
            bool combo = (t == 0 && _effectBox.SelectedIndex == 7)
                      || (t == 1 && _effectBox.SelectedIndex == 3);
            bool color11 = _colorBox.SelectedIndex == 11; // 色谱行
            // _specificRows: 0=color2, 1..3=bars, 4..6=wave, 7..12=radial
            _specificRows[0].Label.Visible = combo;
            _specificRows[0].Ctrl.Visible = combo;
            for (int i = 1; i <= 3; i++) _specificRows[i].Label.Visible = bars;
            for (int i = 1; i <= 3; i++) _specificRows[i].Ctrl.Visible = bars;
            for (int i = 4; i <= 6; i++) _specificRows[i].Label.Visible = wave;
            for (int i = 4; i <= 6; i++) _specificRows[i].Ctrl.Visible = wave;
            for (int i = 7; i <= 12; i++) _specificRows[i].Label.Visible = radial;
            for (int i = 7; i <= 12; i++) _specificRows[i].Ctrl.Visible = radial;
            // 色谱行 gradient tuning rows (color 11 only)
            _gradRangeL.Visible = color11;
            _gradRange.Visible = color11;
            _gradReverseL.Visible = color11;
            _gradReverseCk.Visible = color11;
            // vertical mirror: bars & wave only (radial is symmetric already)
            bool mirrorable = t == 0 || t == 1;
            _mirrorLabel.Visible = mirrorable;
            _mirrorCk.Visible = mirrorable;
            // dual-ring: 环形/扇形 radial styles only
            bool fanRing = t == 2 && (_effectBox.SelectedIndex == 0 || _effectBox.SelectedIndex == 3);
            bool dualOn = fanRing && _dualRingCk.Checked;
            _dualRingLabel.Visible = fanRing;
            _dualRingCk.Visible = fanRing;
            // inner/outer tuning only meaningful while dual-ring is enabled
            _dualInLabel.Visible = dualOn;
            _dualIn.Visible = dualOn;
            _dualOutLabel.Visible = dualOn;
            _dualOut.Visible = dualOn;
        }
        FillEffectList();
        UpdateTypeRows();

        // live-apply: every control change pushes to the device
        void Push()
        {
            var q = new List<string>();
            if (_typeBox.SelectedIndex >= 0) q.Add($"type={_typeBox.SelectedIndex}");
            if (_effectBox.SelectedIndex >= 0) q.Add($"effect={_effectBox.SelectedIndex}");
            if (_colorBox.SelectedIndex >= 0) q.Add($"color={_colorBox.SelectedIndex}");
            if (_color2Box.SelectedIndex >= 0) q.Add($"color2={_color2Box.SelectedIndex}");
            q.Add($"peak={( _peakCk.Checked ? 1 : 0)}");
            q.Add($"smooth={_smooth.Value}");
            q.Add($"width={_width.Value}");
            q.Add($"gap={_gap.Value}");
            q.Add($"decay={_decay.Value}");
            q.Add($"linew={_linew.Value}");
            q.Add($"fill={( _fillCk.Checked ? 1 : 0)}");
            if (_fillColorBox.SelectedIndex >= 0) q.Add($"fillcolor={_fillColorBox.SelectedIndex}");
            q.Add($"ringw={_ringw.Value}");
            q.Add($"ringgap={_ringgap.Value}");
            q.Add($"ringinner={_ringinner.Value}");
            q.Add($"ringouter={_ringouter.Value}");
            if (_ringInColorBox.SelectedIndex >= 0) q.Add($"ringincolor={_ringInColorBox.SelectedIndex}");
            q.Add($"ringfill={( _ringFillCk.Checked ? 1 : 0)}");
            q.Add($"gradrange={_gradRange.Value}");
            q.Add($"gradreverse={( _gradReverseCk.Checked ? 1 : 0)}");
            q.Add($"autorange={( _autoRangeCk.Checked ? 1 : 0)}");
            q.Add($"offset={_offset.Value}");
            q.Add($"silence={_silence.Value}");
            q.Add($"mirror={( _mirrorCk.Checked ? 1 : 0)}");
            q.Add($"dualring={( _dualRingCk.Checked ? 1 : 0)}");
            q.Add($"dualin={_dualIn.Value}");
            q.Add($"dualout={_dualOut.Value}");
            _ = DeviceClient.SetMusicSpectrum(string.Join("&", q));
        }
        _typeBox.SelectedIndexChanged += (_, _) => { FillEffectList(); UpdateTypeRows(); Push(); };
        _effectBox.SelectedIndexChanged += (_, _) => { UpdateTypeRows(); Push(); };
        _colorBox.SelectedIndexChanged += (_, _) => { UpdateTypeRows(); Push(); };
        _color2Box.SelectedIndexChanged += (_, _) => Push();
        _peakCk.CheckedChanged += (_, _) => Push();
        _fillCk.CheckedChanged += (_, _) => Push();
        _fillColorBox.SelectedIndexChanged += (_, _) => Push();
        _ringInColorBox.SelectedIndexChanged += (_, _) => Push();
        _ringFillCk.CheckedChanged += (_, _) => Push();
        _gradRange.ValueChanged += (_, _) => Push();
        _gradReverseCk.CheckedChanged += (_, _) => Push();
        _autoRangeCk.CheckedChanged += (_, _) => Push();
        _offset.ValueChanged += (_, _) => Push();
        _silence.ValueChanged += (_, _) => Push();
        _mirrorCk.CheckedChanged += (_, _) => Push();
        _dualRingCk.CheckedChanged += (_, _) => { UpdateTypeRows(); Push(); };
        _dualIn.ValueChanged += (_, _) => Push();
        _dualOut.ValueChanged += (_, _) => Push();
        _smooth.ValueChanged += (_, _) => Push();
        _width.ValueChanged += (_, _) => Push();
        _gap.ValueChanged += (_, _) => Push();
        _decay.ValueChanged += (_, _) => Push();
        _linew.ValueChanged += (_, _) => Push();
        _ringw.ValueChanged += (_, _) => Push();
        _ringgap.ValueChanged += (_, _) => Push();
        _ringinner.ValueChanged += (_, _) => Push();
        _ringouter.ValueChanged += (_, _) => Push();

        form.Controls.Add(master);
        form.AcceptButton = save;
        form.ShowDialog();
        form.Dispose();
    }
}
