#include "codec/vaapi_encoder.h"
#include "core/logger.h"

#ifdef OMNIDESK_HAS_VAAPI

#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace omnidesk {

// ---- VaapiEncoder ----

VaapiEncoder::VaapiEncoder() = default;

VaapiEncoder::~VaapiEncoder() {
    destroy();
}

bool VaapiEncoder::init(const EncoderConfig& cfg) {
    config_ = cfg;

    if (!openDevice()) {
        LOG_ERROR("VAAPI: failed to open DRM device");
        return false;
    }

    if (!createConfig()) {
        LOG_ERROR("VAAPI: failed to create encode config");
        destroy();
        return false;
    }

    if (!createSurfaces()) {
        LOG_ERROR("VAAPI: failed to create surfaces");
        destroy();
        return false;
    }

    if (!createContext()) {
        LOG_ERROR("VAAPI: failed to create context");
        destroy();
        return false;
    }

    if (!createCodedBuffer()) {
        LOG_ERROR("VAAPI: failed to create coded buffer");
        destroy();
        return false;
    }

    initialized_ = true;
    keyFrameRequested_ = true;
    frameIndex_ = 0;
    LOG_INFO("VAAPI encoder initialized: %dx%d @ %d kbps",
             cfg.width, cfg.height, cfg.targetBitrateBps / 1000);
    return true;
}

bool VaapiEncoder::openDevice() {
    // Try common DRM render nodes
    const char* devices[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/renderD130",
    };

    for (const char* dev : devices) {
        drmFd_ = open(dev, O_RDWR);
        if (drmFd_ >= 0) {
            display_ = vaGetDisplayDRM(drmFd_);
            if (display_) {
                int major, minor;
                VAStatus st = vaInitialize(display_, &major, &minor);
                if (st == VA_STATUS_SUCCESS) {
                    LOG_INFO("VAAPI: opened %s, version %d.%d", dev, major, minor);
                    return true;
                }
            }
            close(drmFd_);
            drmFd_ = -1;
        }
    }
    return false;
}

bool VaapiEncoder::createConfig() {
    // Check for H.264 encode support
    VAEntrypoint entrypoints[16];
    int numEntrypoints = 0;
    VAStatus st = vaQueryConfigEntrypoints(display_, VAProfileH264Main,
                                            entrypoints, &numEntrypoints);
    if (st != VA_STATUS_SUCCESS) return false;

    // Prefer low-power encoding for lower latency
    VAEntrypoint selectedEp = static_cast<VAEntrypoint>(0);
    for (int i = 0; i < numEntrypoints; ++i) {
        if (entrypoints[i] == VAEntrypointEncSliceLP) {
            selectedEp = VAEntrypointEncSliceLP;
            break;
        }
        if (entrypoints[i] == VAEntrypointEncSlice) {
            selectedEp = VAEntrypointEncSlice;
        }
    }
    if (selectedEp == static_cast<VAEntrypoint>(0)) return false;

    // Rate control attribute
    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRateControl;
    vaGetConfigAttributes(display_, VAProfileH264Main, selectedEp, &attrib, 1);

    if (!(attrib.value & VA_RC_CBR)) {
        LOG_WARN("VAAPI: CBR not supported, trying CQP");
    }

    VAConfigAttrib attribs[1];
    attribs[0].type = VAConfigAttribRateControl;
    attribs[0].value = (attrib.value & VA_RC_CBR) ? VA_RC_CBR : VA_RC_CQP;

    st = vaCreateConfig(display_, VAProfileH264Main, selectedEp,
                        attribs, 1, &configId_);
    return st == VA_STATUS_SUCCESS;
}

bool VaapiEncoder::createSurfaces() {
    VASurfaceID surfaces[3];
    VAStatus st = vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420,
                                    config_.width, config_.height,
                                    surfaces, 3, nullptr, 0);
    if (st != VA_STATUS_SUCCESS) return false;

    inputSurface_ = surfaces[0];
    refSurface_ = surfaces[1];
    reconSurface_ = surfaces[2];
    return true;
}

bool VaapiEncoder::createContext() {
    VASurfaceID surfaces[] = {inputSurface_, refSurface_, reconSurface_};
    VAStatus st = vaCreateContext(display_, configId_,
                                  config_.width, config_.height,
                                  VA_PROGRESSIVE, surfaces, 3, &contextId_);
    return st == VA_STATUS_SUCCESS;
}

bool VaapiEncoder::createCodedBuffer() {
    // Allocate a coded buffer large enough for the worst case
    size_t bufSize = config_.width * config_.height * 3 / 2;
    if (bufSize < 1024 * 1024) bufSize = 1024 * 1024;

    VAStatus st = vaCreateBuffer(display_, contextId_, VAEncCodedBufferType,
                                  bufSize, 1, nullptr, &codedBuf_);
    return st == VA_STATUS_SUCCESS;
}

