// VCamReg.dll - tiny native helper the C# app P/Invokes to create/destroy the
// Media Foundation virtual camera. Done in C++ so we use the real
// IMFVirtualCamera declaration from <mfvirtualcamera.h> instead of hand-written
// COM interop. Requires Windows 11 SDK (10.0.22000+) to build and Windows 11 to run.
//
// The camera has Session lifetime: it disappears when the process that created it
// exits (or VCamReg_Stop is called), so the app must stay running.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <mutex>

// {B0C1A1F0-3E5D-4B7E-9A55-5D1E6C2B7A11}  (same CLSID as VCamSource.cpp)
static const wchar_t kSourceClsid[] = L"{B0C1A1F0-3E5D-4B7E-9A55-5D1E6C2B7A11}";
// KSCATEGORY_VIDEO_CAMERA {E5323777-F976-4f5b-9B55-B94699C46E44}
static const GUID kCategoryVideoCamera =
    {0xE5323777, 0xF976, 0x4F5B, {0x9B, 0x55, 0xB9, 0x46, 0x99, 0xC4, 0x6E, 0x44}};

static std::mutex g_mu;
static IMFVirtualCamera* g_vcam = nullptr;
static bool g_mfStarted = false;

extern "C" {

// Returns an HRESULT (0 = success).
__declspec(dllexport) HRESULT __stdcall VCamReg_Start(const wchar_t* friendlyName) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_vcam) return S_OK;  // already running

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return hr;
    g_mfStarted = true;

    hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                               MFVirtualCameraLifetime_Session,
                               MFVirtualCameraAccess_CurrentUser,
                               friendlyName ? friendlyName : L"Virtual Camera",
                               kSourceClsid, &kCategoryVideoCamera, 1, &g_vcam);
    if (FAILED(hr)) { g_vcam = nullptr; return hr; }

    hr = g_vcam->Start(nullptr);
    if (FAILED(hr)) {
        g_vcam->Remove();
        g_vcam->Release();
        g_vcam = nullptr;
    }
    return hr;
}

__declspec(dllexport) void __stdcall VCamReg_Stop() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_vcam) {
        g_vcam->Stop();
        g_vcam->Remove();   // unregisters the device
        g_vcam->Shutdown();
        g_vcam->Release();
        g_vcam = nullptr;
    }
    if (g_mfStarted) { MFShutdown(); g_mfStarted = false; }
}

__declspec(dllexport) int __stdcall VCamReg_IsRunning() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_vcam != nullptr ? 1 : 0;
}

}  // extern "C"
