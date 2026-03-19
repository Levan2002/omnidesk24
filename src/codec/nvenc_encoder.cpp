#include "codec/nvenc_encoder.h"
#include "core/logger.h"

#ifdef OMNIDESK_HAS_NVENC

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstring>
#include <algorithm>

namespace omnidesk {

// ============================================================================
// Minimal NVENC API type definitions — mirrors the official NVENC SDK headers
// so we can load the library at runtime without requiring the SDK at compile
// time.  Only the subset we actually use is defined here.
// ============================================================================

// GUID helper
struct NV_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
};

inline bool operator==(const NV_GUID& a, const NV_GUID& b) {
    return std::memcmp(&a, &b, sizeof(NV_GUID)) == 0;
}

// Well-known GUIDs
static constexpr NV_GUID NV_ENC_CODEC_H264_GUID =
    {0x6bc82762, 0x4e63, 0x4ca4, {0xaa, 0x85, 0x1e, 0xa8, 0x9e, 0x0b, 0x50, 0x8e}};
static constexpr NV_GUID NV_ENC_PRESET_P1_GUID =
    {0xfc0a8d3e, 0x45f8, 0x4cf8, {0x80, 0xc7, 0x29, 0x88, 0x71, 0x59, 0x0e, 0xbf}};
static constexpr NV_GUID NV_ENC_H264_PROFILE_BASELINE_GUID =
    {0x0727bcaa, 0x78c4, 0x4c83, {0x8c, 0x2f, 0xef, 0x3d, 0xff, 0x26, 0x7c, 0x6a}};

// NVENC status codes
using NVENCSTATUS = int32_t;
static constexpr NVENCSTATUS NV_ENC_SUCCESS = 0;

// NVENC buffer formats
enum NV_ENC_BUFFER_FORMAT : uint32_t {
    NV_ENC_BUFFER_FORMAT_NV12 = 1,
    NV_ENC_BUFFER_FORMAT_ARGB = 0x22,
};

// NVENC picture types
enum NV_ENC_PIC_TYPE : uint32_t {
    NV_ENC_PIC_TYPE_P   = 0,
    NV_ENC_PIC_TYPE_IDR = 4,
};

// NVENC tuning info
enum NV_ENC_TUNING_INFO : uint32_t {
    NV_ENC_TUNING_INFO_LOW_LATENCY  = 2,
};

// NVENC rate control modes
enum NV_ENC_PARAMS_RC_MODE : uint32_t {
    NV_ENC_PARAMS_RC_CBR          = 2,
    NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ = 8,
};

// NVENC picture struct
enum NV_ENC_PIC_STRUCT : uint32_t {
    NV_ENC_PIC_STRUCT_FRAME = 1,
};

// NVENC open session params
struct NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    uint32_t version;
    uint32_t apiVersion;
    void*    device;
    uint32_t deviceType; // 0 = CUDA
    uint32_t reserved;
    void*    reserved1[7];
};

// Rate control params
struct NV_ENC_RC_PARAMS {
    uint32_t version;
    NV_ENC_PARAMS_RC_MODE rateControlMode;
    uint32_t constQP_interP;
    uint32_t constQP_interB;
    uint32_t constQP_intra;
    uint32_t averageBitRate;
    uint32_t maxBitRate;
    uint32_t vbvBufferSize;
    uint32_t vbvInitialDelay;
    uint32_t reserved[8];
};

// H.264 specific config
struct NV_ENC_CONFIG_H264 {
    uint32_t idrPeriod;
    uint32_t enableIntraRefresh;
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t repeatSPSPPS;
    uint32_t disableDeblockingFilterIDC;
    uint32_t reserved[16];
};

// Codec-specific config union
struct NV_ENC_CODEC_CONFIG {
    NV_ENC_CONFIG_H264 h264Config;
    uint8_t reserved[256 - sizeof(NV_ENC_CONFIG_H264)];
};

// Encoder config
struct NV_ENC_CONFIG {
    uint32_t version;
    NV_GUID  profileGUID;
    uint32_t gopLength;
    int32_t  frameIntervalP;
    uint32_t reserved1[4];
    NV_ENC_RC_PARAMS rcParams;
    NV_ENC_CODEC_CONFIG encodeCodecConfig;
    uint8_t  reserved[512];
};

