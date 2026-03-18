#include "codec/mf_encoder.h"
#include "core/logger.h"

#ifdef OMNIDESK_PLATFORM_WINDOWS

#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace omnidesk {

// ---- MFEncoder ----

MFEncoder::MFEncoder() = default;

MFEncoder::~MFEncoder() {
    destroy();
}

// Try to identify the vendor behind the activated MFT by querying its
// friendly name from the IMFAttributes on the transform.
void MFEncoder::identifyMFT() {
    if (!transform_) return;

    // Query IMFAttributes for MFT_FRIENDLY_NAME_Attribute
    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    HRESULT hr = transform_->GetAttributes(&attrs);
    if (SUCCEEDED(hr) && attrs) {
        WCHAR name[256] = {};
        UINT32 len = 0;
        // MFT_FRIENDLY_NAME_Attribute = {314FFBAE-5B41-4C95-9C19-4E7D586FACE3}
        static const GUID MFT_FRIENDLY_NAME =
            {0x314FFBAE, 0x5B41, 0x4C95, {0x9C, 0x19, 0x4E, 0x7D, 0x58, 0x6F, 0xAC, 0xE3}};
        hr = attrs->GetString(MFT_FRIENDLY_NAME, name, 256, &len);
        if (SUCCEEDED(hr) && len > 0) {
            // Convert wide string to narrow for logging
            char narrow[256] = {};
            for (UINT32 i = 0; i < len && i < 255; ++i)
                narrow[i] = static_cast<char>(name[i]); // ASCII subset
            LOG_INFO("MF: MFT friendly name: %s", narrow);

            std::string n(narrow);
            if (n.find("Intel") != std::string::npos ||
                n.find("QSV") != std::string::npos) {
                mftName_ = "Intel QSV (H.264)";
            } else if (n.find("AMD") != std::string::npos ||
                       n.find("AMF") != std::string::npos) {
                mftName_ = "AMD AMF (H.264)";
            } else if (n.find("NVIDIA") != std::string::npos ||
                       n.find("NVENC") != std::string::npos) {
                mftName_ = "NVENC via MF (H.264)";
            } else {
                // Generic — still report hardware vs software
                if (isHardware_)
                    mftName_ = "MF Hardware (H.264)";
                else
                    mftName_ = "MF Software (H.264)";
            }
            return;
        }
    }

    // Fallback naming when friendly-name is unavailable
    mftName_ = isHardware_ ? "MF Hardware (H.264)" : "MF Software (H.264)";
}

bool MFEncoder::findEncoderMFT() {
    HRESULT hr;

    // Initialize Media Foundation
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        LOG_ERROR("MF: MFStartup failed: 0x%08lx", hr);
        return false;
    }

    // We do NOT restrict the input subtype in the enumeration query.
    // Some hardware MFTs only accept NV12, others accept I420.
    // By passing nullptr for inputType we get all H.264 encoders and
    // negotiate the input format in configureMFT().
    MFT_REGISTER_TYPE_INFO outputType = {MFMediaType_Video, MFVideoFormat_H264};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    // --- Try 1: hardware MFTs ---
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                    nullptr, &outputType, &activates, &count);

    if (SUCCEEDED(hr) && count > 0) {
        // Try each hardware MFT until one activates
        for (UINT32 i = 0; i < count; ++i) {
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&transform_));
            if (SUCCEEDED(hr)) {
                isHardware_ = true;
                LOG_INFO("MF: Activated hardware H.264 encoder (index %u of %u)", i, count);
                break;
            }
        }
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        activates = nullptr;
        count = 0;

        if (transform_) {
            identifyMFT();
            return true;
        }
    } else {
        if (activates) { CoTaskMemFree(activates); activates = nullptr; }
    }

    // --- Try 2: synchronous software MFTs ---
    LOG_INFO("MF: No hardware encoder found, trying software MFTs");
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                    MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                    nullptr, &outputType, &activates, &count);

    if (SUCCEEDED(hr) && count > 0) {
        for (UINT32 i = 0; i < count; ++i) {
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&transform_));
            if (SUCCEEDED(hr)) {
                isHardware_ = false;
                LOG_INFO("MF: Activated software H.264 encoder (index %u of %u)", i, count);
                break;
            }
        }
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        activates = nullptr;

        if (transform_) {
            identifyMFT();
            return true;
        }
    } else {
        if (activates) CoTaskMemFree(activates);
    }

    LOG_WARN("MF: No H.264 encoder MFT found (hardware or software)");
    return false;
}

