using System.Drawing;

namespace AIClockBridge;

// Modal dialog configuring the photo album: source directory, accepted file
// formats (jpg/png/bmp toggles), auto-play interval and play order
// (sequential / shuffle). Writes into the shared AlbumService and rescans.
sealed class AlbumSettingsDialog
{
    readonly AlbumService _album;

    readonly TextBox _dirBox = new();
    readonly CheckBox _ckJpg = new() { Text = "JPG/JPEG", Checked = true };
    readonly CheckBox _ckPng = new() { Text = "PNG", Checked = true };
    readonly CheckBox _ckBmp = new() { Text = "BMP", Checked = true };
    readonly NumericUpDown _interval = new() { Minimum = 1, Maximum = 600, Value = 4 };
    readonly RadioButton _rbSeq = new() { Text = "按顺序播放", Checked = true };
    readonly RadioButton _rbShuffle = new() { Text = "随机播放" };
    readonly ComboBox _sortBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    readonly RadioButton _rbAsc = new() { Text = "递增", Checked = true };
    readonly RadioButton _rbDesc = new() { Text = "递减" };
    static readonly string[] SortLabels = { "按名称", "按日期", "按类型", "按大小" };
    readonly RadioButton _rbFill = new() { Text = "填充屏幕", Checked = true };
    readonly RadioButton _rbFit = new() { Text = "原比例显示" };
    readonly ComboBox _alignBox = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    static readonly string[] AlignLabels = { "居中", "靠上", "靠下", "靠左", "靠右" };

    public AlbumSettingsDialog(AlbumService album)
    {
        _album = album;
    }

