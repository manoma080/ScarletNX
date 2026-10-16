# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The third-party libraries, fetched at configure time with CPM.cmake (https://github.com/cpm-cmake/CPM.cmake). Each
# package is pinned to a release archive and its SHA-256, and downloaded once into CPM_SOURCE_CACHE (.cache/cpm in the
# repository by default, shared by every build folder). Configuring offline works once the cache is filled.
#
# Bumping a package: change its version, URL and hash together, the hash taken from the release asset's digest or from
# a download checked by hand, never from what the next configure happens to fetch (snx-build skill).
#
# Patching a package: snx_patched_copy (cmake/patched_copy.cmake) on a copy in the build tree, never CPM's PATCHES, which
# patches only what CPM itself downloads into the cache.

set(CPM_VERSION 0.43.2)
set(CPM_HASH 49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa)
if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cache/cpm" CACHE PATH "Where CPM.cmake keeps downloaded packages")
endif()
if(DEFINED CPM_SOURCE_CACHE)
    set(cpm_file "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_VERSION}.cmake")
else()
    set(cpm_file "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_VERSION}.cmake")
endif()
# Checked on every configure, not only after downloading it: a download that failed its check or broke off leaves a
# file behind, and the next configure would include it. The download goes to a .part file, renamed once checked
set(cpm_have "")
if(EXISTS "${cpm_file}")
    file(SHA256 "${cpm_file}" cpm_have)
endif()
if(NOT cpm_have STREQUAL CPM_HASH)
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
         "${cpm_file}.part" EXPECTED_HASH SHA256=${CPM_HASH})
    file(RENAME "${cpm_file}.part" "${cpm_file}")
endif()
include("${cpm_file}")

# Packages whose headers a system may also have (Vulkan) are added without SYSTEM. The compiler searches every
# -I folder before any -isystem one, so as -isystem they lost to an -I/usr/local/include in CFLAGS, usual on FreeBSD,
# and the build mixed in the system's copies. Third-party code builds with its own flags and stays out of the warning
# output
function(snx_quiet)
    foreach(target IN LISTS ARGN)
        target_compile_options(${target} PRIVATE -w)
    endforeach()
endfunction()

# LZ4's block decoder for NSO segments (src/nx/loader/loader.c, src/nx/runtime/loader.c) and hactool's NSO
# extraction. Only lib/lz4.c is built (BSD-2-Clause): the rest of LZ4 is its frame format, the HC compressor and the
# command-line program, none of which an NSO uses
if(SNX_BUILD_NX OR SNX_HACTOOL_ONLY)
    CPMAddPackage(NAME lz4 VERSION 1.10.0
                  URL https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz
                  URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
                  DOWNLOAD_ONLY YES)
    add_library(snx_lz4 STATIC "${lz4_SOURCE_DIR}/lib/lz4.c")
    target_include_directories(snx_lz4 PUBLIC "${lz4_SOURCE_DIR}/lib")
    snx_quiet(snx_lz4)
endif()

# OpenSSL for the hashes and ciphers (the NX and 3DS loaders, ldr, csrng, hactool) and the ssl service's TLS. It builds
# with its own Configure and make, which cmake/openssl.cmake runs
# The archives' SHA-256 are also part of their cache keys (cmake/deps_cache.cmake)
include("${CMAKE_SOURCE_DIR}/cmake/deps_cache.cmake")
if(SNX_BUILD_NX OR SNX_BUILD_3DS OR SNX_HACTOOL_ONLY)
    set(snx_openssl_sha256 325b5c806167c13b40b1ffeadfe0248197c00eccc4cf123ec1e28d2d2fd216d9)
    CPMAddPackage(NAME OpenSSL VERSION 4.0.3
                  URL https://github.com/openssl/openssl/releases/download/openssl-4.0.3/openssl-4.0.3.tar.gz
                  URL_HASH SHA256=${snx_openssl_sha256}
                  DOWNLOAD_ONLY YES)
    include("${CMAKE_SOURCE_DIR}/cmake/openssl.cmake")
endif()

if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND NOT SNX_HACTOOL_ONLY) OR SNX_BUILD_3DS)
    CPMAddPackage(NAME Vulkan-Headers VERSION 1.4.365
                  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.365.tar.gz
                  URL_HASH SHA256=ed832b3292cce324ebba6522fd2d3004bf1b797fa49bfbcad0b302ff23d56abc
                  EXCLUDE_FROM_ALL YES)
    # volk.c is compiled into the vk components, VMA is a header
    CPMAddPackage(NAME volk VERSION 1.4.363
                  URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
                  URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
                  DOWNLOAD_ONLY YES)
endif()

# Extraction-only and tools-only builds do not contain the NX runtime or its graphics/media dependencies.
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND NOT SNX_HACTOOL_ONLY)
    # FFmpeg builds with its own configure and make, which cmake/ffmpeg.cmake runs
    set(snx_ffmpeg_sha256 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e)
    CPMAddPackage(NAME FFmpeg VERSION 9.0.2
                  URL https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
                  URL_HASH SHA256=${snx_ffmpeg_sha256}
                  DOWNLOAD_ONLY YES)
    include("${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake")

    CPMAddPackage(NAME VulkanMemoryAllocator VERSION 3.4.0
                  URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
                  URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1
                  DOWNLOAD_ONLY YES)
endif()

set(SNX_SDL3_FROM_SOURCE OFF)
if((SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND NOT SNX_HACTOOL_ONLY) OR SNX_BUILD_GUI OR SNX_BUILD_3DS)
    # Prefer a compatible system package. CPM handles the pinned static fallback and user overrides.
    CPMFindPackage(NAME SDL3 VERSION 3.4.18 FIND_PACKAGE_ARGUMENTS CONFIG
                   URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
                   URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
                   EXCLUDE_FROM_ALL YES SYSTEM YES
                   OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF"
                           "SDL_EXAMPLES OFF" "SDL_INSTALL OFF" "SDL_UNIX_CONSOLE_BUILD ON")
    if(SDL3_ADDED)
        set(SNX_SDL3_FROM_SOURCE ON)
        snx_quiet(SDL3-static)
        # ccache can't reuse a compile that read a precompiled header: under a launcher, SDL3 goes without one
        if(CMAKE_C_COMPILER_LAUNCHER)
            set_target_properties(SDL3-static PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
        endif()
    endif()
endif()

# hactool runs on the machine doing the extraction (--extract), so a cross build leaves it out, but for Android, where
# the phone extracts its own dumps. The cache keeps it as released; cmake/hactool.cmake applies the decrypted-container
# patch and the OpenSSL one to a copy in the build tree
if((SNX_BUILD_NX OR SNX_HACTOOL_ONLY) AND NOT SNX_TOOLS_ONLY AND (NOT CMAKE_CROSSCOMPILING OR ANDROID))
    set(hactool_patch "${CMAKE_SOURCE_DIR}/cmake/hactool-decrypted.patch")
    set(hactool_openssl_patch "${CMAKE_SOURCE_DIR}/cmake/hactool-openssl.patch")
    CPMAddPackage(NAME hactool VERSION 1.4.0.13
                  URL https://github.com/SciresM/hactool/archive/1d64a83450e025622f3468c28fc4164dad2c5ef6.tar.gz
                  URL_HASH SHA256=f7148daad624ba909f5ae253210734b1a0ce19e89a7914b6cf71401f7134ebbc
                  DOWNLOAD_ONLY YES)
    include("${CMAKE_SOURCE_DIR}/cmake/hactool.cmake")
endif()