bool MFEncoder::tryInputSubtype(const GUID& subtype) {
    if (!transform_) return false;

    Microsoft::WRL::ComPtr<IMFMediaType> testType;
    HRESULT hr = MFCreateMediaType(&testType);
    if (FAILED(hr)) return false;

    testType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    testType->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(testType.Get(), MF_MT_FRAME_SIZE,
                       config_.width, config_.height);
    MFSetAttributeRatio(testType.Get(), MF_MT_FRAME_RATE,
                        static_cast<UINT32>(config_.maxFps), 1);
    testType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(testType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    testType->SetUINT32(MF_MT_AVG_BITRATE, config_.targetBitrateBps);

    // Test without committing (dwFlags = MFT_SET_TYPE_TEST_ONLY)
    hr = transform_->SetInputType(inputStreamId_, testType.Get(),
                                   MFT_SET_TYPE_TEST_ONLY);
    if (SUCCEEDED(hr)) {
        // Commit
        hr = transform_->SetInputType(inputStreamId_, testType.Get(), 0);
        if (SUCCEEDED(hr)) {
            inputType_ = testType;
            return true;
        }
    }

    return false;
}

bool MFEncoder::configureMFT() {
    if (!transform_) return false;
    HRESULT hr;

    // ---- Output type (H.264) — must be set BEFORE input type ----
    hr = MFCreateMediaType(&outputType_);
    if (FAILED(hr)) return false;

    outputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType_->SetUINT32(MF_MT_AVG_BITRATE, config_.targetBitrateBps);
    MFSetAttributeSize(outputType_.Get(), MF_MT_FRAME_SIZE,
                       config_.width, config_.height);
    MFSetAttributeRatio(outputType_.Get(), MF_MT_FRAME_RATE,
                        static_cast<UINT32>(config_.maxFps), 1);
    outputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(outputType_.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outputType_->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

    hr = transform_->SetOutputType(outputStreamId_, outputType_.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: SetOutputType failed: 0x%08lx", hr);
        return false;
    }

    // ---- Input type — try NV12 first, then I420 ----
    // Many hardware MFTs prefer NV12; some (especially software MFTs) prefer I420.
    if (tryInputSubtype(MFVideoFormat_NV12)) {
        inputIsNV12_ = true;
        LOG_INFO("MF: Using NV12 input format");
    } else if (tryInputSubtype(MFVideoFormat_I420)) {
        inputIsNV12_ = false;
        LOG_INFO("MF: Using I420 input format");
    } else {
        // Last resort: try YUY2 (some older MFTs)
        if (tryInputSubtype(MFVideoFormat_YUY2)) {
            // We'll still convert to NV12 path internally; this is rare.
            inputIsNV12_ = true;
            LOG_WARN("MF: MFT accepted YUY2 — using NV12 conversion path");
        } else {
            LOG_ERROR("MF: SetInputType failed for NV12, I420, and YUY2");
            return false;
        }
    }

    // ---- Codec API tuning (low-latency, CBR) ----
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    hr = transform_.As(&codecApi);
    if (SUCCEEDED(hr) && codecApi) {
        VARIANT val;
        VariantInit(&val);

        // Low-latency mode
        val.vt = VT_BOOL;
        val.boolVal = VARIANT_TRUE;
        hr = codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &val);
        if (FAILED(hr))
            LOG_DEBUG("MF: CODECAPI_AVLowLatencyMode not supported (0x%08lx)", hr);

        // CBR rate control
        val.vt = VT_UI4;
        val.ulVal = eAVEncCommonRateControlMode_CBR;
        hr = codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &val);
        if (FAILED(hr))
            LOG_DEBUG("MF: CBR rate control not supported (0x%08lx)", hr);

        // Mean bitrate
        val.vt = VT_UI4;
        val.ulVal = config_.targetBitrateBps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &val);
    }

    // ---- Start streaming ----
    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: BEGIN_STREAMING failed: 0x%08lx", hr);
        return false;
    }

    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: START_OF_STREAM failed: 0x%08lx", hr);
        return false;
    }

    return true;
}