    public bool Show()
    {
        using var form = new Form
        {
            Text = "相册设置",
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterScreen,
            MinimizeBox = false,
            MaximizeBox = false,
            ShowInTaskbar = false,
            Font = new Font("Microsoft YaHei UI", 9f),
            ClientSize = new Size(460, 380),
            TopMost = true,
        };

        var dirLabel = new Label { Text = "相册目录（存放照片的文件夹）", AutoSize = true };
        dirLabel.SetBounds(14, 12, 260, 20);
        _dirBox.Text = _album.DirectoryPath;
        _dirBox.SetBounds(14, 34, 330, 24);
        var browse = new Button { Text = "浏览…" };
        browse.SetBounds(354, 34, 92, 26);
        browse.Click += (_, _) =>
        {
            using var dlg = new FolderBrowserDialog { SelectedPath = _album.DirectoryPath };
            if (dlg.ShowDialog() == DialogResult.OK) _dirBox.Text = dlg.SelectedPath;
        };

        var fmtLabel = new Label { Text = "播放的文件格式", AutoSize = true };
        fmtLabel.SetBounds(14, 68, 200, 20);
        _ckJpg.Checked = _album.IncludeJpg;
        _ckJpg.SetBounds(14, 92, 120, 24);
        _ckPng.Checked = _album.IncludePng;
        _ckPng.SetBounds(150, 92, 100, 24);
        _ckBmp.Checked = _album.IncludeBmp;
        _ckBmp.SetBounds(260, 92, 100, 24);

        var intLabel = new Label { Text = "播放间隔（秒）", AutoSize = true };
        intLabel.SetBounds(14, 124, 120, 20);
        _interval.Value = Math.Clamp(_album.IntervalMs / 1000, 1, 600);
        _interval.SetBounds(130, 120, 70, 24);

        // Play order group.  The sorting options are *sub-options of "按顺序
        // 播放"*:  they live on the second row, indented, and are disabled
        // while "随机播放" is selected.  The 递增/递减 radios sit in their
        // own sub-container so they only exclude each other — a bare GroupBox
        // would lump them together with 按顺序播放/随机播放 and picking a
        // direction would deselect the play order.
        var orderGroup = new GroupBox { Text = "播放顺序", Font = form.Font };
        orderGroup.SetBounds(10, 150, 440, 100);
        _rbSeq.Checked = _album.Order == AlbumService.PlayOrder.Sequential;
        _rbSeq.Parent = orderGroup;
        _rbSeq.SetBounds(14, 22, 140, 24);
        _rbShuffle.Checked = _album.Order == AlbumService.PlayOrder.Shuffle;
        _rbShuffle.Parent = orderGroup;
        _rbShuffle.SetBounds(160, 22, 120, 24);

        // second row: sorting sub-options (only meaningful for sequential)
        foreach (var lbl in SortLabels) _sortBox.Items.Add(lbl);
        _sortBox.SelectedIndex = (int)_album.SortKey;
        _sortBox.Parent = orderGroup;
        _sortBox.SetBounds(44, 52, 120, 24);
        var sortHint = new Label { Text = "排序方式与方向", AutoSize = true, Parent = orderGroup };
        sortHint.SetBounds(14, 56, 120, 20);
        // nested container isolates 递增/递减 from the play-order radios
        var dirGroup = new Panel { Parent = orderGroup };
        dirGroup.SetBounds(170, 50, 170, 26);
        _rbAsc.Checked = _album.SortAscending;
        _rbAsc.Parent = dirGroup;
        _rbAsc.SetBounds(0, 0, 80, 24);
        _rbDesc.Checked = !_album.SortAscending;
        _rbDesc.Parent = dirGroup;
        _rbDesc.SetBounds(86, 0, 80, 24);
        void UpdateSortEnabled() => _sortBox.Enabled = dirGroup.Enabled = _rbSeq.Checked;
        _rbSeq.CheckedChanged += (_, _) => UpdateSortEnabled();
        _rbShuffle.CheckedChanged += (_, _) => UpdateSortEnabled();
        UpdateSortEnabled();

        // display mode: fill vs fit, plus alignment for fit.
        var fitGroup = new GroupBox { Text = "图片显示方式", Font = form.Font };
        fitGroup.SetBounds(10, 258, 440, 56);
        _rbFill.Checked = _album.Fit == AlbumService.FitMode.Fill;
        _rbFill.Parent = fitGroup;
        _rbFill.SetBounds(14, 24, 100, 24);
        _rbFit.Checked = _album.Fit == AlbumService.FitMode.Fit;
        _rbFit.Parent = fitGroup;
        _rbFit.SetBounds(120, 24, 110, 24);
        var alignLabel = new Label { Text = "对齐", AutoSize = true, Parent = fitGroup };
        alignLabel.SetBounds(236, 24, 40, 24);
        foreach (var lbl in AlignLabels) _alignBox.Items.Add(lbl);
        _alignBox.SelectedIndex = (int)_album.Alignment;
        _alignBox.Parent = fitGroup;
        _alignBox.SetBounds(278, 24, 120, 24);

        var ok = new Button { Text = "保存", DialogResult = DialogResult.OK };
        ok.SetBounds(280, 330, 80, 28);
        var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel };
        cancel.SetBounds(366, 330, 80, 28);
        form.Controls.AddRange(new Control[]
        {
            dirLabel, _dirBox, browse, fmtLabel, _ckJpg, _ckPng, _ckBmp,
            intLabel, _interval, orderGroup, fitGroup, ok, cancel,
        });
        form.AcceptButton = ok;
        form.CancelButton = cancel;

        if (form.ShowDialog() != DialogResult.OK) return false;

        _album.DirectoryPath = string.IsNullOrWhiteSpace(_dirBox.Text)
            ? Path.Combine(AppContext.BaseDirectory, "album")
            : _dirBox.Text.Trim();
        _album.IncludeJpg = _ckJpg.Checked;
        _album.IncludePng = _ckPng.Checked;
        _album.IncludeBmp = _ckBmp.Checked;
        _album.IntervalMs = (int)_interval.Value * 1000;
        _album.Order = _rbShuffle.Checked ? AlbumService.PlayOrder.Shuffle
                                          : AlbumService.PlayOrder.Sequential;
        _album.SortKey = (AlbumService.SortBy)_sortBox.SelectedIndex;
        _album.SortAscending = _rbAsc.Checked;
        _album.Fit = _rbFill.Checked ? AlbumService.FitMode.Fill : AlbumService.FitMode.Fit;
        _album.Alignment = (AlbumService.Align)_alignBox.SelectedIndex;
        _album.Refresh();
        _album.Save(); // persist so settings survive a restart
        return true;
    }
}
