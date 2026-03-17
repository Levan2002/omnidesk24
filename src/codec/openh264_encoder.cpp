#include "codec/openh264_encoder.h"
#include "core/logger.h"

#include "codec/openh264_loader.h"
#include <wels/codec_def.h>
#include <cstring>
#include <algorithm>

namespace omnidesk {

OpenH264Encoder::OpenH264Encoder() = default;

OpenH264Encoder::~OpenH264Encoder() {
    destroy();
}

OpenH264Encoder::OpenH264Encoder(OpenH264Encoder&& other) noexcept
    : encoder_(other.encoder_)
    , config_(other.config_)
    , keyFrameRequested_(other.keyFrameRequested_.load())
    , frameIndex_(other.frameIndex_) {
    other.encoder_ = nullptr;
}

OpenH264Encoder& OpenH264Encoder::operator=(OpenH264Encoder&& other) noexcept {
    if (this != &other) {
        destroy();
        encoder_ = other.encoder_;
        config_ = other.config_;
        keyFrameRequested_.store(other.keyFrameRequested_.load());
        frameIndex_ = other.frameIndex_;
        other.encoder_ = nullptr;
    }
    return *this;
}

void OpenH264Encoder::destroy() {
    if (encoder_) {
        encoder_->Uninitialize();
        WelsDestroySVCEncoder(encoder_);
        encoder_ = nullptr;
    }
}

bool OpenH264Encoder::init(const EncoderConfig& cfg) {
    destroy();
    config_ = cfg;

    if (!openh264_load()) {
        LOG_WARN("OpenH264 library not available");
        return false;
    }

    if (WelsCreateSVCEncoder(&encoder_) != 0 || !encoder_) {
        return false;
    }

    SEncParamExt param;
    std::memset(&param, 0, sizeof(param));
    encoder_->GetDefaultParams(&param);

    // Basic parameters
    param.iPicWidth = cfg.width;
    param.iPicHeight = cfg.height;
    param.fMaxFrameRate = cfg.maxFps;
    param.iTargetBitrate = static_cast<int>(cfg.targetBitrateBps);
    param.iMaxBitrate = static_cast<int>(cfg.maxBitrateBps);

    // Screen content optimized usage
    param.iUsageType = SCREEN_CONTENT_REAL_TIME;

    // Rate control: CBR
    param.iRCMode = RC_BITRATE_MODE;

    // Constrained Baseline profile, single slice, no B-frames
    param.iEntropyCodingModeFlag = 0; // CAVLC (Baseline)
    param.iNumRefFrame = 1;
    param.bEnableFrameSkip = false;
    param.iMultipleThreadIdc = 1;
    param.bEnableDenoise = false;
    param.bEnableBackgroundDetection = true;
    param.bEnableAdaptiveQuant = cfg.adaptiveQuantization;
    param.bEnableSceneChangeDetect = true;
    param.bEnableLongTermReference = true;
    param.iLtrMarkPeriod = 30;

    // SVC temporal layers
    param.iTemporalLayerNum = cfg.temporalLayers;

    // Single spatial layer
    param.iSpatialLayerNum = 1;
    param.sSpatialLayers[0].iVideoWidth = cfg.width;
    param.sSpatialLayers[0].iVideoHeight = cfg.height;
    param.sSpatialLayers[0].fFrameRate = cfg.maxFps;
    param.sSpatialLayers[0].iSpatialBitrate = static_cast<int>(cfg.targetBitrateBps);
    param.sSpatialLayers[0].iMaxSpatialBitrate = static_cast<int>(cfg.maxBitrateBps);
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    param.sSpatialLayers[0].uiLevelIdc = LEVEL_3_1;

    // Single slice per frame
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

    if (encoder_->InitializeExt(&param) != cmResultSuccess) {
        destroy();
        return false;
    }

    // Set output to Annex-B format (NAL start codes)
    int videoFormat = videoFormatI420;
    encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &videoFormat);

    frameIndex_ = 0;
    keyFrameRequested_ = true; // Start with a key frame

    return true;
}

