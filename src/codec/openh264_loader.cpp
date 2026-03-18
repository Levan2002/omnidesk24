#include "codec/openh264_loader.h"
#include "core/logger.h"

#ifdef OMNIDESK_OPENH264_DLOPEN

namespace omnidesk {

int  (*pfn_WelsCreateSVCEncoder)(ISVCEncoder**)  = nullptr;
void (*pfn_WelsDestroySVCEncoder)(ISVCEncoder*)  = nullptr;
int  (*pfn_WelsCreateDecoder)(ISVCDecoder**)     = nullptr;
void (*pfn_WelsDestroyDecoder)(ISVCDecoder*)     = nullptr;

bool openh264_load() {
    if (pfn_WelsCreateSVCEncoder) return true;  // already loaded

#ifdef _WIN32
    // Try several common DLL names in order
    static const char* kNames[] = {
        "openh264.dll",
        "libopenh264.dll",
        "libopenh264-7.dll",
        "libopenh264-2.dll",
        nullptr
    };
    HMODULE lib = nullptr;
    for (int i = 0; kNames[i] && !lib; ++i) {
        lib = LoadLibraryA(kNames[i]);
        if (!lib) {
            DWORD err = GetLastError();
            LOG_WARN("OpenH264: LoadLibrary(%s) failed, error=%lu", kNames[i], err);
        }
    }
    if (!lib) return false;
#define GETSYM(h, name) GetProcAddress(h, name)
#else
    static const char* kNames[] = {
        "libopenh264.so.7",
        "libopenh264.so.2",
        "libopenh264.so",
        nullptr
    };
    void* lib = nullptr;
    for (int i = 0; kNames[i] && !lib; ++i)
        lib = dlopen(kNames[i], RTLD_LAZY);
    if (!lib) return false;
#define GETSYM(h, name) dlsym(h, name)
#endif

    pfn_WelsCreateSVCEncoder  = reinterpret_cast<decltype(pfn_WelsCreateSVCEncoder)> (GETSYM(lib, "WelsCreateSVCEncoder"));
    pfn_WelsDestroySVCEncoder = reinterpret_cast<decltype(pfn_WelsDestroySVCEncoder)>(GETSYM(lib, "WelsDestroySVCEncoder"));
    pfn_WelsCreateDecoder     = reinterpret_cast<decltype(pfn_WelsCreateDecoder)>    (GETSYM(lib, "WelsCreateDecoder"));
    pfn_WelsDestroyDecoder    = reinterpret_cast<decltype(pfn_WelsDestroyDecoder)>   (GETSYM(lib, "WelsDestroyDecoder"));

#undef GETSYM

    return pfn_WelsCreateSVCEncoder && pfn_WelsDestroySVCEncoder
        && pfn_WelsCreateDecoder    && pfn_WelsDestroyDecoder;
}

}  // namespace omnidesk

#endif  // OMNIDESK_OPENH264_DLOPEN
