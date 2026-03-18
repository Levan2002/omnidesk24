include(FetchContent)

# ── Save original compiler flags ─────────────────────────────────────
# We temporarily inject defines/flags for the FetchContent sub-projects
# (MbedTLS, libsrtp, libdatachannel) and restore afterwards so our own
# targets are not affected.
set(_save_c_flags   "${CMAKE_C_FLAGS}")
set(_save_cxx_flags "${CMAKE_CXX_FLAGS}")

# ── MbedTLS (TLS backend for libdatachannel) ──────────────────────────
# Fetch MbedTLS 3.6.x so we don't require a system OpenSSL installation.
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        mbedtls-3.6.5
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# MbedTLS build options
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
# Disable -Werror for MbedTLS — MinGW GCC 15 triggers a format warning in
# ssl_client.c (MBEDTLS_PRINTF_LONGLONG / I64 modifier) that is benign.
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

# Enable DTLS-SRTP support (required by libdatachannel).  The feature is
# disabled by default in MbedTLS 3.6.  We supply a user-config header that
# defines MBEDTLS_SSL_PROTO_TLS1_2, MBEDTLS_SSL_PROTO_DTLS and
# MBEDTLS_SSL_DTLS_SRTP.  The definition must reach all sub-projects that
# include MbedTLS headers (mbedtls, libsrtp, libdatachannel).
set(_omnidesk_mbedtls_user_config "${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h")
string(REPLACE "\\" "/" _omnidesk_mbedtls_user_config "${_omnidesk_mbedtls_user_config}")
string(APPEND CMAKE_C_FLAGS   " -DMBEDTLS_USER_CONFIG_FILE=\"\\\"${_omnidesk_mbedtls_user_config}\\\"\"")
string(APPEND CMAKE_CXX_FLAGS " -DMBEDTLS_USER_CONFIG_FILE=\"\\\"${_omnidesk_mbedtls_user_config}\\\"\"")

FetchContent_MakeAvailable(mbedtls)

# Create IMPORTED targets that libdatachannel + libsrtp expect from
# find_package(MbedTLS).  We cannot use ALIAS because the downstream
# FindMbedTLS.cmake calls set_property on these targets.
FetchContent_GetProperties(mbedtls SOURCE_DIR _mbedtls_src)

foreach(_tgt MbedTLS MbedX509 MbedCrypto)
    if(NOT TARGET MbedTLS::${_tgt})
        add_library(MbedTLS::${_tgt} INTERFACE IMPORTED)
    endif()
endforeach()

set_target_properties(MbedTLS::MbedCrypto PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_mbedtls_src}/include"
    INTERFACE_LINK_LIBRARIES      "mbedcrypto"
)
set_target_properties(MbedTLS::MbedX509 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_mbedtls_src}/include"
    INTERFACE_LINK_LIBRARIES      "mbedx509"
)
set_target_properties(MbedTLS::MbedTLS PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_mbedtls_src}/include"
    INTERFACE_LINK_LIBRARIES      "mbedtls;MbedTLS::MbedCrypto;MbedTLS::MbedX509"
    VERSION                       "3.6.5"
)

# Set cache variables so that libsrtp's find_package(MbedTLS) and
# libdatachannel's FindMbedTLS.cmake both succeed without searching.
set(MbedTLS_FOUND TRUE CACHE BOOL "" FORCE)
set(MbedTLS_VERSION "3.6.5" CACHE STRING "" FORCE)
set(MbedTLS_INCLUDE_DIR "${_mbedtls_src}/include" CACHE PATH "" FORCE)
set(MbedTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MbedCrypto_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(MbedX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDTLS_INCLUDE_DIRS "${_mbedtls_src}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARIES "mbedtls;mbedx509;mbedcrypto" CACHE STRING "" FORCE)

# ── libdatachannel ────────────────────────────────────────────────────
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.24.1
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)

# Use MbedTLS instead of OpenSSL (no system OpenSSL needed)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)

# Static library, enable media support (H264 RTP), disable WebSocket
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Disable -Werror in libsrtp — MinGW GCC 15 triggers format warnings
# (%zu/%llu) in debug_print macros that are harmless.
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

# Suppress install() rules from libdatachannel and its dependencies
# (they reference FetchContent targets not present in export sets).
set(_save_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES ON)

FetchContent_MakeAvailable(libdatachannel)

set(CMAKE_SKIP_INSTALL_RULES "${_save_skip_install}")
unset(_save_skip_install)

# ── Restore original compiler flags ──────────────────────────────────
set(CMAKE_C_FLAGS   "${_save_c_flags}")
set(CMAKE_CXX_FLAGS "${_save_cxx_flags}")
unset(_save_c_flags)
unset(_save_cxx_flags)
