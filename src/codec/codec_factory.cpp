#include "codec/codec_factory.h"
#include "codec/openh264_encoder.h"
#include "codec/openh264_decoder.h"
#include "core/logger.h"

#ifdef OMNIDESK_HAS_NVENC
#include "codec/nvenc_encoder.h"
#endif
#ifdef OMNIDESK_HAS_VAAPI
#include "codec/vaapi_encoder.h"
#endif
#ifdef OMNIDESK_PLATFORM_WINDOWS
#include "codec/mf_encoder.h"
#endif

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace omnidesk {

namespace {

// Runtime detection helpers for hardware encoder availability.

bool probeNVENC() {
#ifdef _WIN32
    HMODULE mod = LoadLibraryA("nvEncodeAPI64.dll");
    if (mod) {
        FreeLibrary(mod);
        return true;
    }
    return false;
#elif defined(__linux__)
    void* handle = dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
    }
    if (handle) {
        dlclose(handle);
        return true;
    }
    return false;
#else
    return false;
#endif
}

bool probeVAAPI() {
#ifdef __linux__
    void* handle = dlopen("libva.so.2", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) {
        handle = dlopen("libva.so.2", RTLD_LAZY);
    }
    if (handle) {
        dlclose(handle);
        return true;
    }
    return false;
#else
    return false;
#endif
}

bool probeMediaFoundation() {
#ifdef _WIN32
    HMODULE mod = LoadLibraryA("mfplat.dll");
    if (mod) {
        FreeLibrary(mod);
        return true;
    }
#endif
    return false;
}

} // anonymous namespace

bool CodecFactory::isBackendAvailable(CodecBackend backend) {
    switch (backend) {
        case CodecBackend::NVENC:    return probeNVENC();
        case CodecBackend::VAAPI:    return probeVAAPI();
        case CodecBackend::MF:       return probeMediaFoundation();
        case CodecBackend::OpenH264: return true; // Always available (software)
    }
    return false;
}

std::vector<CodecBackend> CodecFactory::availableBackends() {
    std::vector<CodecBackend> result;
    const CodecBackend all[] = {
        CodecBackend::NVENC,
        CodecBackend::VAAPI,
        CodecBackend::MF,
        CodecBackend::OpenH264,
    };
    for (auto b : all) {
        if (isBackendAvailable(b)) {
            result.push_back(b);
        }
    }
    return result;
}

const char* CodecFactory::backendName(CodecBackend backend) {
    switch (backend) {
        case CodecBackend::NVENC:    return "NVENC";
        case CodecBackend::VAAPI:    return "VA-API";
        case CodecBackend::MF:       return "Media Foundation";
        case CodecBackend::OpenH264: return "OpenH264";
    }
    return "Unknown";
}

std::unique_ptr<IEncoder> CodecFactory::createEncoder() {
    // Try backends in priority order.
    const CodecBackend priority[] = {
        CodecBackend::NVENC,
        CodecBackend::VAAPI,
        CodecBackend::MF,
        CodecBackend::OpenH264,
    };

    for (auto backend : priority) {
        if (isBackendAvailable(backend)) {
            auto encoder = createEncoder(backend);
            if (encoder) {
                return encoder;
            }
        }
    }

    // Should never reach here since OpenH264 is always available.
    return nullptr;
}

std::unique_ptr<IDecoder> CodecFactory::createDecoder() {
    const CodecBackend priority[] = {
        CodecBackend::NVENC,
        CodecBackend::VAAPI,
        CodecBackend::MF,
        CodecBackend::OpenH264,
    };

    for (auto backend : priority) {
        if (isBackendAvailable(backend)) {
            auto decoder = createDecoder(backend);
            if (decoder) {
                return decoder;
            }
        }
    }

    return nullptr;
}

std::unique_ptr<IEncoder> CodecFactory::createEncoder(CodecBackend backend) {
    switch (backend) {
        case CodecBackend::NVENC:
#ifdef OMNIDESK_HAS_NVENC
            return std::make_unique<NvencEncoder>();
#else
            return nullptr;
#endif

        case CodecBackend::VAAPI:
#ifdef OMNIDESK_HAS_VAAPI
            return std::make_unique<VaapiEncoder>();
#else
            return nullptr;
#endif

        case CodecBackend::MF:
#ifdef OMNIDESK_PLATFORM_WINDOWS
            return std::make_unique<MFEncoder>();
#else
            return nullptr;
#endif

        case CodecBackend::OpenH264:
            return std::make_unique<OpenH264Encoder>();
    }

    return nullptr;
}

std::unique_ptr<IDecoder> CodecFactory::createDecoder(CodecBackend backend) {
    switch (backend) {
        case CodecBackend::NVENC:
#ifdef OMNIDESK_HAS_NVENC
            return std::make_unique<NvdecDecoder>();
#else
            return nullptr;
#endif

        case CodecBackend::VAAPI:
#ifdef OMNIDESK_HAS_VAAPI
            return std::make_unique<VaapiDecoder>();
#else
            return nullptr;
#endif

        case CodecBackend::MF:
#ifdef OMNIDESK_PLATFORM_WINDOWS
            return std::make_unique<MFDecoder>();
#else
            return nullptr;
#endif

        case CodecBackend::OpenH264:
            return std::make_unique<OpenH264Decoder>();
    }

    return nullptr;
}

} // namespace omnidesk