void VaapiEncoder::uploadFrame(const Frame& frame) {
    VAImage image;
    VAStatus st = vaDeriveImage(display_, inputSurface_, &image);
    if (st != VA_STATUS_SUCCESS) return;

    void* mapped = nullptr;
    st = vaMapBuffer(display_, image.buf, &mapped);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyImage(display_, image.image_id);
        return;
    }

    auto* dst = static_cast<uint8_t*>(mapped);

    // Copy I420 planes
    const uint8_t* yPlane = frame.plane(0);
    const uint8_t* uPlane = frame.plane(1);
    const uint8_t* vPlane = frame.plane(2);

    // Y plane
    for (int y = 0; y < config_.height; ++y) {
        std::memcpy(dst + y * image.pitches[0],
                    yPlane + y * frame.stride, config_.width);
    }

    // U plane (NV12 interleaved UV, or I420 separate)
    if (image.format.fourcc == VA_FOURCC_NV12) {
        // Interleave U and V into NV12
        uint8_t* uvDst = dst + image.offsets[1];
        int uvH = config_.height / 2;
        int uvW = config_.width / 2;
        for (int y = 0; y < uvH; ++y) {
            for (int x = 0; x < uvW; ++x) {
                uvDst[y * image.pitches[1] + x * 2 + 0] = uPlane[y * (frame.stride / 2) + x];
                uvDst[y * image.pitches[1] + x * 2 + 1] = vPlane[y * (frame.stride / 2) + x];
            }
        }
    } else {
        // I420 layout
        int uvH = config_.height / 2;
        for (int y = 0; y < uvH; ++y) {
            std::memcpy(dst + image.offsets[1] + y * image.pitches[1],
                        uPlane + y * (frame.stride / 2), config_.width / 2);
            std::memcpy(dst + image.offsets[2] + y * image.pitches[2],
                        vPlane + y * (frame.stride / 2), config_.width / 2);
        }
    }

    vaUnmapBuffer(display_, image.buf);
    vaDestroyImage(display_, image.image_id);
}

bool VaapiEncoder::renderPicture(bool isIDR) {
    VAStatus st;

    // Sequence parameter
    VAEncSequenceParameterBufferH264 seq{};
    seq.seq_parameter_set_id = 0;
    seq.level_idc = 41;  // Level 4.1
    seq.intra_period = idrPeriod_;
    seq.intra_idr_period = idrPeriod_;
    seq.ip_period = 1;  // No B-frames
    seq.bits_per_second = config_.targetBitrateBps;
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = (config_.width + 15) / 16;
    seq.picture_height_in_mbs = (config_.height + 15) / 16;
    seq.frame_mbs_only_flag = 1;
    seq.time_scale = static_cast<uint32_t>(config_.maxFps * 2);
    seq.num_units_in_tick = 1;

    VABufferID seqBuf;
    st = vaCreateBuffer(display_, contextId_, VAEncSequenceParameterBufferType,
                        sizeof(seq), 1, &seq, &seqBuf);
    if (st != VA_STATUS_SUCCESS) return false;

    // Picture parameter
    VAEncPictureParameterBufferH264 pic{};
    pic.CurrPic.picture_id = reconSurface_;
    pic.CurrPic.TopFieldOrderCnt = static_cast<int32_t>(frameIndex_ * 2);
    pic.coded_buf = codedBuf_;
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.pic_fields.bits.idr_pic_flag = isIDR ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = 0;  // CAVLC for Baseline
    pic.frame_num = static_cast<uint16_t>(frameIndex_ % idrPeriod_);
    pic.pic_init_qp = 26;
    pic.num_ref_idx_l0_active_minus1 = 0;

    if (!isIDR) {
        pic.ReferenceFrames[0].picture_id = refSurface_;
        pic.ReferenceFrames[0].TopFieldOrderCnt = static_cast<int32_t>((frameIndex_ - 1) * 2);
        pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    }
    // Mark unused ref slots
    for (int i = (isIDR ? 0 : 1); i < 16; ++i) {
        pic.ReferenceFrames[i].picture_id = VA_INVALID_ID;
    }

    VABufferID picBuf;
    st = vaCreateBuffer(display_, contextId_, VAEncPictureParameterBufferType,
                        sizeof(pic), 1, &pic, &picBuf);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(display_, seqBuf);
        return false;
    }

    // Slice parameter
    VAEncSliceParameterBufferH264 slice{};
    slice.macroblock_address = 0;
    slice.num_macroblocks = ((config_.width + 15) / 16) * ((config_.height + 15) / 16);
    slice.slice_type = isIDR ? 2 : 0;  // I=2, P=0
    slice.pic_parameter_set_id = 0;
    slice.idr_pic_id = isIDR ? static_cast<uint16_t>(frameIndex_) : 0;
    slice.pic_order_cnt_lsb = static_cast<uint16_t>((frameIndex_ * 2) % 256);

    if (!isIDR) {
        slice.RefPicList0[0].picture_id = refSurface_;
        slice.RefPicList0[0].TopFieldOrderCnt = static_cast<int32_t>((frameIndex_ - 1) * 2);
        slice.RefPicList0[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        slice.num_ref_idx_l0_active_minus1 = 0;
    }
    for (int i = (isIDR ? 0 : 1); i < 32; ++i) {
        slice.RefPicList0[i].picture_id = VA_INVALID_ID;
        slice.RefPicList1[i].picture_id = VA_INVALID_ID;
    }

    VABufferID sliceBuf;
    st = vaCreateBuffer(display_, contextId_, VAEncSliceParameterBufferType,
                        sizeof(slice), 1, &slice, &sliceBuf);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(display_, seqBuf);
        vaDestroyBuffer(display_, picBuf);
        return false;
    }

    // Render
    VABufferID buffers[] = {seqBuf, picBuf, sliceBuf};
    st = vaBeginPicture(display_, contextId_, inputSurface_);
    if (st != VA_STATUS_SUCCESS) return false;

    st = vaRenderPicture(display_, contextId_, buffers, 3);
    if (st != VA_STATUS_SUCCESS) {
        vaEndPicture(display_, contextId_);
        return false;
    }

    st = vaEndPicture(display_, contextId_);
    if (st != VA_STATUS_SUCCESS) return false;

    st = vaSyncSurface(display_, inputSurface_);
    return st == VA_STATUS_SUCCESS;
}