bool MFEncoder::createInputSample(const Frame& frame) {
    HRESULT hr;

    // Create sample and buffer
    Microsoft::WRL::ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    DWORD bufSize = static_cast<DWORD>(config_.width) * config_.height * 3 / 2;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(bufSize, &buffer);
    if (FAILED(hr)) return false;

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return false;

    const uint8_t* yPlane = frame.plane(0);
    const uint8_t* uPlane = frame.plane(1);
    const uint8_t* vPlane = frame.plane(2);

    if (inputIsNV12_) {
        // Convert I420 input to NV12: Y plane + interleaved UV
        for (int y = 0; y < config_.height; ++y) {
            std::memcpy(dst + y * config_.width,
                        yPlane + y * frame.stride, config_.width);
        }

        BYTE* uvDst = dst + config_.width * config_.height;
        int uvH = config_.height / 2;
        int uvW = config_.width / 2;
        for (int y = 0; y < uvH; ++y) {
            for (int x = 0; x < uvW; ++x) {
                uvDst[y * config_.width + x * 2 + 0] =
                    uPlane[y * (frame.stride / 2) + x];
                uvDst[y * config_.width + x * 2 + 1] =
                    vPlane[y * (frame.stride / 2) + x];
            }
        }
    } else {
        // I420 input: copy Y, U, V planes directly
        int ySize = config_.width * config_.height;
        int uvSize = (config_.width / 2) * (config_.height / 2);
        for (int y = 0; y < config_.height; ++y) {
            std::memcpy(dst + y * config_.width,
                        yPlane + y * frame.stride, config_.width);
        }
        BYTE* uDst = dst + ySize;
        BYTE* vDst = uDst + uvSize;
        int halfStride = frame.stride / 2;
        int halfW = config_.width / 2;
        int halfH = config_.height / 2;
        for (int y = 0; y < halfH; ++y) {
            std::memcpy(uDst + y * halfW, uPlane + y * halfStride, halfW);
            std::memcpy(vDst + y * halfW, vPlane + y * halfStride, halfW);
        }
    }

    buffer->Unlock();
    buffer->SetCurrentLength(bufSize);

    // Set sample timestamp and duration
    LONGLONG ts = static_cast<LONGLONG>(frameIndex_) * 10000000LL /
                  static_cast<LONGLONG>(config_.maxFps);
    sample->SetSampleTime(ts);
    sample->SetSampleDuration(10000000LL / static_cast<LONGLONG>(config_.maxFps));

    inputSample_ = sample;
    inputBuffer_ = buffer;
    return true;
}

bool MFEncoder::init(const EncoderConfig& cfg) {
    config_ = cfg;

    if (!findEncoderMFT()) {
        return false;
    }

    // Get stream IDs
    DWORD inCount = 0, outCount = 0;
    HRESULT hr = transform_->GetStreamCount(&inCount, &outCount);
    if (FAILED(hr) || inCount == 0 || outCount == 0) {
        destroy();
        return false;
    }

    hr = transform_->GetStreamIDs(1, &inputStreamId_, 1, &outputStreamId_);
    if (hr == E_NOTIMPL) {
        // Default stream IDs (most MFTs use 0)
        inputStreamId_ = 0;
        outputStreamId_ = 0;
    }

    if (!configureMFT()) {
        destroy();
        return false;
    }

    initialized_ = true;
    frameIndex_ = 0;
    keyFrameRequested_ = true;
    LOG_INFO("MF encoder initialized: %s — %dx%d @ %d kbps (%s input)",
             mftName_.c_str(), cfg.width, cfg.height,
             cfg.targetBitrateBps / 1000,
             inputIsNV12_ ? "NV12" : "I420");
    return true;
}

