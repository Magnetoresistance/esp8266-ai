using System.Drawing;

namespace AIClockBridge;

// Modal dialog that configures what the screen mirror captures: the full
// primary screen, a specific visible top-level window, or a custom screen
// region. Every control change is applied to the ScreenMirrorService
// *immediately* (no persist) so the user sees the effect right away; 保存
// persists, 取消 restores the snapshot taken when the dialog opened.
sealed class MirrorSettingsDialog
{
    readonly ScreenMirrorService _mirror;

    readonly ComboBox _windowBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly Button _pickBtn = new() { Text = "鼠标选择…" };
    readonly NumericUpDown _x = new() { Minimum = -4096, Maximum = 4096 };
    readonly NumericUpDown _y = new() { Minimum = -4096, Maximum = 4096 };
    readonly NumericUpDown _w = new() { Minimum = 1, Maximum = 4096, Value = 128 };
    readonly NumericUpDown _h = new() { Minimum = 1, Maximum = 4096, Value = 128 };
    readonly RadioButton _rbFull = new() { Text = "全屏", Checked = true };
    readonly RadioButton _rbWindow = new() { Text = "指定窗口" };
    readonly RadioButton _rbRegion = new() { Text = "自定义区域" };
    readonly RadioButton _rbFill = new() { Text = "填充屏幕", Checked = true };
    readonly RadioButton _rbFit = new() { Text = "原比例显示" };
    readonly ComboBox _alignBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    static readonly string[] AlignLabels = { "居中", "靠上", "靠下", "靠左", "靠右" };
    readonly List<(IntPtr Hwnd, string Title)> _windows = new();

    // snapshot of the config as it was when the dialog opened — restored on 取消
    ScreenMirrorService.CaptureTarget _snapTarget;
    IntPtr _snapHwnd;
    Rectangle _snapRegion;
    ScreenMirrorService.FitMode _snapFit;
    ScreenMirrorService.Align _snapAlign;

    public MirrorSettingsDialog(ScreenMirrorService mirror)
    {
        _mirror = mirror;
        _windows = ScreenMirrorService.EnumerateWindows();
    }

    /// Read the live UI state into the mirror service (no persist).
    void ApplyUiToMirror()
    {
        if (_rbFull.Checked)
        {
            _mirror.Target = ScreenMirrorService.CaptureTarget.FullScreen;
        }
        else if (_rbWindow.Checked && _windowBox.SelectedIndex >= 0
                 && _windowBox.SelectedIndex < _windows.Count)
        {
            _mirror.Target = ScreenMirrorService.CaptureTarget.Window;
            _mirror.WindowHwnd = _windows[_windowBox.SelectedIndex].Hwnd;
        }
        else if (_rbRegion.Checked)
        {
            _mirror.Target = ScreenMirrorService.CaptureTarget.Region;
            _mirror.Region = new Rectangle((int)_x.Value, (int)_y.Value,
                                           (int)_w.Value, (int)_h.Value);
        }
        _mirror.Fit = _rbFill.Checked ? ScreenMirrorService.FitMode.Fill
                                       : ScreenMirrorService.FitMode.Fit;
        _mirror.Alignment = (ScreenMirrorService.Align)_alignBox.SelectedIndex;
    }

    /// Restore the mirror service to the snapshot taken when the dialog
    /// opened (i.e. the last persisted config) — used on 取消.
    void RestoreSnapshot()
    {
        _mirror.Target = _snapTarget;
        _mirror.WindowHwnd = _snapHwnd;
        _mirror.Region = _snapRegion;
        _mirror.Fit = _snapFit;
        _mirror.Alignment = _snapAlign;
    }