bool VaapiEncoder::extractNALs(EncodedPacket& out) {
    VACodedBufferSegment* segment = nullptr;
    VAStatus st = vaMapBuffer(display_, codedBuf_, reinterpret_cast<void**>(&segment));
    if (st != VA_STATUS_SUCCESS) return false;

    out.data.clear();
    while (segment) {
        auto* buf = static_cast<const uint8_t*>(segment->buf);
        out.data.insert(out.data.end(), buf, buf + segment->size);
        segment = reinterpret_cast<VACodedBufferSegment*>(segment->next);
    }

    vaUnmapBuffer(display_, codedBuf_);
    return !out.data.empty();
}

bool VaapiEncoder::encode(const Frame& frame,
                           const std::vector<RegionInfo>& /*regions*/,
                           EncodedPacket& out) {
    if (!initialized_) return false;

    uploadFrame(frame);

    bool isIDR = keyFrameRequested_ || (frameIndex_ % idrPeriod_ == 0);
    keyFrameRequested_ = false;

    if (!renderPicture(isIDR)) {
        LOG_ERROR("VAAPI: renderPicture failed");
        return false;
    }

    if (!extractNALs(out)) {
        LOG_ERROR("VAAPI: extractNALs failed");
        return false;
    }

    out.frameId = frameIndex_;
    out.isKeyFrame = isIDR;
    out.temporalLayer = 0;
    out.timestampUs = frame.timestampUs;

    // Swap reference surfaces
    std::swap(refSurface_, reconSurface_);
    frameIndex_++;

    return true;
}

void VaapiEncoder::requestKeyFrame() {
    keyFrameRequested_ = true;
}

void VaapiEncoder::updateBitrate(uint32_t bps) {
    config_.targetBitrateBps = bps;
    // Bitrate change takes effect on next encode via sequence parameters
}

EncoderInfo VaapiEncoder::getInfo() {
    EncoderInfo info;
    info.name = "VA-API H.264";
    info.isHardware = true;
    info.maxWidth = config_.width;
    info.maxHeight = config_.height;
    return info;
}

void VaapiEncoder::destroy() {
    if (!display_) return;

    if (codedBuf_ != VA_INVALID_ID) vaDestroyBuffer(display_, codedBuf_);
    if (contextId_ != VA_INVALID_ID) vaDestroyContext(display_, contextId_);
    if (inputSurface_ != VA_INVALID_ID) {
        VASurfaceID surfaces[] = {inputSurface_, refSurface_, reconSurface_};
        vaDestroySurfaces(display_, surfaces, 3);
    }
    if (configId_ != VA_INVALID_ID) vaDestroyConfig(display_, configId_);

    vaTerminate(display_);
    display_ = nullptr;

    if (drmFd_ >= 0) {
        close(drmFd_);
        drmFd_ = -1;
    }

    initialized_ = false;
}

// ---- VaapiDecoder ----

VaapiDecoder::VaapiDecoder() = default;
VaapiDecoder::~VaapiDecoder() { destroy(); }

