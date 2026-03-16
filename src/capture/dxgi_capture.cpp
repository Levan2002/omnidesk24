#ifdef OMNIDESK_PLATFORM_WINDOWS

#include "capture/dxgi_capture.h"
#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace omnidesk {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DxgiCapture::DxgiCapture() = default;

DxgiCapture::~DxgiCapture() {
    release();
}

// ---------------------------------------------------------------------------
// ICaptureSource interface
// ---------------------------------------------------------------------------

bool DxgiCapture::init(const CaptureConfig& config) {
    release();
    config_ = config;

    if (!createDevice()) {
        LOG_ERROR("DxgiCapture: failed to create D3D11 device");
        release();
        return false;
    }

    // Determine target monitor
    auto monitors = enumMonitors();
    if (monitors.empty()) {
        LOG_ERROR("DxgiCapture: no monitors found");
        release();
        return false;
    }

    // Find the requested monitor (or primary)
    targetOutputIdx_ = 0;
    if (config.monitorId >= 0) {
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].id == config.monitorId) {
                targetOutputIdx_ = static_cast<int32_t>(i);
                break;
            }
        }
    } else {
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].primary) {
                targetOutputIdx_ = static_cast<int32_t>(i);
                break;
            }
        }
    }

    if (!createDuplication()) {
        LOG_ERROR("DxgiCapture: failed to create desktop duplication");
        release();
        return false;
    }

    initialized_ = true;
    frameCounter_ = 0;
    needReinit_ = false;
    LOG_INFO("DxgiCapture: initialised on output %d (%dx%d)",
             targetOutputIdx_,
             outputDesc_.DesktopCoordinates.right -
                 outputDesc_.DesktopCoordinates.left,
             outputDesc_.DesktopCoordinates.bottom -
                 outputDesc_.DesktopCoordinates.top);
    return true;
}

CaptureResult DxgiCapture::captureFrame(Frame& frame) {
    CaptureResult result;

    if (!initialized_) {
        result.status = CaptureResult::ERROR;
        return result;
    }

    // Handle pending reinitialisation (desktop switch or resolution change)
    if (needReinit_) {
        if (!handleDesktopSwitch()) {
            result.status = CaptureResult::ERROR;
            return result;
        }
    }

    auto captureStart = std::chrono::steady_clock::now();

    // Acquire the next frame from the duplication interface
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

    HRESULT hr = duplication_->AcquireNextFrame(
        100,  // timeout in ms
        &frameInfo,
        &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        result.status = CaptureResult::TIMEOUT;
        return result;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Desktop switch (UAC, lock screen, Ctrl+Alt+Del, etc.)
        LOG_WARN("DxgiCapture: access lost — desktop switch detected");
        needReinit_ = true;
        result.status = CaptureResult::DISPLAY_CHANGED;
        return result;
    }

    if (hr == DXGI_ERROR_INVALID_CALL) {
        // Resolution or mode change
        LOG_WARN("DxgiCapture: invalid call — resolution change detected");
        needReinit_ = true;
        result.status = CaptureResult::DISPLAY_CHANGED;
        return result;
    }

    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: AcquireNextFrame failed (hr=0x%08lX)", hr);
        result.status = CaptureResult::ERROR;
        return result;
    }

    // Collect dirty and move rects before processing the frame
    std::vector<Rect> dirtyRects = getDirtyRects(duplication_.Get(), frameInfo);
    std::vector<Rect> moveRects = getMoveRects(duplication_.Get(), frameInfo);

    // Merge move rects into dirty rects (destination regions)
    for (const auto& r : moveRects) {
        dirtyRects.push_back(r);
    }

    // Process the captured desktop texture
    if (!processFrame(desktopResource.Get(), frame)) {
        duplication_->ReleaseFrame();
        result.status = CaptureResult::ERROR;
        return result;
    }

    frame.frameId = frameCounter_++;
    frame.timestampUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            captureStart.time_since_epoch())
            .count());

    // Populate dirty rects
    if (dirtyRects.empty() || frameCounter_ == 1) {
        // First frame or no incremental info: full frame is dirty
        result.dirtyRects.push_back(Rect{0, 0, frame.width, frame.height});
    } else {
        result.dirtyRects = std::move(dirtyRects);
    }

    // Capture pointer shape
    if (config_.captureCursor && frameInfo.PointerPosition.Visible) {
        result.cursor = getPointerShape(frameInfo);
        result.cursor.x = frameInfo.PointerPosition.Position.x;
        result.cursor.y = frameInfo.PointerPosition.Position.y;
        result.cursor.visible = (frameInfo.PointerPosition.Visible != 0);
    }

    duplication_->ReleaseFrame();

    auto captureEnd = std::chrono::steady_clock::now();
    result.captureTimeUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            captureEnd - captureStart)
            .count());
    result.status = CaptureResult::OK;
    return result;
}

