include(FetchContent)

# Cisco OpenH264 — built from source as a static library so the exe has
# zero runtime DLL dependencies on openh264.
set(OPENH264_VERSION "2.4.1")

FetchContent_Declare(
    openh264_src
    GIT_REPOSITORY https://github.com/cisco/openh264.git
    GIT_TAG        v${OPENH264_VERSION}
    GIT_SHALLOW    TRUE
)

FetchContent_GetProperties(openh264_src)
if(NOT openh264_src_POPULATED)
    FetchContent_Populate(openh264_src)
endif()

set(OH264 ${openh264_src_SOURCE_DIR})

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Optional NASM x86/x86_64 assembly for SIMD acceleration
# ---------------------------------------------------------------------------
# We split assembly objects by component to avoid collisions
# (encoder and decoder both have dct.asm, intra_pred.asm, etc.)
set(OH264_COMMON_ASM_OBJECTS "")
set(OH264_ENCODER_ASM_OBJECTS "")
set(OH264_DECODER_ASM_OBJECTS "")
set(OH264_PROCESSING_ASM_OBJECTS "")
set(OH264_X86_DEFS "")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|x86|i[3-6]86")
    find_program(NASM_EXECUTABLE nasm)
    if(NASM_EXECUTABLE)
        message(STATUS "OpenH264: NASM found — building with x86 SIMD assembly")

        # Determine NASM output format, NASM defines, and C++ defines.
        # On Windows the upstream meson build does NOT use -DHAVE_AVX2
        # (only -DWIN64 / -DX86_32 -DPREFIX), so AVX2 asm routines are
        # gated out and the C++ code must likewise not reference them.
        if(WIN32)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(NASM_FORMAT win64)
                set(NASM_DEFINES -DWIN64)
                set(OH264_X86_DEFS X86_ASM)          # no HAVE_AVX2
            else()
                set(NASM_FORMAT win32)
                set(NASM_DEFINES -DPREFIX -DX86_32)
                set(OH264_X86_DEFS X86_ASM X86_32_ASM)
            endif()
        elseif(APPLE)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(NASM_FORMAT macho64)
                set(NASM_DEFINES -DUNIX64 -DHAVE_AVX2)
                set(OH264_X86_DEFS X86_ASM HAVE_AVX2)
            else()
                set(NASM_FORMAT macho32)
                set(NASM_DEFINES -DX86_32 -DHAVE_AVX2)
                set(OH264_X86_DEFS X86_ASM X86_32_ASM HAVE_AVX2)
            endif()
        else()
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(NASM_FORMAT elf64)
                set(NASM_DEFINES -DUNIX64 -DHAVE_AVX2)
                set(OH264_X86_DEFS X86_ASM HAVE_AVX2)
            else()
                set(NASM_FORMAT elf)
                set(NASM_DEFINES -DX86_32 -DHAVE_AVX2)
                set(OH264_X86_DEFS X86_ASM X86_32_ASM HAVE_AVX2)
            endif()
        endif()

        set(NASM_INC_DIR "${OH264}/codec/common/x86/")
        set(OH264_ASM_OBJ_DIR "${CMAKE_CURRENT_BINARY_DIR}/openh264_asm")
        file(MAKE_DIRECTORY ${OH264_ASM_OBJ_DIR})

        # Helper function: assemble .asm files and append to a given output list
        function(oh264_assemble_nasm OUT_LIST PREFIX)
            set(_objs "")
            foreach(ASM_SRC ${ARGN})
                get_filename_component(ASM_NAME ${ASM_SRC} NAME_WE)
                set(ASM_OBJ "${OH264_ASM_OBJ_DIR}/${PREFIX}_${ASM_NAME}.o")
                add_custom_command(
                    OUTPUT  ${ASM_OBJ}
                    COMMAND ${NASM_EXECUTABLE}
                            -f ${NASM_FORMAT}
                            -i ${NASM_INC_DIR}
                            ${NASM_DEFINES}
                            ${ASM_SRC}
                            -o ${ASM_OBJ}
                    DEPENDS ${ASM_SRC}
                    COMMENT "NASM [${PREFIX}]: ${ASM_NAME}.asm"
                )
                list(APPEND _objs ${ASM_OBJ})
            endforeach()
            set(${OUT_LIST} ${_objs} PARENT_SCOPE)
        endfunction()

        oh264_assemble_nasm(OH264_COMMON_ASM_OBJECTS "common"
            ${OH264}/codec/common/x86/cpuid.asm
            ${OH264}/codec/common/x86/dct.asm
            ${OH264}/codec/common/x86/deblock.asm
            ${OH264}/codec/common/x86/expand_picture.asm
            ${OH264}/codec/common/x86/intra_pred_com.asm
            ${OH264}/codec/common/x86/mb_copy.asm
            ${OH264}/codec/common/x86/mc_chroma.asm
            ${OH264}/codec/common/x86/mc_luma.asm
            ${OH264}/codec/common/x86/satd_sad.asm
            ${OH264}/codec/common/x86/vaa.asm
        )
        oh264_assemble_nasm(OH264_ENCODER_ASM_OBJECTS "encoder"
            ${OH264}/codec/encoder/core/x86/coeff.asm
            ${OH264}/codec/encoder/core/x86/dct.asm
            ${OH264}/codec/encoder/core/x86/intra_pred.asm
            ${OH264}/codec/encoder/core/x86/matrix_transpose.asm
            ${OH264}/codec/encoder/core/x86/memzero.asm
            ${OH264}/codec/encoder/core/x86/quant.asm
            ${OH264}/codec/encoder/core/x86/sample_sc.asm
            ${OH264}/codec/encoder/core/x86/score.asm
        )
        oh264_assemble_nasm(OH264_DECODER_ASM_OBJECTS "decoder"
            ${OH264}/codec/decoder/core/x86/dct.asm
            ${OH264}/codec/decoder/core/x86/intra_pred.asm
        )
        oh264_assemble_nasm(OH264_PROCESSING_ASM_OBJECTS "processing"
            ${OH264}/codec/processing/src/x86/denoisefilter.asm
            ${OH264}/codec/processing/src/x86/downsample_bilinear.asm
            ${OH264}/codec/processing/src/x86/vaa.asm
        )

        # OH264_X86_DEFS was already set above per-platform
    else()
        message(STATUS "OpenH264: NASM not found — building C++ only (no x86 SIMD)")
    endif()