// Preset config
struct NV_ENC_PRESET_CONFIG {
    uint32_t version;
    NV_ENC_CONFIG presetCfg;
    uint8_t  reserved[256];
};

// Initialize params
struct NV_ENC_INITIALIZE_PARAMS {
    uint32_t version;
    NV_GUID  encodeGUID;
    NV_GUID  presetGUID;
    uint32_t encodeWidth;
    uint32_t encodeHeight;
    uint32_t darWidth;
    uint32_t darHeight;
    uint32_t frameRateNum;
    uint32_t frameRateDen;
    uint32_t enableEncodeAsync;
    uint32_t enablePTD; // picture type decision
    uint32_t reserved1[4];
    NV_ENC_CONFIG* encodeConfig;
    uint32_t maxEncodeWidth;
    uint32_t maxEncodeHeight;
    NV_ENC_TUNING_INFO tuningInfo;
    uint8_t  reserved[256];
};

// Create input buffer params
struct NV_ENC_CREATE_INPUT_BUFFER {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t reserved1;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    void*    inputBuffer;
    uint8_t  reserved[64];
};

// Create bitstream buffer params
struct NV_ENC_CREATE_BITSTREAM_BUFFER {
    uint32_t version;
    void*    bitstreamBuffer;
    uint32_t size;
    uint8_t  reserved[64];
};

// Lock input buffer params
struct NV_ENC_LOCK_INPUT_BUFFER {
    uint32_t version;
    void*    inputBuffer;
    void*    bufferDataPtr;
    uint32_t pitch;
    uint8_t  reserved[64];
};

// Lock bitstream params
struct NV_ENC_LOCK_BITSTREAM {
    uint32_t version;
    void*    outputBitstream;
    void*    bitstreamBufferPtr;
    uint32_t bitstreamSizeInBytes;
    NV_ENC_PIC_TYPE pictureType;
    uint64_t outputTimeStamp;
    uint64_t outputDuration;
    uint8_t  reserved[64];
};

// Encode picture params
struct NV_ENC_PIC_PARAMS {
    uint32_t version;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t inputPitch;
    uint32_t encodePicFlags; // 0x4 = force IDR
    void*    inputBuffer;
    void*    outputBitstream;
    NV_ENC_PIC_STRUCT pictureStruct;
    uint64_t inputTimeStamp;
    uint64_t inputDuration;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    NV_ENC_CODEC_CONFIG* codecPicParams;
    uint8_t  reserved[128];
};

// Reconfigure params
struct NV_ENC_RECONFIGURE_PARAMS {
    uint32_t version;
    NV_ENC_INITIALIZE_PARAMS reInitEncodeParams;
    uint32_t resetEncoder;
    uint32_t forceIDR;
    uint8_t  reserved[64];
};

// NVENC API function table
struct NV_ENCODE_API_FUNCTION_LIST {
    uint32_t version;
    uint32_t reserved;
    NVENCSTATUS (*nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*, void**);
    NVENCSTATUS (*nvEncGetEncodePresetConfigEx)(void*, NV_GUID, NV_GUID, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG*);
    NVENCSTATUS (*nvEncInitializeEncoder)(void*, NV_ENC_INITIALIZE_PARAMS*);
    NVENCSTATUS (*nvEncCreateInputBuffer)(void*, NV_ENC_CREATE_INPUT_BUFFER*);
    NVENCSTATUS (*nvEncCreateBitstreamBuffer)(void*, NV_ENC_CREATE_BITSTREAM_BUFFER*);
    NVENCSTATUS (*nvEncLockInputBuffer)(void*, NV_ENC_LOCK_INPUT_BUFFER*);
    NVENCSTATUS (*nvEncUnlockInputBuffer)(void*, void*);
    NVENCSTATUS (*nvEncEncodePicture)(void*, NV_ENC_PIC_PARAMS*);
    NVENCSTATUS (*nvEncLockBitstream)(void*, NV_ENC_LOCK_BITSTREAM*);
    NVENCSTATUS (*nvEncUnlockBitstream)(void*, void*);
    NVENCSTATUS (*nvEncDestroyInputBuffer)(void*, void*);
    NVENCSTATUS (*nvEncDestroyBitstreamBuffer)(void*, void*);
    NVENCSTATUS (*nvEncDestroyEncoder)(void*);
    NVENCSTATUS (*nvEncReconfigureEncoder)(void*, NV_ENC_RECONFIGURE_PARAMS*);
    void* reserved2[64]; // padding for future API entries
};

