include(FetchContent)

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

# Suppress install() rules from libdatachannel and its dependencies
# (they reference FetchContent targets not present in export sets).
set(_save_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES ON)

FetchContent_MakeAvailable(libdatachannel)

set(CMAKE_SKIP_INSTALL_RULES "${_save_skip_install}")
unset(_save_skip_install)