std::vector<MonitorInfo> DxgiCapture::enumMonitors() {
    std::vector<MonitorInfo> monitors;

    if (!device_) {
        return monitors;
    }

    // Get the DXGI device from the D3D11 device
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return monitors;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return monitors;

    // Enumerate outputs on this adapter
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc))) {
            MonitorInfo mi;
            mi.id = static_cast<int32_t>(i);

            // Convert wide-char name to UTF-8
            char nameBuf[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1,
                                nameBuf, sizeof(nameBuf), nullptr, nullptr);
            mi.name = nameBuf;

            RECT rc = desc.DesktopCoordinates;
            mi.bounds.x = rc.left;
            mi.bounds.y = rc.top;
            mi.bounds.width = rc.right - rc.left;
            mi.bounds.height = rc.bottom - rc.top;

            // Heuristic: the monitor at (0,0) is typically the primary
            mi.primary = (rc.left == 0 && rc.top == 0);

            monitors.push_back(mi);
        }
        output.Reset();
    }

    return monitors;
}

void DxgiCapture::release() {
    releaseDuplication();

    stagingTex_.Reset();
    context_.Reset();
    device_.Reset();

    dirtyRectBuf_.clear();
    moveRectBuf_.clear();
    pointerShapeBuf_.clear();
    outputDesc_ = {};

    initialized_ = false;
    needReinit_ = false;
    frameCounter_ = 0;
}

// ---------------------------------------------------------------------------
// Private: D3D11 device creation
// ---------------------------------------------------------------------------

bool DxgiCapture::createDevice() {
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,                        // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                        // no software rasterizer
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        static_cast<UINT>(std::size(featureLevels)),
        D3D11_SDK_VERSION,
        &device_,
        &featureLevel,
        &context_);

    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: D3D11CreateDevice failed (hr=0x%08lX)", hr);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Private: Desktop duplication setup
// ---------------------------------------------------------------------------

bool DxgiCapture::createDuplication() {
    if (!device_) return false;

    // Get the adapter and target output
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
    hr = adapter->EnumOutputs(static_cast<UINT>(targetOutputIdx_), &dxgiOutput);
    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: output %d not found", targetOutputIdx_);
        return false;
    }

    hr = dxgiOutput.As(&output_);
    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: failed to get IDXGIOutput1");
        return false;
    }

    // Store output description for resolution info
    dxgiOutput->GetDesc(&outputDesc_);

    // Create the duplication interface
    hr = output_->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: DuplicateOutput failed (hr=0x%08lX)", hr);
        return false;
    }

    // Create a staging texture for CPU readback
    DXGI_OUTDUPL_DESC duplDesc;
    duplication_->GetDesc(&duplDesc);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = duplDesc.ModeDesc.Width;
    texDesc.Height = duplDesc.ModeDesc.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = duplDesc.ModeDesc.Format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device_->CreateTexture2D(&texDesc, nullptr, &stagingTex_);
    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: failed to create staging texture (hr=0x%08lX)",
                  hr);
        return false;
    }

    return true;
}

void DxgiCapture::releaseDuplication() {
    if (duplication_) {
        // Release any held frame before destroying the duplication
        duplication_->ReleaseFrame();
        duplication_.Reset();
    }
    output_.Reset();
    stagingTex_.Reset();
}

// ---------------------------------------------------------------------------
// Private: Frame processing
// ---------------------------------------------------------------------------

bool DxgiCapture::processFrame(IDXGIResource* resource, Frame& frame) {
    // Get the desktop texture from the acquired resource
    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTex;
    HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&desktopTex));
    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: QueryInterface for Texture2D failed");
        return false;
    }

    // Copy to staging texture for CPU access
    context_->CopyResource(stagingTex_.Get(), desktopTex.Get());

    // Map the staging texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LOG_ERROR("DxgiCapture: Map staging texture failed (hr=0x%08lX)", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    stagingTex_->GetDesc(&desc);

    const int32_t w = static_cast<int32_t>(desc.Width);
    const int32_t h = static_cast<int32_t>(desc.Height);

    frame.allocate(w, h, PixelFormat::BGRA);

    // Copy row by row (source stride may differ from destination)
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = frame.data.data();
    const int32_t rowBytes = w * 4;

    for (int32_t row = 0; row < h; ++row) {
        std::memcpy(dst + row * frame.stride,
                     src + row * mapped.RowPitch,
                     rowBytes);
    }

    context_->Unmap(stagingTex_.Get(), 0);
    return true;
}