bool VaapiDecoder::init(int width, int height) {
    width_ = width;
    height_ = height;

    // Open DRM device
    const char* devices[] = {"/dev/dri/renderD128", "/dev/dri/renderD129"};
    for (const char* dev : devices) {
        drmFd_ = open(dev, O_RDWR);
        if (drmFd_ >= 0) {
            display_ = vaGetDisplayDRM(drmFd_);
            if (display_) {
                int major, minor;
                if (vaInitialize(display_, &major, &minor) == VA_STATUS_SUCCESS) {
                    break;
                }
            }
            close(drmFd_);
            drmFd_ = -1;
            display_ = nullptr;
        }
    }
    if (!display_) return false;

    // Create decode config
    VAStatus st = vaCreateConfig(display_, VAProfileH264Main,
                                  VAEntrypointVLD, nullptr, 0, &configId_);
    if (st != VA_STATUS_SUCCESS) { destroy(); return false; }

    // Create surface
    st = vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420,
                          width, height, &surface_, 1, nullptr, 0);
    if (st != VA_STATUS_SUCCESS) { destroy(); return false; }

    // Create context
    st = vaCreateContext(display_, configId_, width, height,
                         VA_PROGRESSIVE, &surface_, 1, &contextId_);
    if (st != VA_STATUS_SUCCESS) { destroy(); return false; }

    initialized_ = true;
    LOG_INFO("VAAPI decoder initialized: %dx%d", width, height);
    return true;
}

bool VaapiDecoder::decode(const uint8_t* data, size_t size, Frame& out) {
    if (!initialized_) return false;

    // Create a slice data buffer
    VABufferID sliceDataBuf;
    VAStatus st = vaCreateBuffer(display_, contextId_, VASliceDataBufferType,
                                  size, 1, const_cast<uint8_t*>(data), &sliceDataBuf);
    if (st != VA_STATUS_SUCCESS) return false;

    // Begin picture
    st = vaBeginPicture(display_, contextId_, surface_);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(display_, sliceDataBuf);
        return false;
    }

    st = vaRenderPicture(display_, contextId_, &sliceDataBuf, 1);
    vaEndPicture(display_, contextId_);
    vaSyncSurface(display_, surface_);

    // Read back decoded frame
    VAImage image;
    st = vaDeriveImage(display_, surface_, &image);
    if (st != VA_STATUS_SUCCESS) return false;

    void* mapped = nullptr;
    st = vaMapBuffer(display_, image.buf, &mapped);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyImage(display_, image.image_id);
        return false;
    }

    out.allocate(width_, height_, PixelFormat::I420);
    auto* src = static_cast<const uint8_t*>(mapped);

    // Copy Y
    for (int y = 0; y < height_; ++y) {
        std::memcpy(out.plane(0) + y * out.stride,
                    src + image.offsets[0] + y * image.pitches[0], width_);
    }

    // Copy U/V (handle NV12 or I420)
    if (image.format.fourcc == VA_FOURCC_NV12) {
        const uint8_t* uvSrc = src + image.offsets[1];
        int uvH = height_ / 2;
        int uvW = width_ / 2;
        for (int y = 0; y < uvH; ++y) {
            for (int x = 0; x < uvW; ++x) {
                out.plane(1)[y * (out.stride / 2) + x] = uvSrc[y * image.pitches[1] + x * 2];
                out.plane(2)[y * (out.stride / 2) + x] = uvSrc[y * image.pitches[1] + x * 2 + 1];
            }
        }
    } else {
        int uvH = height_ / 2;
        for (int y = 0; y < uvH; ++y) {
            std::memcpy(out.plane(1) + y * (out.stride / 2),
                        src + image.offsets[1] + y * image.pitches[1], width_ / 2);
            std::memcpy(out.plane(2) + y * (out.stride / 2),
                        src + image.offsets[2] + y * image.pitches[2], width_ / 2);
        }
    }

    vaUnmapBuffer(display_, image.buf);
    vaDestroyImage(display_, image.image_id);
    return true;
}

void VaapiDecoder::reset() {
    // No special reset needed for VAAPI
}

void VaapiDecoder::destroy() {
    if (!display_) return;
    if (contextId_ != VA_INVALID_ID) vaDestroyContext(display_, contextId_);
    if (surface_ != VA_INVALID_ID) vaDestroySurfaces(display_, &surface_, 1);
    if (configId_ != VA_INVALID_ID) vaDestroyConfig(display_, configId_);
    vaTerminate(display_);
    display_ = nullptr;
    if (drmFd_ >= 0) { close(drmFd_); drmFd_ = -1; }
    initialized_ = false;
}

} // namespace omnidesk

#endif // OMNIDESK_HAS_VAAPI