using NvEncCreateInstance_t = NVENCSTATUS (*)(NV_ENCODE_API_FUNCTION_LIST*);

// CUDA minimal types
using CUdevice  = int;
using CUcontext = void*;
using CUresult  = int;

struct NvencFunctions {
    NV_ENCODE_API_FUNCTION_LIST api{};

    // CUDA function pointers
    CUresult (*cuInit)(unsigned int) = nullptr;
    CUresult (*cuDeviceGet)(CUdevice*, int) = nullptr;
    CUresult (*cuCtxCreate)(CUcontext*, unsigned int, CUdevice) = nullptr;
    CUresult (*cuCtxDestroy)(CUcontext) = nullptr;
};

// ============================================================================
// Platform helpers for loading shared libraries
// ============================================================================

static void* loadLib(const char* name) {
#ifdef _WIN32
    return LoadLibraryA(name);
#else
    return dlopen(name, RTLD_LAZY);
#endif
}

static void* loadSym(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

static void freeLib(void* lib) {
    if (!lib) return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

// ============================================================================
// NvencEncoder implementation
// ============================================================================

NvencEncoder::NvencEncoder() = default;

NvencEncoder::~NvencEncoder() {
    destroy();
}

bool NvencEncoder::loadLibrary() {
    // Load NVENC library
#ifdef _WIN32
    nvencLib_ = loadLib("nvEncodeAPI64.dll");
    if (!nvencLib_) nvencLib_ = loadLib("nvEncodeAPI.dll");
#else
    nvencLib_ = loadLib("libnvidia-encode.so.1");
    if (!nvencLib_) nvencLib_ = loadLib("libnvidia-encode.so");
#endif
    if (!nvencLib_) {
        LOG_INFO("NVENC: library not found");
        return false;
    }

    // Load CUDA library
#ifdef _WIN32
    cudaLib_ = loadLib("nvcuda.dll");
#else
    cudaLib_ = loadLib("libcuda.so.1");
    if (!cudaLib_) cudaLib_ = loadLib("libcuda.so");
#endif
    if (!cudaLib_) {
        LOG_INFO("NVENC: CUDA library not found");
        freeLib(nvencLib_);
        nvencLib_ = nullptr;
        return false;
    }

    fn_ = std::make_unique<NvencFunctions>();

    // Resolve CUDA functions
    fn_->cuInit       = reinterpret_cast<decltype(fn_->cuInit)>(loadSym(cudaLib_, "cuInit"));
    fn_->cuDeviceGet  = reinterpret_cast<decltype(fn_->cuDeviceGet)>(loadSym(cudaLib_, "cuDeviceGet"));
    fn_->cuCtxCreate  = reinterpret_cast<decltype(fn_->cuCtxCreate)>(loadSym(cudaLib_, "cuCtxCreate_v2"));
    fn_->cuCtxDestroy = reinterpret_cast<decltype(fn_->cuCtxDestroy)>(loadSym(cudaLib_, "cuCtxDestroy_v2"));

    if (!fn_->cuInit || !fn_->cuDeviceGet || !fn_->cuCtxCreate || !fn_->cuCtxDestroy) {
        LOG_ERROR("NVENC: failed to resolve CUDA functions");
        return false;
    }

    // Resolve NVENC create instance
    auto createInstance = reinterpret_cast<NvEncCreateInstance_t>(
        loadSym(nvencLib_, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        LOG_ERROR("NVENC: NvEncodeAPICreateInstance not found");
        return false;
    }

    // API version 12.0 (NVENC SDK 12.x)
    fn_->api.version = (12 << 4) | 0;
    NVENCSTATUS st = createInstance(&fn_->api);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: NvEncodeAPICreateInstance failed: %d", st);
        return false;
    }

    LOG_INFO("NVENC: library loaded, API function table initialized");
    return true;
}

bool NvencEncoder::initCuda() {
    CUresult rc = fn_->cuInit(0);
    if (rc != 0) {
        LOG_ERROR("NVENC: cuInit failed: %d", rc);
        return false;
    }

    CUdevice device;
    rc = fn_->cuDeviceGet(&device, 0);
    if (rc != 0) {
        LOG_ERROR("NVENC: cuDeviceGet failed: %d", rc);
        return false;
    }

    CUcontext ctx = nullptr;
    rc = fn_->cuCtxCreate(&ctx, 0, device);
    if (rc != 0) {
        LOG_ERROR("NVENC: cuCtxCreate failed: %d", rc);
        return false;
    }

    cuContext_ = ctx;
    LOG_INFO("NVENC: CUDA context created on device 0");
    return true;
}

bool NvencEncoder::openSession() {
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = (12 << 4) | 0;
    params.apiVersion = (12 << 4) | 0;
    params.device = cuContext_;
    params.deviceType = 0; // CUDA

    NVENCSTATUS st = fn_->api.nvEncOpenEncodeSessionEx(&params, &encoder_);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncOpenEncodeSessionEx failed: %d", st);
        return false;
    }

    LOG_INFO("NVENC: encode session opened");
    return true;
}

bool NvencEncoder::configureEncoder() {
    // Get preset config as starting point
    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = (12 << 4) | 0;
    presetCfg.presetCfg.version = (12 << 4) | 0;

    NVENCSTATUS st = fn_->api.nvEncGetEncodePresetConfigEx(
        encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY, &presetCfg);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncGetEncodePresetConfigEx failed: %d", st);
        return false;
    }

    NV_ENC_CONFIG encCfg = presetCfg.presetCfg;
    encCfg.version = (12 << 4) | 0;
    encCfg.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
    encCfg.gopLength = 300;
    encCfg.frameIntervalP = 1; // No B-frames

    // Rate control: CBR low-delay
    encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
    encCfg.rcParams.averageBitRate = config_.targetBitrateBps;
    encCfg.rcParams.maxBitRate = config_.maxBitrateBps;
    encCfg.rcParams.vbvBufferSize = config_.targetBitrateBps /
        static_cast<uint32_t>(std::max(config_.maxFps, 1.0f));
    encCfg.rcParams.vbvInitialDelay = encCfg.rcParams.vbvBufferSize;

    // H.264 specific
    encCfg.encodeCodecConfig.h264Config.idrPeriod = 300;
    encCfg.encodeCodecConfig.h264Config.enableIntraRefresh = 1;
    encCfg.encodeCodecConfig.h264Config.intraRefreshPeriod = 60;
    encCfg.encodeCodecConfig.h264Config.intraRefreshCnt = 5;
    encCfg.encodeCodecConfig.h264Config.sliceMode = 0;    // single slice
    encCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    encCfg.encodeCodecConfig.h264Config.disableDeblockingFilterIDC = 0;

    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = (12 << 4) | 0;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams.encodeWidth = config_.width;
    initParams.encodeHeight = config_.height;
    initParams.darWidth = config_.width;
    initParams.darHeight = config_.height;
    initParams.frameRateNum = static_cast<uint32_t>(config_.maxFps);
    initParams.frameRateDen = 1;
    initParams.enableEncodeAsync = 0;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &encCfg;
    initParams.maxEncodeWidth = config_.width;
    initParams.maxEncodeHeight = config_.height;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    st = fn_->api.nvEncInitializeEncoder(encoder_, &initParams);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncInitializeEncoder failed: %d", st);
        return false;
    }

    LOG_INFO("NVENC: encoder initialized %dx%d @ %u kbps, P1 low-latency",
             config_.width, config_.height, config_.targetBitrateBps / 1000);
    return true;
}