// ---------------------------------------------------------------------------
// Private: Dirty rects and move rects
// ---------------------------------------------------------------------------

std::vector<Rect> DxgiCapture::getDirtyRects(IDXGIOutputDuplication* dup,
                                              DXGI_OUTDUPL_FRAME_INFO& info) {
    std::vector<Rect> rects;

    if (info.TotalMetadataBufferSize == 0) {
        return rects;
    }

    // Ensure scratch buffer is large enough
    UINT dirtyBufSize = info.TotalMetadataBufferSize;
    dirtyRectBuf_.resize(dirtyBufSize);

    UINT numBytes = dirtyBufSize;
    HRESULT hr = dup->GetFrameDirtyRects(
        numBytes,
        reinterpret_cast<RECT*>(dirtyRectBuf_.data()),
        &numBytes);

    if (FAILED(hr)) {
        return rects;
    }

    UINT numRects = numBytes / sizeof(RECT);
    const RECT* pRects = reinterpret_cast<const RECT*>(dirtyRectBuf_.data());

    rects.reserve(numRects);
    for (UINT i = 0; i < numRects; ++i) {
        Rect r;
        r.x = pRects[i].left;
        r.y = pRects[i].top;
        r.width = pRects[i].right - pRects[i].left;
        r.height = pRects[i].bottom - pRects[i].top;
        if (!r.empty()) {
            rects.push_back(r);
        }
    }

    return rects;
}

std::vector<Rect> DxgiCapture::getMoveRects(IDXGIOutputDuplication* dup,
                                             DXGI_OUTDUPL_FRAME_INFO& info) {
    std::vector<Rect> rects;

    if (info.TotalMetadataBufferSize == 0) {
        return rects;
    }

    UINT moveBufSize = info.TotalMetadataBufferSize;
    moveRectBuf_.resize(moveBufSize);

    UINT numBytes = moveBufSize;
    HRESULT hr = dup->GetFrameMoveRects(
        numBytes,
        reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(moveRectBuf_.data()),
        &numBytes);

    if (FAILED(hr)) {
        return rects;
    }

    UINT numMoves = numBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
    const auto* pMoves =
        reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(moveRectBuf_.data());

    rects.reserve(numMoves);
    for (UINT i = 0; i < numMoves; ++i) {
        // Report the destination rectangle as dirty
        Rect r;
        r.x = pMoves[i].DestinationRect.left;
        r.y = pMoves[i].DestinationRect.top;
        r.width = pMoves[i].DestinationRect.right -
                  pMoves[i].DestinationRect.left;
        r.height = pMoves[i].DestinationRect.bottom -
                   pMoves[i].DestinationRect.top;
        if (!r.empty()) {
            rects.push_back(r);
        }
    }

    return rects;
}

// ---------------------------------------------------------------------------
// Private: Pointer shape capture
// ---------------------------------------------------------------------------

