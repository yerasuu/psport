# psport for Windows (decision #623, kb #926): the GPU library (gpu/) and the programs build.sh's
# Windows branch used to compile by hand, so that MSYS2's CLANG64 shell (build.sh) and a CMake
# toolchain file without MSYS2 (decanter's builds) build them with one cmake and ninja.
# Linux keeps build.sh's own path (PGO, rpaths, the online module, the DLSS bridge).
cmake_minimum_required(VERSION 3.24)
project(psport LANGUAGES C CXX)
if(NOT WIN32)
    message(FATAL_ERROR "This project builds psport for Windows; on Linux run build.sh")
endif()

# build.sh's rule: X.Y.Z only with its notes written, 0-unstable between releases.
file(STRINGS VERSION PS_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)
if(PS_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(ps_notes "${CMAKE_CURRENT_SOURCE_DIR}/tag-changelog/v${PS_VERSION}.md")
    if(EXISTS "${ps_notes}")
        file(SIZE "${ps_notes}" ps_notes_size)
    endif()
    if(NOT ps_notes_size)
        message(FATAL_ERROR "psport ${PS_VERSION} is a release: write its notes in tag-changelog/v${PS_VERSION}.md first.")
    endif()
elseif(NOT PS_VERSION STREQUAL "0-unstable")
    message(FATAL_ERROR "VERSION holds '${PS_VERSION}': expected X.Y.Z or 0-unstable.")
endif()
set(PS_COMMIT "" CACHE STRING "Commit the programs name (git rev-parse --short=12 HEAD), empty outside a checkout")
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gpu/third_party/imgui/imgui.h")
    message(FATAL_ERROR "gpu/third_party/imgui is missing: git submodule update --init gpu/third_party/imgui")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# No PGO (its flags are GCC's), no online module, DLSS bridge or Tracy yet.
set(PS_PGO off)
set(PS_NET OFF)
add_subdirectory(gpu)

# The programs take build.sh's flags as they are, without the build type's (-DNDEBUG would drop the
# runtime's asserts).
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_C_FLAGS_${config} "")
endforeach()
find_package(PkgConfig REQUIRED)
pkg_check_modules(PS_VULKAN REQUIRED vulkan)
pkg_check_modules(PS_VULKAN_SDL3 REQUIRED vulkan sdl3)
# pkg-config's results as include directories and full library paths, which CMake quotes, since its
# -I and -L flags break at a space in the toolchain's path. psport.exe is a console program, so that
# its log reaches stdout; the capability check keeps SDL3's -mwindows.

add_library(psport-atrac9 STATIC)
file(GLOB ps_atrac9_sources CONFIGURE_DEPENDS third_party/LibAtrac9/C/src/*.c)
target_sources(psport-atrac9 PRIVATE ${ps_atrac9_sources})
set_target_properties(psport-atrac9 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(psport-atrac9 PRIVATE -O2 -g -w)

# The runtime: the Linux build's sources with src/platform/win32 in place of src/platform/linux, and
# posix_compat.h for the C library calls UCRT lacks. Its runtime*.c files in the order of their names,
# as the flat src/ listed them, so that the link order and the binary's layout stay.
file(GLOB ps_runtime_paths CONFIGURE_DEPENDS src/*/runtime*.c)
set(ps_runtime_keys "")
foreach(path IN LISTS ps_runtime_paths)
    get_filename_component(name "${path}" NAME)
    list(APPEND ps_runtime_keys "${name}|${path}")
endforeach()
list(SORT ps_runtime_keys)
list(TRANSFORM ps_runtime_keys REPLACE "^[^|]*\\|" "" OUTPUT_VARIABLE ps_runtime_sources)
file(GLOB ps_loader_sources CONFIGURE_DEPENDS src/loader/loader*.c)
file(GLOB ps_platform_sources CONFIGURE_DEPENDS src/platform/*.c)
file(GLOB ps_common_sources CONFIGURE_DEPENDS src/platform/common/*.c)
file(GLOB ps_win32_sources CONFIGURE_DEPENDS src/platform/win32/*.c)
add_executable(psport src/main/main.c src/main/cli.c src/main/game_info.c src/main/bind.c src/main/signals.c
    ${ps_loader_sources} ${ps_runtime_sources} ${ps_platform_sources} ${ps_common_sources} ${ps_win32_sources} src/main/vulkan_smoke.c)
set_target_properties(psport PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(psport PRIVATE -O2 -g -Wall -Wextra -Werror -pthread
    "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/posix_compat.h\"" ${PS_VULKAN_SDL3_CFLAGS_OTHER})
target_compile_definitions(psport PRIVATE PS_VERSION="${PS_VERSION}" PS_COMMIT="${PS_COMMIT}")
target_include_directories(psport PRIVATE ${PS_VULKAN_SDL3_INCLUDE_DIRS} . src/main src/loader src/runtime
    src/runtime/imports src/kernel src/system src/audio src/input src/net src/platform)
# It exports what libpsport-gpu.dll imports (gpu/port/psport_runtime.def, handed to the linker as
# build.sh did, since CMake passes a .def source only to DLLs) and links only that library's import
# library, not what the library itself links. Its low addresses stay free for the guest without
# high-entropy ASLR (ps_host_vm_reserve_arena).
set(ps_runtime_def "${CMAKE_CURRENT_SOURCE_DIR}/gpu/port/psport_runtime.def")
add_dependencies(psport psport-gpu)
target_link_libraries(psport PRIVATE psport-atrac9 "${ps_runtime_def}" $<TARGET_LINKER_FILE:psport-gpu>
    ${PS_VULKAN_SDL3_LINK_LIBRARIES} -pthread ws2_32 bcrypt synchronization onecore)
target_link_options(psport PRIVATE -O2 -g -Wl,--disable-high-entropy-va)
set_property(TARGET psport APPEND PROPERTY LINK_DEPENDS "${ps_runtime_def}")

add_executable(psport-gpu-capabilities tools/gpu_capabilities.c src/runtime/runtime_quirks.c
    src/runtime/runtime_json_print.c)
set_target_properties(psport-gpu-capabilities PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(psport-gpu-capabilities PRIVATE -O2 -Wall -Wextra -Werror ${PS_VULKAN_SDL3_CFLAGS_OTHER})
target_include_directories(psport-gpu-capabilities PRIVATE ${PS_VULKAN_SDL3_INCLUDE_DIRS} src/runtime)
target_link_libraries(psport-gpu-capabilities PRIVATE ${PS_VULKAN_SDL3_LINK_LIBRARIES}
    ${PS_VULKAN_SDL3_LDFLAGS_OTHER})
target_link_options(psport-gpu-capabilities PRIVATE -O2)

# Vulkan only through LoadLibrary, so that it reports a machine where the loader or driver is missing.
file(GLOB ps_probe_sources CONFIGURE_DEPENDS tools/win32_probe/*.c)
add_executable(psport-windows-probe ${ps_probe_sources})
set_target_properties(psport-windows-probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(psport-windows-probe PRIVATE -O2 -Wall -Wextra -Werror -municode ${PS_VULKAN_CFLAGS_OTHER})
target_include_directories(psport-windows-probe PRIVATE ${PS_VULKAN_INCLUDE_DIRS})
target_compile_definitions(psport-windows-probe PRIVATE PS_VERSION="${PS_VERSION}" PS_COMMIT="${PS_COMMIT}")
target_link_libraries(psport-windows-probe PRIVATE advapi32)
target_link_options(psport-windows-probe PRIVATE -O2 -municode)