bool MFEncoder::encode(const Frame& frame,
                        const std::vector<RegionInfo>& /*regions*/,
                        EncodedPacket& out) {
    if (!initialized_) return false;

    // Force IDR if requested
    if (keyFrameRequested_.exchange(false)) {
        Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(transform_.As(&codecApi))) {
            VARIANT val;
            VariantInit(&val);
            val.vt = VT_UI4;
            val.ulVal = 1;
            codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &val);
        }
    }

    if (!createInputSample(frame)) return false;

    // Feed input
    HRESULT hr = transform_->ProcessInput(inputStreamId_, inputSample_.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: ProcessInput failed: 0x%08lx", hr);
        inputSample_.Reset();
        inputBuffer_.Reset();
        return false;
    }

    // Get output
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    hr = transform_->GetOutputStreamInfo(outputStreamId_, &streamInfo);

    MFT_OUTPUT_DATA_BUFFER outputData{};
    outputData.dwStreamID = outputStreamId_;

    // Allocate output sample if MFT does not provide its own
    Microsoft::WRL::ComPtr<IMFSample> outSampleHolder;
    if (SUCCEEDED(hr) && !(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        MFCreateSample(&outSampleHolder);
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
        DWORD outBufSize = streamInfo.cbSize ? streamInfo.cbSize
                             : static_cast<DWORD>(config_.width) * config_.height;
        MFCreateMemoryBuffer(outBufSize, &outBuf);
        outSampleHolder->AddBuffer(outBuf.Get());
        outputData.pSample = outSampleHolder.Get();
    }

    DWORD status = 0;
    hr = transform_->ProcessOutput(0, 1, &outputData, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        // Encoder has buffered the frame but needs more before producing output.
        // This is normal for the first few frames — not an error.
        inputSample_.Reset();
        inputBuffer_.Reset();
        frameIndex_++;
        return false;
    }

    if (FAILED(hr)) {
        // Release MFT-provided sample if applicable
        if (outputData.pSample && (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            outputData.pSample->Release();
        }
        inputSample_.Reset();
        inputBuffer_.Reset();
        LOG_ERROR("MF: ProcessOutput failed: 0x%08lx", hr);
        return false;
    }

    // Extract encoded data from output sample
    IMFSample* resultSample = outputData.pSample;
    if (!resultSample) {
        inputSample_.Reset();
        inputBuffer_.Reset();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
    hr = resultSample->ConvertToContiguousBuffer(&outBuffer);
    if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        DWORD len = 0;
        hr = outBuffer->Lock(&data, nullptr, &len);
        if (SUCCEEDED(hr) && len > 0) {
            out.data.assign(data, data + len);
            outBuffer->Unlock();
        }
    }

    // Release MFT-provided sample
    if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) && outputData.pSample) {
        outputData.pSample->Release();
    }

    out.frameId = frameIndex_;
    out.isKeyFrame = (frameIndex_ == 0); // first frame is always key
    out.temporalLayer = 0;
    out.timestampUs = frame.timestampUs;

    // Check for key frame by inspecting sample attributes
    if (resultSample) {
        UINT32 cleanPoint = 0;
        if (SUCCEEDED(resultSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint))) {
            if (cleanPoint) out.isKeyFrame = true;
        }
    }

    inputSample_.Reset();
    inputBuffer_.Reset();

    frameIndex_++;
    return !out.data.empty();
}

void MFEncoder::requestKeyFrame() {
    keyFrameRequested_ = true;
}

void MFEncoder::updateBitrate(uint32_t bps) {
    config_.targetBitrateBps = bps;
    if (!transform_) return;

    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(transform_.As(&codecApi))) {
        VARIANT val;
        VariantInit(&val);
        val.vt = VT_UI4;
        val.ulVal = bps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &val);
    }
}

EncoderInfo MFEncoder::getInfo() {
    EncoderInfo info;
    info.name = mftName_.empty() ? "Media Foundation H.264" : mftName_;
    info.isHardware = isHardware_;
    info.maxWidth = config_.width;
    info.maxHeight = config_.height;
    return info;
}

void MFEncoder::destroy() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    inputSample_.Reset();
    inputBuffer_.Reset();
    inputType_.Reset();
    outputType_.Reset();
    transform_.Reset();
    if (initialized_) {
        MFShutdown();
    }
    initialized_ = false;
}

// ---- MFDecoder ----

MFDecoder::MFDecoder() = default;
MFDecoder::~MFDecoder() { destroy(); }

