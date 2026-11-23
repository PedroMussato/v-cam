// WinVCam virtual camera media source (Phase 1).
//
// Loaded by the Windows Frame Server when the app calls MFCreateVirtualCamera()
// with this DLL's CLSID. It reads BGRA frames from the shared-memory block that
// VCamApp writes (see SharedFrame.h) and delivers them as NV12 samples.
//
// Structure follows Microsoft's VCamSample (IMFMediaSourceEx + IMFMediaStream2),
// but is written from scratch and kept minimal. NOT compiled or tested on the
// machine that authored it - see README.md "Known risks".

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include "SharedFrame.h"

using Microsoft::WRL::ComPtr;

// {B0C1A1F0-3E5D-4B7E-9A55-5D1E6C2B7A11}  (keep in sync with VCamReg.cpp)
static const GUID CLSID_VCamSource =
    {0xB0C1A1F0, 0x3E5D, 0x4B7E, {0x9A, 0x55, 0x5D, 0x1E, 0x6C, 0x2B, 0x7A, 0x11}};
static const wchar_t kClsidString[] = L"{B0C1A1F0-3E5D-4B7E-9A55-5D1E6C2B7A11}";

static HMODULE g_module = nullptr;
static std::atomic<long> g_objectCount{0};
static std::atomic<long> g_lockCount{0};

static constexpr LONGLONG kFrameDuration100ns = 10000000LL / 30;

// ---------------------------------------------------------------------------
// Shared-memory frame reader + BGRA -> NV12 conversion (nearest-neighbour scale)
// ---------------------------------------------------------------------------
class FrameReader {
public:
    ~FrameReader() { Close(); }

    // Fills dst (NV12, dw x dh). Returns true if a real camera frame was rendered.
    bool Render(uint8_t* dst, uint32_t dw, uint32_t dh) {
        if (!EnsureOpen()) { FillIdle(dst, dw, dh); return false; }
        auto* hdr = reinterpret_cast<SharedHeader*>(view_);
        const uint8_t* pixels = view_ + kHeaderSize;
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint64_t s1 = hdr->sequence;
            if (s1 & 1) { Sleep(0); continue; }  // writer busy
            MemoryBarrier();
            uint32_t w = hdr->width, h = hdr->height, stride = hdr->stride;
            if (hdr->magic != kMagic || w == 0 || h == 0 || w > kMaxWidth ||
                h > kMaxHeight || stride < w * 4 || stride > kMaxWidth * 4) {
                FillIdle(dst, dw, dh);
                return false;
            }
            Convert(pixels, w, h, stride, dst, dw, dh);
            MemoryBarrier();
            if (hdr->sequence == s1) return true;
        }
        return true;  // tearing tolerated for one frame
    }

private:
    bool EnsureOpen() {
        if (view_) return true;
        ULONGLONG now = GetTickCount64();
        if (now - lastTry_ < 500) return false;  // don't hammer the kernel
        lastTry_ = now;
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kSharedMemName);
        if (!mapping_) return false;
        view_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kSharedMemSize));
        if (!view_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
        return true;
    }
    void Close() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr; mapping_ = nullptr;
    }
    static void FillIdle(uint8_t* dst, uint32_t dw, uint32_t dh) {
        memset(dst, 40, (size_t)dw * dh);                 // dark grey luma
        memset(dst + (size_t)dw * dh, 128, (size_t)dw * dh / 2);  // neutral chroma
    }
    void Convert(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t sstride,
                 uint8_t* dst, uint32_t dw, uint32_t dh) {
        xmap_.resize(dw);
        for (uint32_t i = 0; i < dw; ++i) xmap_[i] = (uint32_t)((uint64_t)i * sw / dw) * 4;
        uint8_t* yPlane = dst;
        uint8_t* uvPlane = dst + (size_t)dw * dh;
        for (uint32_t j = 0; j < dh; ++j) {
            const uint8_t* row = src + (size_t)((uint64_t)j * sh / dh) * sstride;
            uint8_t* yRow = yPlane + (size_t)j * dw;
            uint8_t* uvRow = uvPlane + (size_t)(j / 2) * dw;
            bool evenRow = (j & 1) == 0;
            for (uint32_t i = 0; i < dw; ++i) {
                const uint8_t* p = row + xmap_[i];
                int b = p[0], g = p[1], r = p[2];
                yRow[i] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                if (evenRow && (i & 1) == 0) {
                    uvRow[i]     = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                    uvRow[i + 1] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                }
            }
        }
    }

    HANDLE mapping_ = nullptr;
    uint8_t* view_ = nullptr;
    ULONGLONG lastTry_ = 0;
    std::vector<uint32_t> xmap_;
};

