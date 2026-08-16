using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace AIClockBridge;

// Full-screen overlay for picking what the screen mirror should capture:
//   - Drag with the left button: rubber-band select a screen region. On
//     release, Region is set and DialogResult = OK.
//   - Single click (no drag): pick the top-level window under the cursor
//     (highlighted live as you hover). On click, WindowHwnd is set and
//     DialogResult = OK.
//   - Esc / right-click cancels (DialogResult = Cancel).
sealed class RegionPickerForm : Form
{
    public new Rectangle Region { get; private set; } = Rectangle.Empty;
    public IntPtr WindowHwnd { get; private set; } = IntPtr.Zero;

    Point _start, _current;
    bool _dragging;
    IntPtr _hoverWindow = IntPtr.Zero;
    Rectangle _hoverRect;
    readonly Color _maskColor = Color.FromArgb(90, 0, 0, 0);

    public RegionPickerForm()
    {
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.Manual;
        ShowInTaskbar = false;
        TopMost = true;
        Cursor = Cursors.Cross;
        DoubleBuffered = true;
        BackColor = Color.Black;
        // cover the virtual desktop (all monitors)
        var bounds = VirtualScreenBounds();
        Location = bounds.Location;
        Size = bounds.Size;
        Opacity = 0.25; // dim the screen underneath
        KeyPreview = true;
    }