bool MFDecoder::configureTypes() {
    if (!transform_) return false;

    // --- Input type: H.264 ---
    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    HRESULT hr = MFCreateMediaType(&inType);
    if (FAILED(hr)) return false;

    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 30, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF decoder: SetInputType failed: 0x%08lx", hr);
        return false;
    }

    // --- Output type: try NV12 first (most common), then I420 ---
    // Some MFTs do not support I420 output; NV12 is virtually universal.
    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(&outType);
    if (FAILED(hr)) return false;

    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, 30, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = transform_->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        // Fallback: let the MFT choose its preferred output type
        LOG_WARN("MF decoder: SetOutputType(NV12) failed 0x%08lx, trying MFT preferred", hr);
        for (DWORD idx = 0; ; ++idx) {
            Microsoft::WRL::ComPtr<IMFMediaType> preferred;
            hr = transform_->GetOutputAvailableType(0, idx, &preferred);
            if (FAILED(hr)) break;

            GUID subtype;
            preferred->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_I420 ||
                subtype == MFVideoFormat_YV12) {
                hr = transform_->SetOutputType(0, preferred.Get(), 0);
                if (SUCCEEDED(hr)) {
                    LOG_INFO("MF decoder: Using MFT preferred output type (index %u)", idx);
                    break;
                }
            }
        }
        if (FAILED(hr)) {
            LOG_ERROR("MF decoder: Could not set any output type");
            return false;
        }
    }

    typesConfigured_ = true;
    return true;
}

bool MFDecoder::handleFormatChange() {
    // The MFT is signaling a format change (e.g. resolution change from SPS).
    // Re-negotiate the output type.
    HRESULT hr;

    // Query the new output type the MFT wants
    Microsoft::WRL::ComPtr<IMFMediaType> newType;
    hr = transform_->GetOutputAvailableType(0, 0, &newType);
    if (FAILED(hr)) return false;

    hr = transform_->SetOutputType(0, newType.Get(), 0);
    if (FAILED(hr)) return false;

    // Update width/height from the new type
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(newType.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (w > 0 && h > 0) {
        width_ = static_cast<int>(w);
        height_ = static_cast<int>(h);
        LOG_INFO("MF decoder: Format change — new resolution %dx%d", width_, height_);
    }

    return true;
}

bool MFDecoder::init(int width, int height) {
    // Default to 1920x1080 if caller doesn't know yet — the MFT will
    // re-negotiate via MF_E_TRANSFORM_STREAM_CHANGE when it parses the SPS.
    width_ = (width > 0) ? width : 1920;
    height_ = (height > 0) ? height : 1080;
    inputCount_ = 0;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        LOG_ERROR("MF decoder: MFStartup failed: 0x%08lx", hr);
        return false;
    }

    // Find H.264 decoder MFT
    MFT_REGISTER_TYPE_INFO inputType = {MFMediaType_Video, MFVideoFormat_H264};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    // Try hardware decoder first
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                    &inputType, nullptr, &activates, &count);

    if (FAILED(hr) || count == 0) {
        if (activates) { CoTaskMemFree(activates); activates = nullptr; }
        // Try synchronous software decoder
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                        &inputType, nullptr, &activates, &count);
    }

    if (FAILED(hr) || count == 0) {
        if (activates) { CoTaskMemFree(activates); activates = nullptr; }
        // Try async software decoder (some Windows versions only have async)
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                        MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                        &inputType, nullptr, &activates, &count);
    }

    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        LOG_ERROR("MF decoder: No H.264 decoder MFT found");
        return false;
    }

    // Try to activate each MFT until one works
    bool activated = false;
    for (UINT32 i = 0; i < count; ++i) {
        hr = activates[i]->ActivateObject(IID_PPV_ARGS(&transform_));
        if (SUCCEEDED(hr)) {
            activated = true;
            LOG_INFO("MF decoder: Activated decoder MFT (index %u of %u)", i, count);
            break;
        }
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);

    if (!activated || !transform_) {
        LOG_ERROR("MF decoder: Failed to activate any decoder MFT");
        return false;
    }

    if (!configureTypes()) {
        destroy();
        return false;
    }

    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        LOG_WARN("MF decoder: BEGIN_STREAMING failed: 0x%08lx (continuing)", hr);
    }
    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        LOG_WARN("MF decoder: START_OF_STREAM failed: 0x%08lx (continuing)", hr);
    }

    initialized_ = true;
    LOG_INFO("MF decoder initialized: %dx%d", width_, height_);
    return true;
}

