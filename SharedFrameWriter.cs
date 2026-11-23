using System.ComponentModel;
using System.Runtime.InteropServices;

namespace WinVCam;

/// <summary>
/// Publishes BGRA frames into the named shared-memory block read by VCamSource.dll.
/// Layout MUST match VCamSource/SharedFrame.h.
/// </summary>
public sealed unsafe class SharedFrameWriter : IDisposable
{
    public const string GlobalName = @"Global\WinVCamFrame";
    public const string LocalName = @"Local\WinVCamFrame";
    public const uint Magic = 0x4D414356;
    public const int MaxWidth = 1920, MaxHeight = 1080, HeaderSize = 64;
    public const long TotalSize = HeaderSize + (long)MaxWidth * MaxHeight * 4;

    // Header offsets (see SharedHeader in SharedFrame.h)
    const int OffMagic = 0, OffVersion = 4, OffWidth = 8, OffHeight = 12, OffStride = 16,
              OffSequence = 24, OffFrameCounter = 32;

    IntPtr _mapping;
    byte* _view;
    ulong _sequence;
    ulong _frames;

    /// <summary>True when the block lives in the Global\ namespace (visible to the Frame Server).</summary>
    public bool IsGlobal { get; private set; }

    public SharedFrameWriter()
    {
        // Read-only for LocalService/SYSTEM/ALL APPLICATION PACKAGES; full control for the owner.
        const string sddl = "D:(A;;GA;;;OW)(A;;GR;;;LS)(A;;GR;;;SY)(A;;GR;;;AC)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, out IntPtr sd, out _))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            var sa = new SECURITY_ATTRIBUTES
            {
                nLength = (uint)sizeof(SECURITY_ATTRIBUTES),
                lpSecurityDescriptor = sd,
                bInheritHandle = 0
            };
            _mapping = CreateFileMappingW(new IntPtr(-1), ref sa, 0x04 /*PAGE_READWRITE*/,
                (uint)(TotalSize >> 32), (uint)(TotalSize & 0xFFFFFFFF), GlobalName);
            IsGlobal = _mapping != IntPtr.Zero;
            if (!IsGlobal)
            {
                // Needs SeCreateGlobalPrivilege (elevated). Fall back so the preview still works,
                // but the Frame Server will not be able to see this block.
                _mapping = CreateFileMappingW(new IntPtr(-1), ref sa, 0x04,
                    (uint)(TotalSize >> 32), (uint)(TotalSize & 0xFFFFFFFF), LocalName);
            }
        }
        finally { LocalFree(sd); }

        if (_mapping == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        _view = (byte*)MapViewOfFile(_mapping, 0x0002 /*FILE_MAP_WRITE*/, 0, 0, (UIntPtr)TotalSize);
        if (_view == null) throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    /// <summary>Copies one top-down BGRA frame (max 1920x1080) into shared memory.</summary>
    public void Write(byte[] bgra, int width, int height)
    {
        if (_view == null) return;
        if (width <= 0 || height <= 0 || width > MaxWidth || height > MaxHeight) return;
        int stride = width * 4;
        if (bgra.Length < stride * height) return;

        // Seqlock: odd = writing, even = stable.
        Volatile.Write(ref *(ulong*)(_view + OffSequence), ++_sequence);
        Interlocked.MemoryBarrier();
        *(uint*)(_view + OffVersion) = 1;
        *(uint*)(_view + OffWidth) = (uint)width;
        *(uint*)(_view + OffHeight) = (uint)height;
        *(uint*)(_view + OffStride) = (uint)stride;
        fixed (byte* src = bgra)
            Buffer.MemoryCopy(src, _view + HeaderSize, TotalSize - HeaderSize, (long)stride * height);
        *(uint*)(_view + OffMagic) = Magic;
        Volatile.Write(ref *(ulong*)(_view + OffFrameCounter), ++_frames);
        Interlocked.MemoryBarrier();
        Volatile.Write(ref *(ulong*)(_view + OffSequence), ++_sequence);
    }

    public void Dispose()
    {
        if (_view != null) { UnmapViewOfFile((IntPtr)_view); _view = null; }
        if (_mapping != IntPtr.Zero) { CloseHandle(_mapping); _mapping = IntPtr.Zero; }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct SECURITY_ATTRIBUTES
    {
        public uint nLength;
        public IntPtr lpSecurityDescriptor;
        public int bInheritHandle;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(IntPtr file, ref SECURITY_ATTRIBUTES attrs, uint protect,
        uint maxHigh, uint maxLow, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr mapping, uint access, uint offHigh, uint offLow, UIntPtr bytes);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UnmapViewOfFile(IntPtr view);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr h);

    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr p);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(
        string sddl, uint revision, out IntPtr sd, out uint size);
}
