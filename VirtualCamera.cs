using System.Runtime.InteropServices;

namespace WinVCam;

/// <summary>Thin wrapper over VCamReg.dll (native helper that calls MFCreateVirtualCamera).</summary>
public static class VirtualCamera
{
    public const string FriendlyName = "Virtual Camera";

    [DllImport("VCamReg.dll", CharSet = CharSet.Unicode)]
    static extern int VCamReg_Start(string friendlyName);

    [DllImport("VCamReg.dll")]
    static extern void VCamReg_Stop();

    [DllImport("VCamReg.dll")]
    static extern int VCamReg_IsRunning();

    public static bool IsRunning
    {
        get { try { return VCamReg_IsRunning() != 0; } catch { return false; } }
    }

    /// <summary>Returns null on success, otherwise a user-readable error.</summary>
    public static string? Start()
    {
        int hr;
        try { hr = VCamReg_Start(FriendlyName); }
        catch (DllNotFoundException)
        {
            return "VCamReg.dll was not found next to the app. Build the native projects first (see README).";
        }
        catch (EntryPointNotFoundException)
        {
            return "VCamReg.dll is out of date. Rebuild the native projects.";
        }

        if (hr == 0) return null;
        // E_ACCESSDENIED, REGDB_E_CLASSNOTREG, E_NOTIMPL / not supported on Windows 10, etc.
        return (uint)hr switch
        {
            0x80070005 => "Windows refused to create the virtual camera (access denied). Run the app as Administrator.",
            0x80040154 => "The virtual camera source is not registered. Run scripts\\register.ps1 as Administrator.",
            0x80070002 or 0x8007007E => "A required Windows component was not found. The virtual camera needs Windows 11.",
            _ => $"The virtual camera could not be started (error 0x{hr:X8})."
        };
    }

    public static void Stop()
    {
        try { VCamReg_Stop(); } catch { }
    }
}
