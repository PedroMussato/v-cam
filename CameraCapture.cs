using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Devices.Enumeration;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.Capture.Frames;
using Windows.Media.MediaProperties;

namespace WinVCam;

public sealed record CameraInfo(string Id, string Name)
{
    public override string ToString() => Name;
}

/// <summary>
/// Captures BGRA frames from one physical webcam via MediaCapture + MediaFrameReader.
/// Only one MediaCapture is ever opened per camera; call StopAsync before switching.
/// </summary>
public sealed class CameraCapture : IAsyncDisposable
{
    public delegate void FrameHandler(byte[] bgra, int width, int height);

    public event FrameHandler? FrameReady;
    public event Action<string>? Failed;   // user-readable message

    MediaCapture? _capture;
    MediaFrameReader? _reader;
    byte[] _bytes = Array.Empty<byte>();
    Windows.Storage.Streams.IBuffer? _buffer;
    int _busy;

    public string? ActiveFormatDescription { get; private set; }

    public static async Task<List<CameraInfo>> ListAsync()
    {
        var devices = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture);
        return devices
            .Where(d => !d.Name.Contains("Virtual Camera", StringComparison.OrdinalIgnoreCase)) // never capture ourselves
            .Select(d => new CameraInfo(d.Id, d.Name))
            .ToList();
    }

    public async Task StartAsync(string deviceId, int targetWidth = 1920, int targetHeight = 1080, int targetFps = 30)
    {
        await StopAsync();
        try
        {
            var groups = await MediaFrameSourceGroup.FindAllAsync();
            var group = groups.FirstOrDefault(g =>
                g.SourceInfos.Any(i => i.DeviceInformation?.Id == deviceId));
            if (group == null) { Failed?.Invoke("The selected camera was not found. It may have been unplugged."); return; }

            _capture = new MediaCapture();
            _capture.Failed += (_, e) => Failed?.Invoke("The camera stopped working or was disconnected.");
            await _capture.InitializeAsync(new MediaCaptureInitializationSettings
            {
                SourceGroup = group,
                VideoDeviceId = deviceId,
                SharingMode = MediaCaptureSharingMode.ExclusiveControl,
                MemoryPreference = MediaCaptureMemoryPreference.Cpu,
                StreamingCaptureMode = StreamingCaptureMode.Video
            });

            var source = _capture.FrameSources.Values
                .FirstOrDefault(s => s.Info.SourceKind == MediaFrameSourceKind.Color);
            if (source == null) { Failed?.Invoke("This camera does not provide a colour video stream."); return; }

            var format = PickFormat(source, targetWidth, targetHeight, targetFps);
            if (format != null)
            {
                await source.SetFormatAsync(format);
                ActiveFormatDescription =
                    $"{format.VideoFormat.Width}x{format.VideoFormat.Height} @ " +
                    $"{format.FrameRate.Numerator / Math.Max(1.0, format.FrameRate.Denominator):0}fps";
            }

            _reader = await _capture.CreateFrameReaderAsync(source, MediaEncodingSubtypes.Bgra8);
            _reader.AcquisitionMode = MediaFrameReaderAcquisitionMode.Realtime;
            _reader.FrameArrived += OnFrameArrived;
            var status = await _reader.StartAsync();
            if (status != MediaFrameReaderStartStatus.Success)
                Failed?.Invoke(status == MediaFrameReaderStartStatus.AccessDenied
                    ? "Camera access is blocked. Enable camera access for desktop apps in Windows Settings > Privacy > Camera."
                    : "The camera could not be started. It may be in use by another application.");
        }
        catch (UnauthorizedAccessException)
        {
            Failed?.Invoke("Camera access was denied. Enable camera access for desktop apps in Windows Settings > Privacy > Camera.");
            await StopAsync();
        }
        catch (Exception)
        {
            Failed?.Invoke("The camera could not be opened. It may be in use by another application.");
            await StopAsync();
        }
    }

    /// Prefers the largest format that fits within targetWidth x targetHeight and reaches targetFps.
    static MediaFrameFormat? PickFormat(MediaFrameSource source, int w, int h, int fps)
    {
        double Fps(MediaFrameFormat f) => f.FrameRate.Numerator / Math.Max(1.0, f.FrameRate.Denominator);
        var fitting = source.SupportedFormats
            .Where(f => f.VideoFormat.Width <= (uint)w && f.VideoFormat.Height <= (uint)h)
            .ToList();
        return fitting
            .OrderByDescending(f => Fps(f) >= fps - 0.5)                 // 30fps-capable first
            .ThenByDescending(f => (long)f.VideoFormat.Width * f.VideoFormat.Height)
            .ThenBy(f => Math.Abs(Fps(f) - fps))
            .FirstOrDefault();
    }

    void OnFrameArrived(MediaFrameReader sender, MediaFrameArrivedEventArgs args)
    {
        if (Interlocked.Exchange(ref _busy, 1) == 1) return;   // drop frames rather than queue up
        try
        {
            using var frameRef = sender.TryAcquireLatestFrame();
            var bmp = frameRef?.VideoMediaFrame?.SoftwareBitmap;
            if (bmp == null) return;

            SoftwareBitmap? converted = null;
            try
            {
                var use = bmp;
                if (bmp.BitmapPixelFormat != BitmapPixelFormat.Bgra8 || bmp.BitmapAlphaMode == BitmapAlphaMode.Straight)
                {
                    converted = SoftwareBitmap.Convert(bmp, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore);
                    use = converted;
                }

                int w = use.PixelWidth, h = use.PixelHeight;
                int needed = w * h * 4;
                if (_bytes.Length != needed)
                {
                    _bytes = new byte[needed];
                    _buffer = _bytes.AsBuffer();
                }
                use.CopyToBuffer(_buffer);
                FrameReady?.Invoke(_bytes, w, h);
            }
            finally { converted?.Dispose(); bmp.Dispose(); }
        }
        catch { /* a single bad frame must not kill the stream */ }
        finally { Interlocked.Exchange(ref _busy, 0); }
    }

    public async Task StopAsync()
    {
        var reader = _reader; var capture = _capture;
        _reader = null; _capture = null;
        ActiveFormatDescription = null;
        try
        {
            if (reader != null) { reader.FrameArrived -= OnFrameArrived; await reader.StopAsync(); reader.Dispose(); }
        }
        catch { }
        try { capture?.Dispose(); } catch { }
    }

    public async ValueTask DisposeAsync() => await StopAsync();
}