    public bool Show()
    {
        using var form = new Form
        {
            Text = "投屏设置",
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterScreen,
            MinimizeBox = false,
            MaximizeBox = false,
            ShowInTaskbar = false,
            Font = new Font("Microsoft YaHei UI", 9f),
            ClientSize = new Size(420, 300),
            TopMost = true,
        };

        // mode radios
        _rbFull.SetBounds(16, 14, 120, 24);
        _rbWindow.SetBounds(16, 44, 120, 24);
        _rbRegion.SetBounds(16, 74, 120, 24);
        // window picker: dropdown + one unified mouse-pick button.
        // Clicking it opens the RegionPickerForm overlay, which both
        // highlights a hovered window (click = pick that window) and
        // rubber-bands a dragged region — one button, two gestures.
        _windowBox.SetBounds(140, 44, 150, 24);
        foreach (var w in _windows)
            _windowBox.Items.Add(w.Title);
        _windowBox.SelectedIndex = 0;
        _pickBtn.SetBounds(294, 44, 112, 24);
        _pickBtn.Click += (_, _) => PickByMouse();
        // region fields
        var lblX = new Label { Text = "X", TextAlign = ContentAlignment.MiddleRight };
        var lblY = new Label { Text = "Y", TextAlign = ContentAlignment.MiddleRight };
        var lblW = new Label { Text = "宽", TextAlign = ContentAlignment.MiddleRight };
        var lblH = new Label { Text = "高", TextAlign = ContentAlignment.MiddleRight };
        lblX.SetBounds(140, 74, 30, 22);
        _x.SetBounds(174, 74, 52, 22);
        lblY.SetBounds(232, 74, 24, 22);
        _y.SetBounds(260, 74, 52, 22);
        lblW.SetBounds(140, 100, 30, 22);
        _w.SetBounds(174, 100, 52, 22);
        lblH.SetBounds(232, 100, 24, 22);
        _h.SetBounds(260, 100, 52, 22);
        var hint = new Label
        {
            Text = "改动即时生效；保存后永久，取消恢复上次保存",
            ForeColor = SystemColors.GrayText,
            AutoSize = true,
        };
        hint.SetBounds(140, 128, 260, 18);

        // frame placement: fill vs fit, plus alignment for fit.
        var fitGroup = new GroupBox { Text = "画面显示方式", Font = form.Font };
        fitGroup.SetBounds(10, 180, 400, 60);
        _rbFill.Checked = _mirror.Fit == ScreenMirrorService.FitMode.Fill;
        _rbFill.Parent = fitGroup;
        _rbFill.SetBounds(14, 26, 100, 24);
        _rbFit.Checked = _mirror.Fit == ScreenMirrorService.FitMode.Fit;
        _rbFit.Parent = fitGroup;
        _rbFit.SetBounds(120, 26, 110, 24);
        var alignLabel = new Label { Text = "对齐", AutoSize = true, Parent = fitGroup };
        alignLabel.SetBounds(236, 26, 40, 24);
        foreach (var lbl in AlignLabels) _alignBox.Items.Add(lbl);
        _alignBox.SelectedIndex = (int)_mirror.Alignment;
        _alignBox.Parent = fitGroup;
        _alignBox.SetBounds(278, 26, 100, 24);

        // restore current selection into the controls
        switch (_mirror.Target)
        {
            case ScreenMirrorService.CaptureTarget.Window:
                _rbWindow.Checked = true;
                var idx = _windows.FindIndex(w => w.Hwnd == _mirror.WindowHwnd);
                if (idx >= 0) _windowBox.SelectedIndex = idx;
                break;
            case ScreenMirrorService.CaptureTarget.Region:
                _rbRegion.Checked = true;
                _x.Value = _mirror.Region.X;
                _y.Value = _mirror.Region.Y;
                _w.Value = _mirror.Region.Width;
                _h.Value = _mirror.Region.Height;
                break;
            default:
                _rbFull.Checked = true;
                break;
        }

        // snapshot BEFORE wiring live-apply events: 取消 restores this
        _snapTarget = _mirror.Target;
        _snapHwnd = _mirror.WindowHwnd;
        _snapRegion = _mirror.Region;
        _snapFit = _mirror.Fit;
        _snapAlign = _mirror.Alignment;

        // every control change applies to the mirror immediately (no save)
        _rbFull.CheckedChanged += (_, _) => ApplyUiToMirror();
        _rbWindow.CheckedChanged += (_, _) => ApplyUiToMirror();
        _rbRegion.CheckedChanged += (_, _) => ApplyUiToMirror();
        _windowBox.SelectedIndexChanged += (_, _) => ApplyUiToMirror();
        _x.ValueChanged += (_, _) => ApplyUiToMirror();
        _y.ValueChanged += (_, _) => ApplyUiToMirror();
        _w.ValueChanged += (_, _) => ApplyUiToMirror();
        _h.ValueChanged += (_, _) => ApplyUiToMirror();
        _rbFill.CheckedChanged += (_, _) => ApplyUiToMirror();
        _rbFit.CheckedChanged += (_, _) => ApplyUiToMirror();
        _alignBox.SelectedIndexChanged += (_, _) => ApplyUiToMirror();

        var ok = new Button { Text = "保存", DialogResult = DialogResult.OK };
        ok.SetBounds(230, 252, 80, 28);
        var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel };
        cancel.SetBounds(322, 252, 80, 28);
        form.Controls.AddRange(new Control[]
        {
            _rbFull, _rbWindow, _rbRegion, _windowBox, _pickBtn,
            lblX, _x, lblY, _y, lblW, _w, lblH, _h, hint,
            fitGroup, ok, cancel,
        });
        form.AcceptButton = ok;
        form.CancelButton = cancel;

        var result = form.ShowDialog();

        if (result == DialogResult.OK)
        {
            ApplyUiToMirror(); // ensure the live state matches the controls
            _mirror.Save();    // persist so settings survive a restart
            Console.Error.WriteLine($"[mirror] saved target = {_mirror.Target}");
            return true;
        }
        // 取消 (or Esc / window close): undo any live-apply changes
        RestoreSnapshot();
        Console.Error.WriteLine($"[mirror] cancelled, restored target = {_mirror.Target}");
        return false;
    }

    /// Unified mouse picker: opens the RegionPickerForm overlay where a
    /// hovered window is highlighted and a click selects it (window target),
    /// while a drag rubber-bands a region (region target). Only the UI
    /// controls are updated here; the live-apply events push the change to
    /// the mirror immediately, and 保存/取消 decide persistence.
    void PickByMouse()
    {
        using var picker = new RegionPickerForm();
        if (picker.ShowDialog() != DialogResult.OK) return;

        if (picker.WindowHwnd != IntPtr.Zero)
        {
            var hwnd = picker.WindowHwnd;
            // keep the combo in sync (append frameless windows not in the list)
            int idx = _windows.FindIndex(w => w.Hwnd == hwnd);
            if (idx < 0)
            {
                string title = ScreenMirrorService.WindowTitleOf(hwnd);
                _windows.Add((Hwnd: hwnd, Title: title.Length > 0 ? title : $"窗口 0x{hwnd.ToInt64():X}"));
                _windowBox.Items.Add(_windows[^1].Title);
                idx = _windows.Count - 1;
            }
            _windowBox.SelectedIndex = idx; // triggers live-apply
            _rbWindow.Checked = true;       // triggers live-apply
        }
        else if (!picker.Region.IsEmpty)
        {
            _x.Value = picker.Region.X;
            _y.Value = picker.Region.Y;
            _w.Value = picker.Region.Width;
            _h.Value = picker.Region.Height;
            _rbRegion.Checked = true;       // triggers live-apply
        }
    }
}
