using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace WinVCam;

public partial class MainWindow : Window
{
    readonly Settings _settings = Settings.Load();
    readonly CameraCapture _camera = new();
    SharedFrameWriter? _writer;
    WriteableBitmap? _bitmap;
    long _lastPreviewTick;
    bool _populating;
    string _cameraStatus = "";

    public MainWindow()
    {
        InitializeComponent();
        _camera.FrameReady += OnFrame;
        _camera.Failed += msg => Dispatcher.BeginInvoke(() => { _cameraStatus = msg; UpdateStatus(); });
    }

    async void Window_Loaded(object sender, RoutedEventArgs e)
    {
        try { _writer = new SharedFrameWriter(); }
        catch (Exception)
        {
            _cameraStatus = "Could not create the shared frame buffer. Try running the app as Administrator.";
        }
        await RefreshCamerasAsync();
        UpdateStatus();
    }

    async Task RefreshCamerasAsync()
    {
        List<CameraInfo> cams;
        try { cams = await CameraCapture.ListAsync(); }
        catch { cams = new(); }

        _populating = true;
        CameraBox.Items.Clear();
        foreach (var c in cams) CameraBox.Items.Add(c);
        _populating = false;

        if (cams.Count == 0)
        {
            _cameraStatus = "No webcam detected. Plug one in and press Refresh.";
            PreviewHint.Visibility = Visibility.Visible;
            await _camera.StopAsync();
            UpdateStatus();
            return;
        }

        var saved = cams.FirstOrDefault(c => c.Id == _settings.CameraId) ?? cams[0];
        CameraBox.SelectedItem = saved;   // triggers SelectionChanged -> starts capture
    }

    async void CameraBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_populating || CameraBox.SelectedItem is not CameraInfo cam) return;
        _settings.CameraId = cam.Id;
        _settings.Save();
        _cameraStatus = "";
        await _camera.StartAsync(cam.Id, _settings.OutputWidth, _settings.OutputHeight, _settings.FrameRate);
        UpdateStatus();
    }

    // Runs on a capture thread.
    void OnFrame(byte[] bgra, int width, int height)
    {
        _writer?.Write(bgra, width, height);   // -> virtual camera

        long now = Environment.TickCount64;
        if (now - _lastPreviewTick < 66) return;   // preview at ~15 fps; the virtual camera gets every frame
        _lastPreviewTick = now;

        // Synchronous so the capture thread cannot overwrite the buffer mid-copy.
        Dispatcher.Invoke(() =>
        {
            if (_bitmap == null || _bitmap.PixelWidth != width || _bitmap.PixelHeight != height)
            {
                _bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgr32, null);
                Preview.Source = _bitmap;
            }
            _bitmap.WritePixels(new Int32Rect(0, 0, width, height), bgra, width * 4, 0);
            PreviewHint.Visibility = Visibility.Collapsed;
        });
    }

    void VCamButton_Click(object sender, RoutedEventArgs e)
    {
        if (VirtualCamera.IsRunning)
        {
            VirtualCamera.Stop();
            _cameraStatus = "";
        }
        else
        {
            var error = VirtualCamera.Start();
            _cameraStatus = error ?? "";
        }
        UpdateStatus();
    }

    async void Refresh_Click(object sender, RoutedEventArgs e) => await RefreshCamerasAsync();

    void UpdateStatus()
    {
        bool vcam = VirtualCamera.IsRunning;
        VCamButton.Content = vcam ? "Stop virtual camera" : "Start virtual camera";

        var parts = new List<string>();
        parts.Add(vcam ? $"Virtual camera: running (\"{VirtualCamera.FriendlyName}\")" : "Virtual camera: stopped");
        if (_camera.ActiveFormatDescription != null) parts.Add($"Capture: {_camera.ActiveFormatDescription}");
        if (_writer != null && !_writer.IsGlobal)
            parts.Add("Warning: not running as Administrator - other apps will not receive video");
        if (_cameraStatus.Length > 0) parts.Add(_cameraStatus);
        StatusText.Text = string.Join("   |   ", parts);
    }

    void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        VirtualCamera.Stop();
        try { _camera.StopAsync().GetAwaiter().GetResult(); } catch { }
        _writer?.Dispose();
        _settings.Save();
    }
}
