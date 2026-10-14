# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (PS_LTO, default on) and profile-guided optimization
# (PS_PGO: off, generate = instrumented build writing into PS_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(PS_LTO "Link-time optimization of bbgpu" ON)
set(PS_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(PS_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (PS_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (PS_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${PS_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${PS_PGO_DIR})
    add_compile_definitions(PS_PGO_GENERATE)
elseif (PS_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${PS_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(magic_enum REQUIRED)
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
find_package(miniz REQUIRED)
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(fsr4_color.comp fsr4_decode_comp)
add_host_shader(fsr4_color.comp fsr4_encode_comp ENCODE=1)
add_host_shader(fsr4_reactive.comp fsr4_reactive_comp)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(occlusion_resolve.comp occlusion_resolve_comp)
add_host_shader(gpu_timestamp.comp gpu_timestamp_comp)
add_host_shader(indirect_guard.comp indirect_guard_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# The layer's own modules (independent of the shadPS4 code): layer/.
file(GLOB LAYER_SOURCES CONFIGURE_DEPENDS layer/*.cpp)
# Game profiles (what the translator knows about one game, out of its core): games/.
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS games/*.cpp games/*/*.cpp)
# The translator's own command processor (docs/en/history/TRANSLATOR_ARCHITECTURE.md, step 2): cp/.
file(GLOB CP_SOURCES CONFIGURE_DEPENDS cp/*.cpp)
# Ngs2, the software audio mixer of titles other than Bloodborne (ngs2/README.md), with the ATRAC9
# decoder its waveforms use.
file(GLOB NGS2_SOURCES CONFIGURE_DEPENDS ngs2/*.cpp)
# libSceFiber, the fibers libSceUlt's job threads run on (fiber/README.md).
file(GLOB FIBER_SOURCES CONFIGURE_DEPENDS fiber/*.cpp)
# libSceFont and libSceFontFt, text rendered over FreeType (font/README.md).
file(GLOB FONT_SOURCES CONFIGURE_DEPENDS font/*.cpp)
# libSceAudiodec, AAC and MP3 over FFmpeg's libavcodec, ATRAC9 over LibAtrac9 (audiodec/README.md).
file(GLOB AUDIODEC_SOURCES CONFIGURE_DEPENDS audiodec/*.cpp)
# libScePngEnc over miniz's PNG writer.
file(GLOB PNGENC_SOURCES CONFIGURE_DEPENDS pngenc/*.cpp)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
file(GLOB ATRAC9_SOURCES CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
add_library(bbgpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES} ${LAYER_SOURCES} ${GAME_SOURCES} ${CP_SOURCES}
    ${NGS2_SOURCES} ${FIBER_SOURCES} ${FONT_SOURCES} ${AUDIODEC_SOURCES} ${PNGENC_SOURCES} ${ATRAC9_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp shadps4/core/libraries/videodec/videodec2.cpp
    shadps4/core/libraries/videodec/videodec2_impl.cpp)
set_target_properties(bbgpu PROPERTIES OUTPUT_NAME psport-gpu)
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
# Dear ImGui stays private to libpsport-gpu.so. The library is linked into psport, so its exports are
# in the global symbol scope; a Vulkan layer carrying its own Dear ImGui without -Bsymbolic
# (vkShade 0.1) had its ImGui_ImplVulkan_Init bound to this copy, which called the Vulkan loader
# with the layer's down-chain physical device handle ("Invalid physicalDevice", abort).
set_source_files_properties(third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    PS_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
target_include_directories(bbgpu PRIVATE shim layer games cp shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include
    ../third_party/LibAtrac9/C/src)
# Third-party C, built as its upstream ships it (build.sh builds the runtime's copy with -w too).
set_source_files_properties(${ATRAC9_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Loaded with psport at startup (DT_NEEDED, never dlopen): thread_local access without a
# TLS descriptor call (_dl_tlsdesc_return was ~2% of the draw recording thread).
target_compile_options(bbgpu PRIVATE -ftls-model=initial-exec)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against psport.
target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::X11 PkgConfig::FFMPEG PkgConfig::FREETYPE miniz::miniz Zydis::Zydis sirit)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

# bbport online module (gpu/bbnet, libbbnet.so): built when its libraries are there (shell.nix);
# PS_NET=OFF leaves it out. Without it the port plays offline.
option(PS_NET "Build the online module (shadNet)" ON)
if (PS_NET)
    find_package(Protobuf CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(OpenSSL QUIET)
    find_package(ZLIB QUIET)
    find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc)
    find_library(MINIUPNPC_LIBRARY NAMES miniupnpc)
    if (Protobuf_FOUND AND nlohmann_json_FOUND AND OpenSSL_FOUND AND ZLIB_FOUND AND
        MINIUPNPC_INCLUDE AND MINIUPNPC_LIBRARY)
        add_subdirectory(bbnet)
        message(STATUS "Online module: on (libbbnet.so)")
    else()
        message(STATUS "Online module: off (protobuf, nlohmann_json, OpenSSL, zlib or miniupnpc missing)")
    endif()
endif()
