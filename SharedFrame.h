// Shared-memory frame protocol between VCamApp (writer) and VCamSource (reader).
// MUST stay in sync with VCamApp/SharedFrameWriter.cs.
#pragma once
#include <cstdint>

constexpr wchar_t kSharedMemName[] = L"Global\\WinVCamFrame";
constexpr uint32_t kMagic = 0x4D414356;  // 'VCAM'
constexpr uint32_t kMaxWidth = 1920;
constexpr uint32_t kMaxHeight = 1080;
constexpr uint32_t kHeaderSize = 64;

#pragma pack(push, 1)
struct SharedHeader {
    uint32_t magic;       // kMagic once the writer has initialised the block
    uint32_t version;     // 1
    uint32_t width;       // current frame width (BGRA, top-down)
    uint32_t height;      // current frame height
    uint32_t stride;      // bytes per row
    uint32_t reserved0;
    volatile uint64_t sequence;  // seqlock: odd while writing, even when stable
    volatile uint64_t frameCounter;  // increments once per published frame
    uint8_t pad[kHeaderSize - 40];
};
#pragma pack(pop)
static_assert(sizeof(SharedHeader) == kHeaderSize, "header must be 64 bytes");

constexpr size_t kSharedMemSize = kHeaderSize + (size_t)kMaxWidth * kMaxHeight * 4;