bool NvencEncoder::createBuffers() {
    // Create input buffer (NV12)
    NV_ENC_CREATE_INPUT_BUFFER inBufParams{};
    inBufParams.version = (12 << 4) | 0;
    inBufParams.width = config_.width;
    inBufParams.height = config_.height;
    inBufParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS st = fn_->api.nvEncCreateInputBuffer(encoder_, &inBufParams);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncCreateInputBuffer failed: %d", st);
        return false;
    }
    registeredInput_ = inBufParams.inputBuffer;

    // Create output bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER outBufParams{};
    outBufParams.version = (12 << 4) | 0;

    st = fn_->api.nvEncCreateBitstreamBuffer(encoder_, &outBufParams);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncCreateBitstreamBuffer failed: %d", st);
        return false;
    }
    bitstreamBuffer_ = outBufParams.bitstreamBuffer;

    // Pre-allocate NV12 staging buffer for I420→NV12 conversion
    nv12Pitch_ = config_.width;
    nv12Buffer_.resize(static_cast<size_t>(nv12Pitch_) * config_.height * 3 / 2);

    LOG_INFO("NVENC: input/output buffers created");
    return true;
}

void NvencEncoder::uploadFrame(const Frame& frame) {
    // Convert I420 to NV12 in our staging buffer
    const uint8_t* yPlane = frame.plane(0);
    const uint8_t* uPlane = frame.plane(1);
    const uint8_t* vPlane = frame.plane(2);
    int w = config_.width;
    int h = config_.height;

    uint8_t* nv12Y = nv12Buffer_.data();
    uint8_t* nv12UV = nv12Y + static_cast<size_t>(nv12Pitch_) * h;

    // Copy Y plane
    for (int y = 0; y < h; ++y) {
        std::memcpy(nv12Y + y * nv12Pitch_, yPlane + y * frame.stride, w);
    }

    // Interleave U and V into NV12 UV plane
    int uvH = h / 2;
    int uvW = w / 2;
    int uvStride = frame.stride / 2;
    for (int y = 0; y < uvH; ++y) {
        for (int x = 0; x < uvW; ++x) {
            nv12UV[y * nv12Pitch_ + x * 2 + 0] = uPlane[y * uvStride + x];
            nv12UV[y * nv12Pitch_ + x * 2 + 1] = vPlane[y * uvStride + x];
        }
    }

    // Lock NVENC input buffer and copy NV12 data into it
    NV_ENC_LOCK_INPUT_BUFFER lockParams{};
    lockParams.version = (12 << 4) | 0;
    lockParams.inputBuffer = registeredInput_;

    NVENCSTATUS st = fn_->api.nvEncLockInputBuffer(encoder_, &lockParams);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncLockInputBuffer failed: %d", st);
        return;
    }

    auto* dst = static_cast<uint8_t*>(lockParams.bufferDataPtr);
    uint32_t dstPitch = lockParams.pitch;

    // Copy Y
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * dstPitch, nv12Y + y * nv12Pitch_, w);
    }
    // Copy UV
    for (int y = 0; y < uvH; ++y) {
        std::memcpy(dst + h * dstPitch + y * dstPitch,
                    nv12UV + y * nv12Pitch_, w);
    }

    fn_->api.nvEncUnlockInputBuffer(encoder_, registeredInput_);
}

