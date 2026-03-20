#pragma once

#include "core/types.h"
#include "codec/encoder.h"
#include "codec/decoder.h"
#include <memory>
#include <string>
#include <vector>

namespace omnidesk {

// Encoder backend types, in priority order.
enum class CodecBackend : uint8_t {
    OmniCodec,  // OmniCodec (custom screen-content codec, highest priority)
    NVENC,      // NVIDIA NVENC (hardware)
    VAAPI,      // VA-API (Linux hardware)
    MF,         // Media Foundation (Windows: Intel QSV, AMD AMF, Intel Arc, etc.)
    OpenH264,   // Cisco OpenH264 (software fallback)
};

// Factory for creating the best available encoder and decoder at runtime.
// Probes for hardware acceleration and falls back to OpenH264.
class CodecFactory {
public:
    // Create the best available encoder. Tries backends in priority order:
    // OmniCodec > NVENC > MF/VAAPI > OpenH264
    static std::unique_ptr<IEncoder> createEncoder();

    // Create the best available decoder. Same priority order.
    static std::unique_ptr<IDecoder> createDecoder();

    // Create an encoder for a specific backend.
    static std::unique_ptr<IEncoder> createEncoder(CodecBackend backend);

    // Create a decoder for a specific backend.
    static std::unique_ptr<IDecoder> createDecoder(CodecBackend backend);

    // Query which backends are available on this system.
    static std::vector<CodecBackend> availableBackends();

    // Get a human-readable name for a backend.
    static const char* backendName(CodecBackend backend);

private:
    static bool isBackendAvailable(CodecBackend backend);
};

} // namespace omnidesk