endif()

# Common compile options for all OpenH264 sub-libraries
set(OH264_COMMON_OPTS -w)   # suppress upstream warnings

# ---------------------------------------------------------------------------
# common — shared between encoder and decoder
# ---------------------------------------------------------------------------
add_library(openh264_common STATIC
    ${OH264}/codec/common/src/common_tables.cpp
    ${OH264}/codec/common/src/copy_mb.cpp
    ${OH264}/codec/common/src/cpu.cpp
    ${OH264}/codec/common/src/crt_util_safe_x.cpp
    ${OH264}/codec/common/src/deblocking_common.cpp
    ${OH264}/codec/common/src/expand_pic.cpp
    ${OH264}/codec/common/src/intra_pred_common.cpp
    ${OH264}/codec/common/src/mc.cpp
    ${OH264}/codec/common/src/memory_align.cpp
    ${OH264}/codec/common/src/sad_common.cpp
    ${OH264}/codec/common/src/utils.cpp
    ${OH264}/codec/common/src/welsCodecTrace.cpp
    ${OH264}/codec/common/src/WelsTaskThread.cpp
    ${OH264}/codec/common/src/WelsThread.cpp
    ${OH264}/codec/common/src/WelsThreadLib.cpp
    ${OH264}/codec/common/src/WelsThreadPool.cpp
    ${OH264_COMMON_ASM_OBJECTS}
)
target_include_directories(openh264_common PUBLIC
    ${OH264}/codec/api
    ${OH264}/codec/api/wels
    ${OH264}/codec/common/inc
)
target_compile_definitions(openh264_common PUBLIC ${OH264_X86_DEFS})
target_compile_options(openh264_common PRIVATE ${OH264_COMMON_OPTS})
target_link_libraries(openh264_common PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# processing — used by the encoder
# ---------------------------------------------------------------------------
add_library(openh264_processing STATIC
    ${OH264}/codec/processing/src/adaptivequantization/AdaptiveQuantization.cpp
    ${OH264}/codec/processing/src/backgrounddetection/BackgroundDetection.cpp
    ${OH264}/codec/processing/src/common/memory.cpp
    ${OH264}/codec/processing/src/common/WelsFrameWork.cpp
    ${OH264}/codec/processing/src/common/WelsFrameWorkEx.cpp
    ${OH264}/codec/processing/src/complexityanalysis/ComplexityAnalysis.cpp
    ${OH264}/codec/processing/src/denoise/denoise.cpp
    ${OH264}/codec/processing/src/denoise/denoise_filter.cpp
    ${OH264}/codec/processing/src/downsample/downsample.cpp
    ${OH264}/codec/processing/src/downsample/downsamplefuncs.cpp
    ${OH264}/codec/processing/src/imagerotate/imagerotate.cpp
    ${OH264}/codec/processing/src/imagerotate/imagerotatefuncs.cpp
    ${OH264}/codec/processing/src/scenechangedetection/SceneChangeDetection.cpp
    ${OH264}/codec/processing/src/scrolldetection/ScrollDetection.cpp
    ${OH264}/codec/processing/src/scrolldetection/ScrollDetectionFuncs.cpp
    ${OH264}/codec/processing/src/vaacalc/vaacalcfuncs.cpp
    ${OH264}/codec/processing/src/vaacalc/vaacalculation.cpp
    ${OH264_PROCESSING_ASM_OBJECTS}
)
target_include_directories(openh264_processing PUBLIC
    ${OH264}/codec/processing/interface
)
target_include_directories(openh264_processing PRIVATE
    ${OH264}/codec/processing/src/common
    ${OH264}/codec/processing/src/adaptivequantization
    ${OH264}/codec/processing/src/downsample
    ${OH264}/codec/processing/src/scrolldetection
    ${OH264}/codec/processing/src/vaacalc
)
target_link_libraries(openh264_processing PUBLIC openh264_common)
target_compile_options(openh264_processing PRIVATE ${OH264_COMMON_OPTS})

# ---------------------------------------------------------------------------
# encoder — has its own core/inc headers (deblocking.h etc.)
# ---------------------------------------------------------------------------
add_library(openh264_encoder STATIC
    ${OH264}/codec/encoder/core/src/au_set.cpp
    ${OH264}/codec/encoder/core/src/deblocking.cpp
    ${OH264}/codec/encoder/core/src/decode_mb_aux.cpp
    ${OH264}/codec/encoder/core/src/encode_mb_aux.cpp
    ${OH264}/codec/encoder/core/src/encoder.cpp
    ${OH264}/codec/encoder/core/src/encoder_data_tables.cpp
    ${OH264}/codec/encoder/core/src/encoder_ext.cpp
    ${OH264}/codec/encoder/core/src/get_intra_predictor.cpp
    ${OH264}/codec/encoder/core/src/md.cpp
    ${OH264}/codec/encoder/core/src/mv_pred.cpp
    ${OH264}/codec/encoder/core/src/nal_encap.cpp
    ${OH264}/codec/encoder/core/src/paraset_strategy.cpp
    ${OH264}/codec/encoder/core/src/picture_handle.cpp
    ${OH264}/codec/encoder/core/src/ratectl.cpp
    ${OH264}/codec/encoder/core/src/ref_list_mgr_svc.cpp
    ${OH264}/codec/encoder/core/src/sample.cpp
    ${OH264}/codec/encoder/core/src/set_mb_syn_cabac.cpp
    ${OH264}/codec/encoder/core/src/set_mb_syn_cavlc.cpp
    ${OH264}/codec/encoder/core/src/slice_multi_threading.cpp
    ${OH264}/codec/encoder/core/src/svc_base_layer_md.cpp
    ${OH264}/codec/encoder/core/src/svc_enc_slice_segment.cpp
    ${OH264}/codec/encoder/core/src/svc_encode_mb.cpp
    ${OH264}/codec/encoder/core/src/svc_encode_slice.cpp
    ${OH264}/codec/encoder/core/src/svc_mode_decision.cpp
    ${OH264}/codec/encoder/core/src/svc_motion_estimate.cpp
    ${OH264}/codec/encoder/core/src/svc_set_mb_syn_cabac.cpp
    ${OH264}/codec/encoder/core/src/svc_set_mb_syn_cavlc.cpp
    ${OH264}/codec/encoder/core/src/wels_preprocess.cpp
    ${OH264}/codec/encoder/core/src/wels_task_base.cpp
    ${OH264}/codec/encoder/core/src/wels_task_encoder.cpp
    ${OH264}/codec/encoder/core/src/wels_task_management.cpp
    ${OH264}/codec/encoder/plus/src/welsEncoderExt.cpp
    ${OH264_ENCODER_ASM_OBJECTS}
)
# IMPORTANT: encoder/core/inc must come BEFORE common/inc so the encoder's
# own deblocking.h, decode_mb_aux.h, etc. take precedence.
target_include_directories(openh264_encoder PRIVATE
    ${OH264}/codec/encoder/core/inc
    ${OH264}/codec/encoder/plus/inc
)
target_link_libraries(openh264_encoder PUBLIC openh264_common openh264_processing)
target_compile_options(openh264_encoder PRIVATE ${OH264_COMMON_OPTS})

# ---------------------------------------------------------------------------
# decoder — has its own core/inc headers (deblocking.h etc.)
# ---------------------------------------------------------------------------
add_library(openh264_decoder STATIC
    ${OH264}/codec/decoder/core/src/au_parser.cpp
    ${OH264}/codec/decoder/core/src/bit_stream.cpp
    ${OH264}/codec/decoder/core/src/cabac_decoder.cpp
    ${OH264}/codec/decoder/core/src/deblocking.cpp
    ${OH264}/codec/decoder/core/src/decode_mb_aux.cpp
    ${OH264}/codec/decoder/core/src/decode_slice.cpp
    ${OH264}/codec/decoder/core/src/decoder.cpp
    ${OH264}/codec/decoder/core/src/decoder_core.cpp
    ${OH264}/codec/decoder/core/src/decoder_data_tables.cpp
    ${OH264}/codec/decoder/core/src/error_concealment.cpp
    ${OH264}/codec/decoder/core/src/fmo.cpp
    ${OH264}/codec/decoder/core/src/get_intra_predictor.cpp
    ${OH264}/codec/decoder/core/src/manage_dec_ref.cpp
    ${OH264}/codec/decoder/core/src/memmgr_nal_unit.cpp
    ${OH264}/codec/decoder/core/src/mv_pred.cpp
    ${OH264}/codec/decoder/core/src/parse_mb_syn_cabac.cpp
    ${OH264}/codec/decoder/core/src/parse_mb_syn_cavlc.cpp
    ${OH264}/codec/decoder/core/src/pic_queue.cpp
    ${OH264}/codec/decoder/core/src/rec_mb.cpp
    ${OH264}/codec/decoder/core/src/wels_decoder_thread.cpp
    ${OH264}/codec/decoder/plus/src/welsDecoderExt.cpp
    ${OH264_DECODER_ASM_OBJECTS}
)
# IMPORTANT: decoder/core/inc must come BEFORE common/inc so the decoder's
# own deblocking.h, decode_mb_aux.h, etc. take precedence.
target_include_directories(openh264_decoder PRIVATE
    ${OH264}/codec/decoder/core/inc
    ${OH264}/codec/decoder/plus/inc
)
target_link_libraries(openh264_decoder PUBLIC openh264_common)
target_compile_options(openh264_decoder PRIVATE ${OH264_COMMON_OPTS})

# ---------------------------------------------------------------------------
# Umbrella INTERFACE target that our project links against.
# Name is "openh264" for drop-in compatibility with the old CMake file.
# ---------------------------------------------------------------------------
add_library(openh264 INTERFACE)
target_link_libraries(openh264 INTERFACE
    openh264_encoder openh264_decoder openh264_processing openh264_common
)
# The public API header path comes from openh264_common (codec/api).

message(STATUS "OpenH264 ${OPENH264_VERSION}: static library built from source (no DLL dependency)")