// ---------------------------------------------------------------------------
// Media stream
// ---------------------------------------------------------------------------
class VCamStream final : public IMFMediaStream2 {
public:
    VCamStream(IMFMediaSource* source, IMFStreamDescriptor* sd) : source_(source), sd_(sd) {
        MFCreateEventQueue(&queue_);
        g_objectCount++;
    }
    ~VCamStream() { g_objectCount--; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
            riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
            *ppv = static_cast<IMFMediaStream2*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override { return queue_->GetEvent(flags, ev); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override { return queue_->BeginGetEvent(cb, st); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* res, IMFMediaEvent** ev) override { return queue_->EndGetEvent(res, ev); }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override {
        return queue_->QueueEventParamVar(met, ext, hr, v);
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> lk(mu_);
        if (!source_) return MF_E_SHUTDOWN;
        *pp = source_.Get(); (*pp)->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** pp) override {
        if (!pp) return E_POINTER;
        *pp = sd_.Get(); (*pp)->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown*) override { return S_OK; }  // we push at a fixed rate

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override {
        switch (state) {
            case MF_STREAM_STATE_RUNNING: running_ = true; break;
            case MF_STREAM_STATE_STOPPED:
            case MF_STREAM_STATE_PAUSED:  running_ = false; break;
            default: return MF_E_INVALID_STATE_TRANSITION;
        }
        return S_OK;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* s) override {
        if (!s) return E_POINTER;
        *s = running_ ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
        return S_OK;
    }

    void Start() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!source_) return;
            if (!thread_.joinable()) thread_ = std::thread([this] { Loop(); });
        }
        running_ = true;
        queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    }
    void Stop() {
        running_ = false;
        queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    void Shutdown() {
        stop_ = true;
        running_ = false;
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        queue_->Shutdown();
        source_.Reset();
    }

private:
    void Loop() {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        LONGLONG next = MFGetSystemTime();
        while (!stop_) {
            next += kFrameDuration100ns;
            LONGLONG now = MFGetSystemTime();
            LONGLONG wait = next - now;
            if (wait > 0) {
                LARGE_INTEGER due; due.QuadPart = -wait;
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            } else if (wait < -5 * kFrameDuration100ns) {
                next = now;  // fell behind: resync instead of bursting
            }
            if (stop_) break;
            if (running_) Deliver();
        }
        if (timer) CloseHandle(timer);
    }

    void Deliver() {
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> mt;
        UINT32 w = 0, h = 0;
        if (FAILED(sd_->GetMediaTypeHandler(&handler)) || FAILED(handler->GetCurrentMediaType(&mt)) ||
            FAILED(MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return;

        DWORD bytes = w * h * 3 / 2;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return;
        BYTE* data = nullptr;
        if (FAILED(buf->Lock(&data, nullptr, nullptr))) return;
        reader_.Render(data, w, h);
        buf->Unlock();
        buf->SetCurrentLength(bytes);

        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) return;
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(MFGetSystemTime());
        sample->SetSampleDuration(kFrameDuration100ns);
        queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }

    std::atomic<ULONG> refs_{1};
    std::mutex mu_;
    ComPtr<IMFMediaSource> source_;  // released in Shutdown() to break the cycle
    ComPtr<IMFStreamDescriptor> sd_;
    ComPtr<IMFMediaEventQueue> queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    FrameReader reader_;
};

// ---------------------------------------------------------------------------
// Media source
// ---------------------------------------------------------------------------
class VCamSource final : public IMFMediaSourceEx,
                         public IMFGetService,
                         public IKsControl,
                         public IMFSampleAllocatorControl {
public:
    VCamSource() { g_objectCount++; }
    ~VCamSource() { g_objectCount--; }

    HRESULT Init() {
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (FAILED(hr)) return hr;

        struct Mode { UINT32 w, h; };
        const Mode modes[] = {{1920, 1080}, {1280, 720}};  // first = default
        ComPtr<IMFMediaType> types[2];
        IMFMediaType* raw[2] = {};
        for (int i = 0; i < 2; ++i) {
            hr = MFCreateMediaType(&types[i]);
            if (FAILED(hr)) return hr;
            IMFMediaType* t = types[i].Get();
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(t, MF_MT_FRAME_SIZE, modes[i].w, modes[i].h);
            MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 30, 1);
            MFSetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MIN, 30, 1);
            MFSetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MAX, 30, 1);
            MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            t->SetUINT32(MF_MT_DEFAULT_STRIDE, modes[i].w);
            t->SetUINT32(MF_MT_SAMPLE_SIZE, modes[i].w * modes[i].h * 3 / 2);
            raw[i] = t;
        }

        hr = MFCreateStreamDescriptor(0, 2, raw, &sd_);
        if (FAILED(hr)) return hr;
        sd_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        sd_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        sd_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        sd_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

        ComPtr<IMFMediaTypeHandler> handler;
        hr = sd_->GetMediaTypeHandler(&handler);
        if (FAILED(hr)) return hr;
        handler->SetCurrentMediaType(raw[0]);

        hr = MFCreatePresentationDescriptor(1, sd_.GetAddressOf(), &pd_);
        if (FAILED(hr)) return hr;
        pd_->SelectStream(0);

        hr = MFCreateAttributes(&attrs_, 2);
        if (FAILED(hr)) return hr;
        attrs_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

        stream_.Attach(new (std::nothrow) VCamStream(static_cast<IMFMediaSourceEx*>(this), sd_.Get()));
        return stream_ ? S_OK : E_OUTOFMEMORY;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
            riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx)
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (riid == IID_IMFGetService)
            *ppv = static_cast<IMFGetService*>(this);
        else if (riid == __uuidof(IKsControl))
            *ppv = static_cast<IKsControl*>(this);
        else if (riid == IID_IMFSampleAllocatorControl)
            *ppv = static_cast<IMFSampleAllocatorControl*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override { return queue_->GetEvent(flags, ev); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override { return queue_->BeginGetEvent(cb, st); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* res, IMFMediaEvent** ev) override { return queue_->EndGetEvent(res, ev); }
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override {
        return queue_->QueueEventParamVar(met, ext, hr, v);
    }

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* c) override {
        if (!c) return E_POINTER;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pp) override {
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return pd_->Clone(pp);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* start) override {
        if (!pd || !start) return E_INVALIDARG;
        if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                   static_cast<IMFMediaStream2*>(stream_.Get()));
        started_ = true;
        queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, start);
        stream_->Start();
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        stream_->Stop();
        queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
        return S_OK;
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return S_OK;
        shutdown_ = true;
        stream_->Shutdown();
        queue_->Shutdown();
        return S_OK;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** pp) override {
        if (!pp) return E_POINTER;
        *pp = attrs_.Get(); (*pp)->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** pp) override {
        if (!pp) return E_POINTER;
        if (id != 0) return E_INVALIDARG;
        return sd_.CopyTo(pp);
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return E_NOTIMPL; }

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }

    // IKsControl - no custom properties supported
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

    // IMFSampleAllocatorControl - we allocate our own samples
    STDMETHODIMP SetDefaultAllocator(DWORD, IUnknown*) override { return S_OK; }
    STDMETHODIMP GetAllocatorUsage(DWORD outId, DWORD* inId, MFSampleAllocatorUsage* usage) override {
        if (!inId || !usage) return E_POINTER;
        *inId = outId;
        *usage = MFSampleAllocatorUsage_DoesNotAllocate;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex mu_;
    bool shutdown_ = false;
    bool started_ = false;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> sd_;
    ComPtr<IMFPresentationDescriptor> pd_;
    ComPtr<IMFAttributes> attrs_;
    ComPtr<VCamStream> stream_;
};

