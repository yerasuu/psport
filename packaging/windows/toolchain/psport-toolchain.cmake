# CMake toolchain file of psport's Windows toolchain package (tools/win32-toolchain.sh, kb #926), at
# the package's root: llvm-mingw's clang and lld aimed at x86_64-w64-windows-gnu over the CLANG64
# packages in clang64/ (libc++, libunwind, compiler-rt, UCRT), as MSYS2's CLANG64 shell builds psport
# in .github/workflows/windows.yml. PS_LLVM_MINGW is the llvm-mingw folder manifest.json names, which
# the caller downloads and passes; nothing else is looked up on the host.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PS_LLVM_MINGW "${PS_LLVM_MINGW}" CACHE PATH "llvm-mingw's folder, as manifest.json pins it")
if(NOT IS_DIRECTORY "${PS_LLVM_MINGW}/bin")
    message(FATAL_ERROR "PS_LLVM_MINGW must name llvm-mingw's folder (manifest.json), not '${PS_LLVM_MINGW}'")
endif()
# try_compile projects read this file again with only these variables.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS_LLVM_MINGW)

set(_ps_suffix "")
if(CMAKE_HOST_WIN32)
    set(_ps_suffix .exe)
endif()
set(CMAKE_C_COMPILER "${PS_LLVM_MINGW}/bin/clang${_ps_suffix}")
set(CMAKE_CXX_COMPILER "${PS_LLVM_MINGW}/bin/clang++${_ps_suffix}")
set(CMAKE_RC_COMPILER "${PS_LLVM_MINGW}/bin/llvm-windres${_ps_suffix}")
set(CMAKE_AR "${PS_LLVM_MINGW}/bin/llvm-ar${_ps_suffix}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${PS_LLVM_MINGW}/bin/llvm-ranlib${_ps_suffix}" CACHE FILEPATH "")
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/clang64")

# llvm-mingw's own configuration files would pick its bundled sysroot's defaults; the runtime is named
# here instead. Unused in a C compile, the C++ library flag would fail -Werror.
set(_ps_flags "--no-default-config --start-no-unused-arguments -rtlib=compiler-rt -unwindlib=libunwind")
string(APPEND _ps_flags " -stdlib=libc++ -fuse-ld=lld --end-no-unused-arguments")
set(CMAKE_C_FLAGS_INIT "${_ps_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_ps_flags}")
set(CMAKE_RC_FLAGS_INIT "--target=pe-x86-64 \"--preprocessor-arg=--sysroot=${CMAKE_SYSROOT}\"")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_PREFIX_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(PS_DLLTOOL "${PS_LLVM_MINGW}/bin/llvm-dlltool${_ps_suffix}" CACHE FILEPATH "")

# CLANG64's pkgconf and glslang run on a Windows host; elsewhere (a check from Linux) the host's.
# Some .pc files name /clang64 outright (FFmpeg's, fmt's), so every path is put under the package's
# root instead of letting pkgconf on Windows derive each prefix from the file's folder.
unset(ENV{PKG_CONFIG_PATH})
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_CURRENT_LIST_DIR}")
set(ENV{PKG_CONFIG_DONT_DEFINE_PREFIX} 1)
if(CMAKE_HOST_WIN32)
    set(PKG_CONFIG_EXECUTABLE "${CMAKE_SYSROOT}/bin/pkgconf.exe" CACHE FILEPATH "")
    set(GLSLANG_COMPILER "${CMAKE_SYSROOT}/bin/glslang.exe" CACHE FILEPATH "")
    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/lib/pkgconfig;${CMAKE_SYSROOT}/share/pkgconfig")
else()
    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/lib/pkgconfig:${CMAKE_SYSROOT}/share/pkgconfig")
endif()

# gpu/CMakeLists.txt fetches these when no package has them: here from the package, never the network.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "")
set(FETCHCONTENT_SOURCE_DIR_MAGIC_ENUM "${CMAKE_CURRENT_LIST_DIR}/sources/magic_enum" CACHE PATH "")
set(FETCHCONTENT_SOURCE_DIR_MINIZ "${CMAKE_CURRENT_LIST_DIR}/sources/miniz" CACHE PATH "")
