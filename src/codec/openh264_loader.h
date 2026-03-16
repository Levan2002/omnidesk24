#pragma once

// Runtime loader for OpenH264 — avoids a hard DLL dependency on Windows.
// When OMNIDESK_OPENH264_DLOPEN is defined the symbols are loaded via
// LoadLibrary / dlopen instead of being linked at build time.

#include <wels/codec_api.h>

#ifdef OMNIDESK_OPENH264_DLOPEN

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace omnidesk {

// Returns true if the library was loaded (or was already loaded).
bool openh264_load();

// Function pointers — set after a successful openh264_load().
extern int  (*pfn_WelsCreateSVCEncoder)(ISVCEncoder**);
extern void (*pfn_WelsDestroySVCEncoder)(ISVCEncoder*);
extern int  (*pfn_WelsCreateDecoder)(ISVCDecoder**);
extern void (*pfn_WelsDestroyDecoder)(ISVCDecoder*);

}  // namespace omnidesk

// Redirect the bare symbol names used throughout the codec files.
#define WelsCreateSVCEncoder  ::omnidesk::pfn_WelsCreateSVCEncoder
#define WelsDestroySVCEncoder ::omnidesk::pfn_WelsDestroySVCEncoder
#define WelsCreateDecoder     ::omnidesk::pfn_WelsCreateDecoder
#define WelsDestroyDecoder    ::omnidesk::pfn_WelsDestroyDecoder

#else  // static / shared-lib link — symbols resolved at link time

namespace omnidesk {
inline bool openh264_load() { return true; }
}

#endif  // OMNIDESK_OPENH264_DLOPEN
