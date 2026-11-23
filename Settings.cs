using System.IO;
using System.Text.Json;

namespace WinVCam;

/// <summary>Local JSON settings (%APPDATA%\WinVCam\settings.json). Extended in later phases.</summary>
public sealed class Settings
{
    public string? CameraId { get; set; }
    public int OutputWidth { get; set; } = 1920;
    public int OutputHeight { get; set; } = 1080;
    public int FrameRate { get; set; } = 30;

    static readonly string PathOnDisk = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "WinVCam", "settings.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(PathOnDisk))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(PathOnDisk)) ?? new Settings();
        }
        catch { /* corrupt file: fall back to defaults */ }
        return new Settings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(PathOnDisk)!);
            File.WriteAllText(PathOnDisk, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { }
    }
}
