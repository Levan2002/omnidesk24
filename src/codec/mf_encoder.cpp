#include "codec/mf_encoder.h"
#include "core/logger.h"

#ifdef OMNIDESK_PLATFORM_WINDOWS

#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
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

bool MFEncoder::findHardwareMFT() {
    HRESULT hr;

    // Initialize Media Foundation
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        LOG_ERROR("MF: MFStartup failed: 0x%08lx", hr);
        return false;
    }

    // Enumerate H.264 hardware encoder MFTs
    MFT_REGISTER_TYPE_INFO inputType = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outputType = {MFMediaType_Video, MFVideoFormat_H264};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                    &inputType, &outputType, &activates, &count);

    if (FAILED(hr) || count == 0) {
        LOG_WARN("MF: No hardware H.264 encoder found");
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    // Activate the first hardware encoder
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));

    // Clean up activates
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        LOG_ERROR("MF: Failed to activate encoder MFT");
        return false;
    }

    LOG_INFO("MF: Hardware H.264 encoder found");
    return true;
}

bool MFEncoder::configureMFT() {
    if (!transform_) return false;
    HRESULT hr;

    // Set output type (H.264)
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
    outputType_->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

    hr = transform_->SetOutputType(outputStreamId_, outputType_.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: SetOutputType failed: 0x%08lx", hr);
        return false;
    }

    // Set input type (NV12)
    hr = MFCreateMediaType(&inputType_);
    if (FAILED(hr)) return false;

    inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inputType_.Get(), MF_MT_FRAME_SIZE,
                       config_.width, config_.height);
    MFSetAttributeRatio(inputType_.Get(), MF_MT_FRAME_RATE,
                        static_cast<UINT32>(config_.maxFps), 1);
    inputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = transform_->SetInputType(inputStreamId_, inputType_.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: SetInputType failed: 0x%08lx", hr);
        return false;
    }

    // Enable low-latency mode
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    hr = transform_.As(&codecApi);
    if (SUCCEEDED(hr)) {
        VARIANT val;
        val.vt = VT_BOOL;
        val.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &val);

        // Set CBR rate control
        val.vt = VT_UI4;
        val.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &val);
    }

    // Start processing
    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;

    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return SUCCEEDED(hr);
}

bool MFEncoder::createInputSample(const Frame& frame) {
    HRESULT hr;

    // Create sample and buffer
    hr = MFCreateSample(&inputSample_);
    if (FAILED(hr)) return false;

    DWORD bufSize = config_.width * config_.height * 3 / 2; // NV12
    hr = MFCreateMemoryBuffer(bufSize, &inputBuffer_);
    if (FAILED(hr)) return false;

    hr = inputSample_->AddBuffer(inputBuffer_.Get());
    if (FAILED(hr)) return false;

    // Copy I420 to NV12 in the buffer
    BYTE* dst = nullptr;
    hr = inputBuffer_->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return false;

    const uint8_t* yPlane = frame.plane(0);
    const uint8_t* uPlane = frame.plane(1);
    const uint8_t* vPlane = frame.plane(2);

    // Y plane
    for (int y = 0; y < config_.height; ++y) {
        std::memcpy(dst + y * config_.width,
                    yPlane + y * frame.stride, config_.width);
    }

    // Interleave U/V for NV12
    BYTE* uvDst = dst + config_.width * config_.height;
    int uvH = config_.height / 2;
    int uvW = config_.width / 2;
    for (int y = 0; y < uvH; ++y) {
        for (int x = 0; x < uvW; ++x) {
            uvDst[y * config_.width + x * 2 + 0] = uPlane[y * (frame.stride / 2) + x];
            uvDst[y * config_.width + x * 2 + 1] = vPlane[y * (frame.stride / 2) + x];
        }
    }

    inputBuffer_->Unlock();
    inputBuffer_->SetCurrentLength(bufSize);

    // Set sample timestamp
    LONGLONG ts = static_cast<LONGLONG>(frameIndex_) * 10000000LL /
                  static_cast<LONGLONG>(config_.maxFps);
    inputSample_->SetSampleTime(ts);
    inputSample_->SetSampleDuration(10000000LL / static_cast<LONGLONG>(config_.maxFps));

    return true;
}