bool MFDecoder::decode(const uint8_t* data, size_t size, Frame& out) {
    if (!initialized_ || !transform_) return false;

    ++inputCount_;

    // Create input sample with the H.264 NAL data
    Microsoft::WRL::ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf);
    BYTE* dst = nullptr;
    buf->Lock(&dst, nullptr, nullptr);
    std::memcpy(dst, data, size);
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(size));
    sample->AddBuffer(buf.Get());

    // Set timestamp (monotonically increasing)
    sample->SetSampleTime(static_cast<LONGLONG>(inputCount_) * 333333LL); // ~30fps

    HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        if (inputCount_ <= 5)
            LOG_WARN("MF decode: ProcessInput failed: 0x%08lx (frame %llu)",
                     hr, (unsigned long long)inputCount_);
        return false;
    }

    // Try to get output — loop to handle format changes
    for (int attempt = 0; attempt < 3; ++attempt) {
        MFT_OUTPUT_DATA_BUFFER outputData{};
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        transform_->GetOutputStreamInfo(0, &streamInfo);

        Microsoft::WRL::ComPtr<IMFSample> outSample;
        bool weOwnSample = false;
        if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            MFCreateSample(&outSample);
            Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
            DWORD bufSize = streamInfo.cbSize ? streamInfo.cbSize
                                : static_cast<DWORD>(width_) * height_ * 3 / 2;
            if (bufSize == 0) bufSize = 1920 * 1080 * 3 / 2;
            MFCreateMemoryBuffer(bufSize, &outBuf);
            outSample->AddBuffer(outBuf.Get());
            outputData.pSample = outSample.Get();
            weOwnSample = true;
        }

        DWORD status = 0;
        hr = transform_->ProcessOutput(0, 1, &outputData, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // The decoder needs more NAL units before it can produce a frame.
            // This is expected for SPS/PPS-only packets and B-frame buffering.
            return false;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Resolution or format changed — re-negotiate and retry
            if (!handleFormatChange()) {
                LOG_ERROR("MF decoder: Failed to handle format change");
                return false;
            }
            continue; // retry ProcessOutput
        }

        if (FAILED(hr)) {
            if (inputCount_ <= 10)
                LOG_WARN("MF decode: ProcessOutput failed: 0x%08lx (frame %llu)",
                         hr, (unsigned long long)inputCount_);
            return false;
        }

        // Successfully got output — extract NV12 frame and convert to I420
        IMFSample* resultSample = outputData.pSample;
        if (!resultSample) return false;

        // Check if MFT changed the resolution in the output sample
        Microsoft::WRL::ComPtr<IMFMediaType> currentOutType;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &currentOutType))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(currentOutType.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w > 0 && h > 0 && (static_cast<int>(w) != width_ ||
                                     static_cast<int>(h) != height_)) {
                width_ = static_cast<int>(w);
                height_ = static_cast<int>(h);
                LOG_INFO("MF decoder: Actual output resolution %dx%d", width_, height_);
            }
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
        resultSample->ConvertToContiguousBuffer(&outBuffer);
        BYTE* srcData = nullptr;
        DWORD srcLen = 0;
        outBuffer->Lock(&srcData, nullptr, &srcLen);

        out.allocate(width_, height_, PixelFormat::I420);

        // Copy Y plane
        for (int y = 0; y < height_; ++y) {
            std::memcpy(out.plane(0) + y * out.stride,
                        srcData + y * width_, width_);
        }
        // De-interleave NV12 UV to I420 U + V
        const BYTE* uvSrc = srcData + width_ * height_;
        int uvH = height_ / 2;
        int uvW = width_ / 2;
        for (int y = 0; y < uvH; ++y) {
            for (int x = 0; x < uvW; ++x) {
                out.plane(1)[y * (out.stride / 2) + x] =
                    uvSrc[y * width_ + x * 2];
                out.plane(2)[y * (out.stride / 2) + x] =
                    uvSrc[y * width_ + x * 2 + 1];
            }
        }

        outBuffer->Unlock();

        // Release MFT-provided sample if applicable
        if (!weOwnSample && outputData.pSample) {
            outputData.pSample->Release();
        }

        return true;
    }

    return false;
}

void MFDecoder::reset() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    inputCount_ = 0;
}

void MFDecoder::destroy() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        transform_.Reset();
    }
    if (initialized_) {
        MFShutdown();
    }
    initialized_ = false;
    typesConfigured_ = false;
}

} // namespace omnidesk

#endif // OMNIDESK_PLATFORM_WINDOWS