    static Rectangle VirtualScreenBounds()
    {
        var left = System.Windows.Forms.Screen.AllScreens.Min(s => s.Bounds.Left);
        var top = System.Windows.Forms.Screen.AllScreens.Min(s => s.Bounds.Top);
        var right = System.Windows.Forms.Screen.AllScreens.Max(s => s.Bounds.Right);
        var bottom = System.Windows.Forms.Screen.AllScreens.Max(s => s.Bounds.Bottom);
        return new Rectangle(left, top, right - left, bottom - top);
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        if (e.Button == MouseButtons.Left)
        {
            _start = PointToScreen(e.Location);
            _current = _start;
            _dragging = true;
            Invalidate();
        }
        else if (e.Button == MouseButtons.Right)
        {
            DialogResult = DialogResult.Cancel;
            Close();
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (_dragging)
        {
            _current = PointToScreen(e.Location);
            Invalidate();
            return;
        }
        // hover: highlight the top-level window under the cursor. This
        // overlay is TopMost+fullscreen, so WindowFromPoint() would return
        // this form itself — walk the Z-order instead, skipping ourselves.
        // GetCursorPos returns physical pixels, matching GetWindowRect()
        // exactly even under DPI scaling (PointToScreen uses logical coords).
        GetCursorPos(out var screenPt);
        var hwnd = FindPickableWindowAt(screenPt);
        if (hwnd != _hoverWindow)
        {
            _hoverWindow = hwnd;
            _hoverRect = hwnd != IntPtr.Zero && GetWindowRect(hwnd, out var rc)
                ? new Rectangle(rc.Left, rc.Top, rc.Right - rc.Left, rc.Bottom - rc.Top)
                : Rectangle.Empty;
            Invalidate();
        }
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        if (!_dragging || e.Button != MouseButtons.Left) return;
        _dragging = false;
        GetCursorPos(out var screenPt);
        var rect = NormalizeRect(_start, _current);
        if (rect.Width < 4 || rect.Height < 4)
        {
            // treated as a click: select the window under the cursor
            var hwnd = FindPickableWindowAt(screenPt);
            if (hwnd != IntPtr.Zero)
            {
                WindowHwnd = hwnd;
                DialogResult = DialogResult.OK;
                Close();
                return;
            }
            DialogResult = DialogResult.Cancel;
            Close();
            return;
        }
        Region = rect;
        DialogResult = DialogResult.OK;
        Close();
    }

    /// Find the top-most visible app window whose rectangle contains the
    /// point. EnumWindows enumerates every top-level window on the desktop in
    /// Z-order (front to back), which works even though this overlay is a
    /// modal/owned window — GetWindow(GW_HWNDNEXT) would walk only the owner
    /// chain and never reach other apps' windows. The overlay itself is
    /// skipped, and desktop/taskbar/shell-overlay windows are rejected.
    IntPtr FindPickableWindowAt(Point screenPt)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, _) =>
        {
            if (h != Handle && IsWindowVisible(h) && GetWindowRect(h, out var rc))
            {
                if (screenPt.X >= rc.Left && screenPt.X < rc.Right &&
                    screenPt.Y >= rc.Top && screenPt.Y < rc.Bottom)
                {
                    var pick = ResolvePickableWindow(h);
                    if (pick != IntPtr.Zero)
                    {
                        found = pick;
                        return false; // Z-order front-to-back: first hit wins
                    }
                }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    /// WindowFromPoint can return a child control, the desktop (Progman /
    /// WorkerW), the taskbar or an overlay window — none of which are useful
    /// mirror targets. Walk up to the top-level owner and reject system
    /// windows so the highlighted/picked window is always a real app window.
    static IntPtr ResolvePickableWindow(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return IntPtr.Zero;
        var root = GetAncestor(hwnd, GA_ROOT);
        if (root == IntPtr.Zero) root = hwnd;
        if (!IsWindowVisible(root)) return IntPtr.Zero;

        var cls = new System.Text.StringBuilder(256);
        GetClassName(root, cls, cls.Capacity);
        var name = cls.ToString();
        // desktop + taskbar + known shell overlays
        if (name is "Progman" or "WorkerW" or "Shell_TrayWnd" or "Shell_SecondaryTrayWnd"
            or "Windows.UI.Core.CoreWindow" or "DV2ControlHost") return IntPtr.Zero;
        return root;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.KeyCode == Keys.Escape)
        {
            DialogResult = DialogResult.Cancel;
            Close();
        }
    }

    static Rectangle NormalizeRect(Point a, Point b) =>
        new(Math.Min(a.X, b.X), Math.Min(a.Y, b.Y),
            Math.Abs(a.X - b.X), Math.Abs(a.Y - b.Y));

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        // dim mask over everything outside the live selection
        if (_dragging)
        {
            var sel = NormalizeRect(_start, _current);
            var bounds = ClientRectangle;
            using var mask = new SolidBrush(_maskColor);
            g.FillRectangle(mask, bounds);
            // clear the selected area
            using var clear = new SolidBrush(Color.Transparent);
            g.CompositingMode = CompositingMode.SourceCopy;
            g.FillRectangle(clear, sel);
            g.CompositingMode = CompositingMode.SourceOver;
            using var pen = new Pen(Color.White, 1.5f) { DashStyle = DashStyle.Dash };
            g.DrawRectangle(pen, sel);
            using var labelFont = new Font("Microsoft YaHei UI", 11f);
            using var labelBrush = new SolidBrush(Color.White);
            g.DrawString($"{sel.Width} × {sel.Height}", labelFont, labelBrush,
                         sel.Right + 6, sel.Top - 24);
        }
        else if (_hoverWindow != IntPtr.Zero && _hoverRect != Rectangle.Empty)
        {
            // highlight the hovered window. _hoverRect is in physical screen
            // pixels (from GetWindowRect); convert to client coords so DPI
            // scaling (logical vs physical) can't misplace the outline.
            var tl = PointToClient(_hoverRect.Location);
            var br = PointToClient(new Point(_hoverRect.Right, _hoverRect.Bottom));
            var rc = new Rectangle(tl.X, tl.Y, br.X - tl.X, br.Y - tl.Y);
            using var pen = new Pen(Color.White, 2.5f);
            g.DrawRectangle(pen, rc);
            using var labelFont = new Font("Microsoft YaHei UI", 10f);
            using var labelBrush = new SolidBrush(Color.White);
            g.DrawString("点击选择此窗口（Esc 取消）", labelFont, labelBrush,
                         rc.X + 4, rc.Y + 4);
        }
        else
        {
            using var hintFont = new Font("Microsoft YaHei UI", 12f);
            using var hintBrush = new SolidBrush(Color.White);
            using var center = new StringFormat { Alignment = StringAlignment.Center };
            var hint = "拖动鼠标框选投屏区域\n单击选择要投屏的窗口\nEsc 取消";
            g.DrawString(hint, hintFont, hintBrush,
                         new RectangleF(0, 40, Width, 100), center);
        }
    }

    [DllImport("user32.dll")]
    static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")]
    static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    static extern bool GetCursorPos(out Point lpPoint);
    [DllImport("user32.dll")]
    static extern IntPtr GetAncestor(IntPtr hWnd, uint gaFlags);
    [DllImport("user32.dll")]
    static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder lpClassName, int nMaxCount);
    const uint GA_ROOT = 2;
    [StructLayout(LayoutKind.Sequential)]
    struct RECT { public int Left, Top, Right, Bottom; }
}