bool MFEncoder::init(const EncoderConfig& cfg) {
    config_ = cfg;

    if (!findHardwareMFT()) {
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
        // Default stream IDs
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
    LOG_INFO("MF encoder initialized: %dx%d @ %d kbps",
             cfg.width, cfg.height, cfg.targetBitrateBps / 1000);
    return true;
}

bool MFEncoder::encode(const Frame& frame,
                        const std::vector<RegionInfo>& /*regions*/,
                        EncodedPacket& out) {
    if (!initialized_) return false;

    // Force IDR if requested
    if (keyFrameRequested_) {
        Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(transform_.As(&codecApi))) {
            VARIANT val;
            val.vt = VT_UI4;
            val.ulVal = 1;
            codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &val);
        }
        keyFrameRequested_ = false;
    }

    if (!createInputSample(frame)) return false;

    // Feed input
    HRESULT hr = transform_->ProcessInput(inputStreamId_, inputSample_.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("MF: ProcessInput failed: 0x%08lx", hr);
        return false;
    }

    // Get output
    MFT_OUTPUT_DATA_BUFFER outputData{};
    outputData.dwStreamID = outputStreamId_;

    // Check if we need to allocate the output sample
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    hr = transform_->GetOutputStreamInfo(outputStreamId_, &streamInfo);
    if (SUCCEEDED(hr) && !(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        Microsoft::WRL::ComPtr<IMFSample> outSample;
        MFCreateSample(&outSample);
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
        MFCreateMemoryBuffer(streamInfo.cbSize, &outBuf);
        outSample->AddBuffer(outBuf.Get());
        outputData.pSample = outSample.Get();
    }

    DWORD status = 0;
    hr = transform_->ProcessOutput(0, 1, &outputData, &status);
    if (FAILED(hr)) {
        if (outputData.pSample && !(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            outputData.pSample->Release();
        }
        return false;
    }

    // Extract encoded data
    Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
    hr = outputData.pSample->ConvertToContiguousBuffer(&outBuffer);
    if (SUCCEEDED(hr)) {
        BYTE* data = nullptr;
        DWORD len = 0;
        hr = outBuffer->Lock(&data, nullptr, &len);
        if (SUCCEEDED(hr)) {
            out.data.assign(data, data + len);
            outBuffer->Unlock();
        }
    }

    out.frameId = frameIndex_;
    out.isKeyFrame = (frameIndex_ == 0) || keyFrameRequested_;
    out.temporalLayer = 0;
    out.timestampUs = frame.timestampUs;

    if (outputData.pSample && !(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        outputData.pSample->Release();
    }

    // Clear input sample for next frame
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
        val.vt = VT_UI4;
        val.ulVal = bps;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &val);
    }
}

EncoderInfo MFEncoder::getInfo() {
    EncoderInfo info;
    info.name = "Media Foundation H.264";
    info.isHardware = true;
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
    MFShutdown();
    initialized_ = false;
}

// ---- MFDecoder ----

MFDecoder::MFDecoder() = default;
MFDecoder::~MFDecoder() { destroy(); }

bool MFDecoder::init(int width, int height) {
    width_ = width;
    height_ = height;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return false;

    // Find H.264 decoder MFT
    MFT_REGISTER_TYPE_INFO inputType = {MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO outputType = {MFMediaType_Video, MFVideoFormat_NV12};

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                    &inputType, &outputType, &activates, &count);

    if (FAILED(hr) || count == 0) {
        // Try software decoder
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                        &inputType, &outputType, &activates, &count);
    }

    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return false;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);

    if (FAILED(hr)) return false;

    // Configure input/output types
    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
    transform_->SetInputType(0, inType.Get(), 0);

    Microsoft::WRL::ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height);
    transform_->SetOutputType(0, outType.Get(), 0);

    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    initialized_ = true;
    LOG_INFO("MF decoder initialized: %dx%d", width, height);
    return true;
}

bool MFDecoder::decode(const uint8_t* data, size_t size, Frame& out) {
    if (!initialized_ || !transform_) return false;

    // Create input sample
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

    HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) return false;

    // Get output
    MFT_OUTPUT_DATA_BUFFER outputData{};
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    transform_->GetOutputStreamInfo(0, &streamInfo);

    Microsoft::WRL::ComPtr<IMFSample> outSample;
    if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        MFCreateSample(&outSample);
        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
        MFCreateMemoryBuffer(streamInfo.cbSize ? streamInfo.cbSize : width_ * height_ * 3 / 2, &outBuf);
        outSample->AddBuffer(outBuf.Get());
        outputData.pSample = outSample.Get();
    }

    DWORD status = 0;
    hr = transform_->ProcessOutput(0, 1, &outputData, &status);
    if (FAILED(hr)) return false;

    // Extract NV12 → I420
    Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuffer;
    outputData.pSample->ConvertToContiguousBuffer(&outBuffer);
    BYTE* srcData = nullptr;
    DWORD srcLen = 0;
    outBuffer->Lock(&srcData, nullptr, &srcLen);

    out.allocate(width_, height_, PixelFormat::I420);
    // Copy Y
    for (int y = 0; y < height_; ++y) {
        std::memcpy(out.plane(0) + y * out.stride, srcData + y * width_, width_);
    }
    // De-interleave NV12 UV → I420 U + V
    const BYTE* uvSrc = srcData + width_ * height_;
    int uvH = height_ / 2;
    int uvW = width_ / 2;
    for (int y = 0; y < uvH; ++y) {
        for (int x = 0; x < uvW; ++x) {
            out.plane(1)[y * (out.stride / 2) + x] = uvSrc[y * width_ + x * 2];
            out.plane(2)[y * (out.stride / 2) + x] = uvSrc[y * width_ + x * 2 + 1];
        }
    }

    outBuffer->Unlock();
    return true;
}

void MFDecoder::reset() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
}

void MFDecoder::destroy() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform_.Reset();
    }
    MFShutdown();
    initialized_ = false;
}

} // namespace omnidesk

#endif // OMNIDESK_PLATFORM_WINDOWS
