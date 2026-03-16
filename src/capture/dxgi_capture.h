#pragma once

#include "capture/capture.h"

#ifdef OMNIDESK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace omnidesk {

/// Screen capture using DXGI Desktop Duplication API (Windows 8+).
/// Uses IDXGIOutputDuplication for efficient GPU-side frame acquisition.
/// Handles dirty rects, move rects, pointer shape, desktop switches,
/// and resolution/DPI changes.
class DxgiCapture : public ICaptureSource {
public:
    DxgiCapture();
    ~DxgiCapture() override;

    DxgiCapture(const DxgiCapture&) = delete;
    DxgiCapture& operator=(const DxgiCapture&) = delete;

    bool init(const CaptureConfig& config) override;
    CaptureResult captureFrame(Frame& frame) override;
    std::vector<MonitorInfo> enumMonitors() override;
    void release() override;

private:
    bool createDevice();
    bool createDuplication();
    void releaseDuplication();

    bool processFrame(IDXGIResource* resource, Frame& frame);
    std::vector<Rect> getDirtyRects(IDXGIOutputDuplication* dup,
                                    DXGI_OUTDUPL_FRAME_INFO& info);
    std::vector<Rect> getMoveRects(IDXGIOutputDuplication* dup,
                                   DXGI_OUTDUPL_FRAME_INFO& info);
    CursorInfo getPointerShape(const DXGI_OUTDUPL_FRAME_INFO& info);

    bool handleDesktopSwitch();

    // D3D11 / DXGI objects
    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication>  duplication_;
    Microsoft::WRL::ComPtr<IDXGIOutput1>            output_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         stagingTex_;

    // Metadata scratch buffers (reused across frames to avoid allocation)
    std::vector<uint8_t> dirtyRectBuf_;
    std::vector<uint8_t> moveRectBuf_;
    std::vector<uint8_t> pointerShapeBuf_;

    // State
    CaptureConfig config_          = {};
    DXGI_OUTPUT_DESC outputDesc_   = {};
    int32_t       targetOutputIdx_ = 0;
    uint64_t      frameCounter_    = 0;
    bool          initialized_     = false;
    bool          needReinit_      = false;

    // Pointer shape cache
    uint64_t      lastPointerShapeHash_ = 0;
    CursorInfo    cachedCursor_;
};

} // namespace omnidesk

#endif // OMNIDESK_PLATFORM_WINDOWS