bool NvencEncoder::init(const EncoderConfig& cfg) {
    destroy();
    config_ = cfg;

    if (!loadLibrary()) return false;
    if (!initCuda())    return false;
    if (!openSession()) return false;
    if (!configureEncoder()) return false;
    if (!createBuffers()) return false;

    initialized_ = true;
    keyFrameRequested_ = true;
    frameIndex_ = 0;

    LOG_INFO("NVENC encoder ready: %dx%d", cfg.width, cfg.height);
    return true;
}

bool NvencEncoder::encode(const Frame& frame,
                           const std::vector<RegionInfo>& /*regions*/,
                           EncodedPacket& out) {
    if (!initialized_) return false;

    // Upload I420 frame as NV12 to the NVENC input buffer
    uploadFrame(frame);

    // Set up encode params
    NV_ENC_PIC_PARAMS picParams{};
    picParams.version = (12 << 4) | 0;
    picParams.inputWidth = config_.width;
    picParams.inputHeight = config_.height;
    picParams.inputPitch = nv12Pitch_;
    picParams.inputBuffer = registeredInput_;
    picParams.outputBitstream = bitstreamBuffer_;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputTimeStamp = frame.timestampUs;
    picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    bool forceIDR = keyFrameRequested_.exchange(false);
    if (forceIDR) {
        picParams.encodePicFlags = 0x4; // NV_ENC_PIC_FLAG_FORCEIDR
    }

    NVENCSTATUS st = fn_->api.nvEncEncodePicture(encoder_, &picParams);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncEncodePicture failed: %d", st);
        return false;
    }

    // Lock and extract the bitstream
    NV_ENC_LOCK_BITSTREAM lockBs{};
    lockBs.version = (12 << 4) | 0;
    lockBs.outputBitstream = bitstreamBuffer_;

    st = fn_->api.nvEncLockBitstream(encoder_, &lockBs);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NVENC: nvEncLockBitstream failed: %d", st);
        return false;
    }

    out.data.assign(
        static_cast<const uint8_t*>(lockBs.bitstreamBufferPtr),
        static_cast<const uint8_t*>(lockBs.bitstreamBufferPtr) + lockBs.bitstreamSizeInBytes
    );

    out.frameId = frameIndex_;
    out.isKeyFrame = (lockBs.pictureType == NV_ENC_PIC_TYPE_IDR) || forceIDR;
    out.temporalLayer = 0;
    out.timestampUs = frame.timestampUs;

    fn_->api.nvEncUnlockBitstream(encoder_, bitstreamBuffer_);
    frameIndex_++;

    return true;
}