// ---------------------------------------------------------------------------
// COM plumbing and registration
// ---------------------------------------------------------------------------
class VCamFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // static lifetime
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        ComPtr<VCamSource> src;
        src.Attach(new (std::nothrow) VCamSource());
        if (!src) return E_OUTOFMEMORY;
        HRESULT hr = src->Init();
        if (FAILED(hr)) return hr;
        return src->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) g_lockCount++; else g_lockCount--;
        return S_OK;
    }
};
static VCamFactory g_factory;

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = m;
        DisableThreadLibraryCalls(m);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_VCamSource) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return (g_objectCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

// Registers the in-proc COM server machine-wide (needs elevation: HKLM).
// The Frame Server runs as LocalService, so a per-user (HKCU) registration
// would not be visible to it.
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString;
    std::wstring inproc = key + L"\\InprocServer32";
    const wchar_t* name = L"WinVCam Source";
    LSTATUS s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, REG_SZ, name, (DWORD)((wcslen(name) + 1) * 2));
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, inproc.c_str(), nullptr, REG_SZ, path, (DWORD)((wcslen(path) + 1) * 2));
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    const wchar_t* model = L"Both";
    s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, inproc.c_str(), L"ThreadingModel", REG_SZ, model, (DWORD)((wcslen(model) + 1) * 2));
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidString;
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}