bool OpenH264Encoder::encode(const Frame& frame,
                              const std::vector<RegionInfo>& regions,
                              EncodedPacket& out) {
    if (!encoder_) {
        return false;
    }

    // Prepare source picture (expects I420 input)
    SSourcePicture srcPic;
    std::memset(&srcPic, 0, sizeof(srcPic));
    srcPic.iColorFormat = videoFormatI420;
    srcPic.iPicWidth = frame.width;
    srcPic.iPicHeight = frame.height;
    srcPic.uiTimeStamp = frame.timestampUs / 1000; // Convert us to ms

    if (frame.format == PixelFormat::I420) {
        srcPic.pData[0] = const_cast<uint8_t*>(frame.plane(0));
        srcPic.pData[1] = const_cast<uint8_t*>(frame.plane(1));
        srcPic.pData[2] = const_cast<uint8_t*>(frame.plane(2));
        srcPic.iStride[0] = frame.stride;
        srcPic.iStride[1] = frame.stride / 2;
        srcPic.iStride[2] = frame.stride / 2;
    } else {
        // Caller must provide I420 frames; other formats require prior conversion.
        return false;
    }

    // Request key frame if needed
    if (keyFrameRequested_) {
        encoder_->ForceIntraFrame(true);
        keyFrameRequested_ = false;
    }

    // Encode
    SFrameBSInfo bsInfo;
    std::memset(&bsInfo, 0, sizeof(bsInfo));

    int ret = encoder_->EncodeFrame(&srcPic, &bsInfo);
    if (ret != cmResultSuccess) {
        return false;
    }

    if (bsInfo.eFrameType == videoFrameTypeSkip) {
        // Encoder decided to skip this frame.
        out.data.clear();
        out.frameId = frame.frameId;
        out.timestampUs = frame.timestampUs;
        out.isKeyFrame = false;
        return true;
    }

    // Collect NAL units from all layers into a single output buffer.
    out.data.clear();
    for (int layer = 0; layer < bsInfo.iLayerNum; ++layer) {
        const SLayerBSInfo& layerInfo = bsInfo.sLayerInfo[layer];
        const uint8_t* layerData = layerInfo.pBsBuf;
        int layerSize = 0;
        for (int nal = 0; nal < layerInfo.iNalCount; ++nal) {
            layerSize += layerInfo.pNalLengthInByte[nal];
        }
        out.data.insert(out.data.end(), layerData, layerData + layerSize);
    }

    out.frameId = frame.frameId;
    out.timestampUs = frame.timestampUs;
    out.isKeyFrame = (bsInfo.eFrameType == videoFrameTypeIDR);
    out.temporalLayer = static_cast<uint8_t>(bsInfo.sLayerInfo[0].uiTemporalId);

    // Copy dirty rects from region info.
    out.dirtyRects.clear();
    out.dirtyRects.reserve(regions.size());
    for (const auto& ri : regions) {
        out.dirtyRects.push_back(ri.rect);
    }

    ++frameIndex_;
    return true;
}

void OpenH264Encoder::requestKeyFrame() {
    keyFrameRequested_ = true;
}

void OpenH264Encoder::updateBitrate(uint32_t bps) {
    if (!encoder_) {
        return;
    }

    config_.targetBitrateBps = bps;

    SBitrateInfo bitrateInfo;
    std::memset(&bitrateInfo, 0, sizeof(bitrateInfo));
    bitrateInfo.iLayer = SPATIAL_LAYER_ALL;
    bitrateInfo.iBitrate = static_cast<int>(bps);
    encoder_->SetOption(ENCODER_OPTION_BITRATE, &bitrateInfo);

    // Also update max bitrate if target exceeds it.
    if (bps > config_.maxBitrateBps) {
        config_.maxBitrateBps = bps;
        SBitrateInfo maxInfo;
        std::memset(&maxInfo, 0, sizeof(maxInfo));
        maxInfo.iLayer = SPATIAL_LAYER_ALL;
        maxInfo.iBitrate = static_cast<int>(bps);
        encoder_->SetOption(ENCODER_OPTION_MAX_BITRATE, &maxInfo);
    }
}

EncoderInfo OpenH264Encoder::getInfo() {
    EncoderInfo info;
    info.name = "OpenH264";
    info.isHardware = false;
    info.supportsROI = false;
    info.supportsSVC = true;
    info.maxWidth = 4096;
    info.maxHeight = 2160;
    return info;
}

} // namespace omnidesk