void NvencEncoder::requestKeyFrame() {
    keyFrameRequested_ = true;
}

void NvencEncoder::updateBitrate(uint32_t bps) {
    if (!initialized_ || !encoder_) {
        config_.targetBitrateBps = bps;
        return;
    }

    config_.targetBitrateBps = bps;

    // Get current preset config and update bitrate via reconfigure
    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = (12 << 4) | 0;
    presetCfg.presetCfg.version = (12 << 4) | 0;
    fn_->api.nvEncGetEncodePresetConfigEx(
        encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY, &presetCfg);

    NV_ENC_CONFIG encCfg = presetCfg.presetCfg;
    encCfg.rcParams.averageBitRate = bps;
    encCfg.rcParams.maxBitRate = bps * 3 / 2;
    encCfg.rcParams.vbvBufferSize = bps / static_cast<uint32_t>(std::max(config_.maxFps, 1.0f));

    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = (12 << 4) | 0;
    initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
    initParams.encodeWidth = config_.width;
    initParams.encodeHeight = config_.height;
    initParams.darWidth = config_.width;
    initParams.darHeight = config_.height;
    initParams.frameRateNum = static_cast<uint32_t>(config_.maxFps);
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &encCfg;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    NV_ENC_RECONFIGURE_PARAMS reconfig{};
    reconfig.version = (12 << 4) | 0;
    reconfig.reInitEncodeParams = initParams;
    reconfig.resetEncoder = 0;
    reconfig.forceIDR = 0;

    NVENCSTATUS st = fn_->api.nvEncReconfigureEncoder(encoder_, &reconfig);
    if (st != NV_ENC_SUCCESS) {
        LOG_WARN("NVENC: reconfigure bitrate to %u failed: %d", bps, st);
    }
}

EncoderInfo NvencEncoder::getInfo() {
    EncoderInfo info;
    info.name = "NVENC H.264";
    info.isHardware = true;
    info.maxWidth = config_.width;
    info.maxHeight = config_.height;
    return info;
}

void NvencEncoder::destroy() {
    if (encoder_ && fn_) {
        if (registeredInput_) {
            fn_->api.nvEncDestroyInputBuffer(encoder_, registeredInput_);
            registeredInput_ = nullptr;
        }
        if (bitstreamBuffer_) {
            fn_->api.nvEncDestroyBitstreamBuffer(encoder_, bitstreamBuffer_);
            bitstreamBuffer_ = nullptr;
        }
        fn_->api.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }

    if (cuContext_ && fn_ && fn_->cuCtxDestroy) {
        fn_->cuCtxDestroy(static_cast<CUcontext>(cuContext_));
        cuContext_ = nullptr;
    }

    fn_.reset();

    freeLib(nvencLib_);
    nvencLib_ = nullptr;
    freeLib(cudaLib_);
    cudaLib_ = nullptr;

    nv12Buffer_.clear();
    initialized_ = false;
}

// ---- NvdecDecoder ----

NvdecDecoder::NvdecDecoder() = default;
NvdecDecoder::~NvdecDecoder() { destroy(); }

bool NvdecDecoder::init(int width, int height) {
    width_ = width;
    height_ = height;

    // NVDEC decoder requires the CUVID SDK — not yet implemented.
    // Falls back to software (OpenH264) decoder.
    LOG_INFO("NVDEC: decoder not yet implemented, falling back to software");
    return false;
}

bool NvdecDecoder::decode(const uint8_t* /*data*/, size_t /*size*/, Frame& /*out*/) {
    return false;
}

void NvdecDecoder::reset() {}

void NvdecDecoder::destroy() {
    freeLib(libHandle_);
    libHandle_ = nullptr;
    initialized_ = false;
}

} // namespace omnidesk

#endif // OMNIDESK_HAS_NVENC
