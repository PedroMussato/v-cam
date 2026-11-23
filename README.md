# WinVCam - Phase 1: Virtual Camera Foundation

Physical webcam -> WinRT capture -> shared memory -> `VCamSource.dll` (Media Foundation
virtual camera source, loaded by the Windows Frame Server) -> Zoom / Teams / Meet / Discord.

Phase 1 forwards the **original** camera frames. Segmentation, blur and backgrounds are Phases 2-3.

## Layout

| Path | What it is |
|---|---|
| `VCamSource/VCamSource.cpp` | C++ `IMFMediaSourceEx` + `IMFMediaStream2`; reads BGRA from shared memory, outputs NV12 (1080p / 720p, 30 fps) |
| `VCamSource/VCamReg.cpp` | Tiny C++ DLL the app calls to create/remove the virtual camera (`MFCreateVirtualCamera`) |
| `VCamSource/SharedFrame.h` | Shared-memory layout (must match `VCamApp/SharedFrameWriter.cs`) |
| `VCamApp/` | WPF app (.NET 8): camera picker, live preview, start/stop virtual camera, JSON settings |
| `scripts/` | `build.ps1`, `register.ps1`, `unregister.ps1` |

## Requirements

- **Windows 11** (the Media Foundation virtual camera API is Windows 11 only), x64
- Visual Studio 2022 with *Desktop development with C++* and the **Windows 11 SDK (10.0.22000+)**
- CMake 3.20+ and the .NET 8 SDK

## Build and run

1. Elevated PowerShell is not needed to build: `scripts\build.ps1`
2. **Once, as Administrator:** `scripts\register.ps1` (copies `VCamSource.dll` to
   `C:\Program Files\WinVCam` and registers the COM server in HKLM)
3. Run `VCamApp\bin\x64\Release\net8.0-windows10.0.19041.0\win-x64\WinVCam.exe`
   (the manifest requests Administrator; see "Known risks")
4. Pick your webcam, press **Start virtual camera**
5. Open Windows Camera / Zoom / Meet and choose **Virtual Camera**

To remove: close the app, then run `scripts\unregister.ps1` as Administrator.

## Phase 1 test checklist

- [ ] App shows your webcam preview
- [ ] "Virtual Camera" appears in Windows Camera's camera list while the app runs
- [ ] Windows Camera shows live video through it (also with the app window minimized)
- [ ] Google Meet in Chrome and Zoom can select and show it
- [ ] Stop virtual camera -> device disappears; close app -> device disappears
- [ ] Switch cameras in the dropdown without restarting
- [ ] Unplug the webcam -> friendly message, no crash

Do not start Phase 2 until all of these pass.

## Known risks (this code was written but NOT compiled or run)

It was authored on a machine without Windows or the Windows SDK, so expect to fix some
compile errors on first build. The parts I am least sure about, in order:

1. **Shared memory across sessions.** The Frame Server runs as LocalService in session 0, so the
   frame buffer is created in the `Global\` namespace with a DACL granting LocalService read
   access. Creating `Global\` objects needs SeCreateGlobalPrivilege, which is why the app is
   elevated for now. If the camera shows only a grey frame while the preview works, this is
   the first thing to check (the status bar warns when the app fell back to `Local\`).
   A cleaner design for Phase 5/6 is a small elevated/service process that owns the buffer.
2. **Source interfaces.** I implemented `IMFMediaSourceEx`, `IMFGetService`, `IKsControl` and
   `IMFSampleAllocatorControl` on the source following Microsoft's VCamSample from memory;
   compare against the official sample (github.com/microsoft/Windows-Camera, `VCamSample`) if the device
   appears but produces no frames.
3. **Registration scope.** The virtual camera is created with `Session` lifetime and
   `CurrentUser` access, so it vanishes when the app exits. Whether `regsvr32`-style HKLM
   registration is sufficient on every Windows 11 build is unverified.
4. **Scaling.** Output is nearest-neighbour scaled on the CPU in the source. This is fine for
   Phase 1 and gets replaced by GPU processing later.
5. **Capture format.** Formats above 1920x1080 are ignored; the camera runs at the largest
   format up to 1080p that reaches 30 fps.

No frames or images leave the machine; everything is local.