CursorInfo DxgiCapture::getPointerShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    CursorInfo cursor;
    cursor.visible = (info.PointerPosition.Visible != 0);
    cursor.x = info.PointerPosition.Position.x;
    cursor.y = info.PointerPosition.Position.y;

    if (info.PointerShapeBufferSize == 0) {
        // Shape hasn't changed; return cached data
        cursor.shapeChanged = false;
        cursor.width = cachedCursor_.width;
        cursor.height = cachedCursor_.height;
        cursor.hotspotX = cachedCursor_.hotspotX;
        cursor.hotspotY = cachedCursor_.hotspotY;
        cursor.imageData = cachedCursor_.imageData;
        cursor.shapeHash = cachedCursor_.shapeHash;
        return cursor;
    }

    // New pointer shape available
    pointerShapeBuf_.resize(info.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo = {};
    UINT bufSize = info.PointerShapeBufferSize;

    HRESULT hr = duplication_->GetFramePointerShape(
        bufSize,
        pointerShapeBuf_.data(),
        &bufSize,
        &shapeInfo);

    if (FAILED(hr)) {
        cursor.visible = false;
        return cursor;
    }

    cursor.width = static_cast<int32_t>(shapeInfo.Width);
    cursor.hotspotX = static_cast<int32_t>(shapeInfo.HotSpot.x);
    cursor.hotspotY = static_cast<int32_t>(shapeInfo.HotSpot.y);
    cursor.shapeChanged = true;

    if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        // 32-bit BGRA cursor image
        cursor.height = static_cast<int32_t>(shapeInfo.Height);
        const size_t imageSize =
            static_cast<size_t>(cursor.width) * cursor.height * 4;
        cursor.imageData.resize(imageSize);

        // Copy row by row to handle stride differences
        const uint8_t* src = pointerShapeBuf_.data();
        uint8_t* dst = cursor.imageData.data();
        const int32_t rowBytes = cursor.width * 4;
        for (int32_t row = 0; row < cursor.height; ++row) {
            std::memcpy(dst + row * rowBytes,
                         src + row * shapeInfo.Pitch,
                         rowBytes);
        }
    } else if (shapeInfo.Type ==
               DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Masked color cursor: top half is AND mask XOR'd with color
        cursor.height = static_cast<int32_t>(shapeInfo.Height);
        const size_t imageSize =
            static_cast<size_t>(cursor.width) * cursor.height * 4;
        cursor.imageData.resize(imageSize);

        const uint8_t* src = pointerShapeBuf_.data();
        uint8_t* dst = cursor.imageData.data();
        const int32_t rowBytes = cursor.width * 4;
        for (int32_t row = 0; row < cursor.height; ++row) {
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(
                src + row * shapeInfo.Pitch);
            uint32_t* dstRow =
                reinterpret_cast<uint32_t*>(dst + row * rowBytes);
            for (int32_t col = 0; col < cursor.width; ++col) {
                uint32_t pixel = srcRow[col];
                // If the high bit of alpha is set, this is an XOR pixel;
                // otherwise, treat it as a regular BGRA pixel with full alpha.
                if (pixel & 0xFF000000) {
                    // XOR pixel: keep RGB, set alpha to 0xFF for display
                    dstRow[col] = (pixel & 0x00FFFFFF) | 0xFF000000;
                } else {
                    dstRow[col] = pixel | 0xFF000000;
                }
            }
        }
    } else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Monochrome cursor: height is 2x actual (AND mask + XOR mask)
        cursor.height = static_cast<int32_t>(shapeInfo.Height / 2);
        const size_t imageSize =
            static_cast<size_t>(cursor.width) * cursor.height * 4;
        cursor.imageData.resize(imageSize);

        // Convert 1-bit monochrome AND/XOR to BGRA
        const int32_t andPitch = static_cast<int32_t>(shapeInfo.Pitch);
        const uint8_t* andMask = pointerShapeBuf_.data();
        const uint8_t* xorMask = andMask + andPitch * cursor.height;

        for (int32_t row = 0; row < cursor.height; ++row) {
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(
                cursor.imageData.data() +
                row * cursor.width * 4);
            for (int32_t col = 0; col < cursor.width; ++col) {
                int byteIdx = col / 8;
                int bitIdx = 7 - (col % 8);

                bool andBit =
                    (andMask[row * andPitch + byteIdx] >> bitIdx) & 1;
                bool xorBit =
                    (xorMask[row * andPitch + byteIdx] >> bitIdx) & 1;

                if (!andBit && !xorBit) {
                    // Black, fully opaque
                    dstRow[col] = 0xFF000000;
                } else if (!andBit && xorBit) {
                    // White, fully opaque
                    dstRow[col] = 0xFFFFFFFF;
                } else if (andBit && !xorBit) {
                    // Transparent
                    dstRow[col] = 0x00000000;
                } else {
                    // Inverted (XOR) — render as white semi-transparent
                    dstRow[col] = 0x80FFFFFF;
                }
            }
        }
    }

    // Compute shape hash for caching
    uint64_t hash = 14695981039346656037ULL;  // FNV-1a offset basis
    for (size_t i = 0; i < cursor.imageData.size(); ++i) {
        hash ^= cursor.imageData[i];
        hash *= 1099511628211ULL;  // FNV prime
    }
    cursor.shapeHash = hash;

    // Cache the shape for frames where it doesn't change
    cachedCursor_ = cursor;

    return cursor;
}

// ---------------------------------------------------------------------------
// Private: Desktop switch / resolution change recovery
// ---------------------------------------------------------------------------

bool DxgiCapture::handleDesktopSwitch() {
    LOG_INFO("DxgiCapture: reinitialising after desktop switch / mode change");

    releaseDuplication();

    // Wait briefly for the desktop to become available
    // (UAC prompt or lock screen may keep it locked for a while)
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (createDuplication()) {
            needReinit_ = false;
            LOG_INFO("DxgiCapture: reinitialisation succeeded");
            return true;
        }
        ::Sleep(200);
    }

    LOG_ERROR("DxgiCapture: reinitialisation failed after retries");
    return false;
}

} // namespace omnidesk

#endif // OMNIDESK_PLATFORM_WINDOWS
