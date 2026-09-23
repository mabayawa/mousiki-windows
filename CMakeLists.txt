cmake_minimum_required(VERSION 3.16)
project(mousiki VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Third-party sources.
#
# miniaudio.h and kissfft are vendored in third_party/, so on a normal clone
# nothing is fetched. The download below is only a fallback for a tree where
# they are missing -- and it is pinned: upstream's version of this fetched
# from a moving `master` branch with no integrity check at all, so a fresh
# clone could silently compile against different code from the one that was
# reviewed. EXPECTED_HASH turns that into a loud failure instead.
#
# The hashes are of the exact vendored files (miniaudio v0.11.25); if you
# intentionally bump either dependency, update the hash in the same commit.
# ---------------------------------------------------------------------------
set(MINIAUDIO_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(MINIAUDIO_HEADER "${MINIAUDIO_DIR}/miniaudio.h")
if(NOT EXISTS "${MINIAUDIO_HEADER}")
  message(STATUS "Downloading miniaudio.h ...")
  file(DOWNLOAD
    "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
    "${MINIAUDIO_HEADER}"
    SHOW_PROGRESS
    EXPECTED_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    STATUS MA_DL_STATUS)
  list(GET MA_DL_STATUS 0 MA_DL_CODE)
  if(NOT MA_DL_CODE EQUAL 0)
    message(FATAL_ERROR "Failed to download miniaudio.h — download it manually into ${MINIAUDIO_DIR}/miniaudio.h")
  endif()
endif()

set(KISSFFT_DIR "${CMAKE_SOURCE_DIR}/third_party/kissfft")
set(KISSFFT_BASE_URL "https://raw.githubusercontent.com/mborgerding/kissfft/master")
set(KISSFFT_FILES
  kiss_fft.c       84154815c4e734bdc986fae5f2301212b1ed4dd91e005bc88680218d4bc3bae3
  kiss_fft.h       6907d7d90187eab2cd1e264e11d986b322e419e1a543846d4529da0f23218d47
  _kiss_fft_guts.h 8140ad59872effc33922c954f3a682a2dc5668532de8000a134e1a6c0e994595
  kiss_fft_log.h   f954cb6890ec999f7fbc80cdd1c8c0194bbaeb0f1c27e11bd23450f53871cf8c
  kiss_fftr.c      df411538fa8870c090ab0d3232775fe97416ae6811b7c7df0e8ee01a7250276d
  kiss_fftr.h      a0f72c1bddc3126d7a51ad80e965b58f6af77e4419aa882a0b11951e783632a8)
file(MAKE_DIRECTORY "${KISSFFT_DIR}")
list(LENGTH KISSFFT_FILES KISSFFT_ENTRIES)
math(EXPR KISSFFT_LAST "${KISSFFT_ENTRIES} / 2 - 1")
foreach(i RANGE ${KISSFFT_LAST})
  math(EXPR NAME_IDX "${i} * 2")
  math(EXPR HASH_IDX "${i} * 2 + 1")
  list(GET KISSFFT_FILES ${NAME_IDX} kf)
  list(GET KISSFFT_FILES ${HASH_IDX} kf_hash)
  if(NOT EXISTS "${KISSFFT_DIR}/${kf}")
    message(STATUS "Downloading kissfft/${kf} ...")
    file(DOWNLOAD "${KISSFFT_BASE_URL}/${kf}" "${KISSFFT_DIR}/${kf}"
         EXPECTED_HASH SHA256=${kf_hash}
         STATUS KF_DL_STATUS)
    list(GET KF_DL_STATUS 0 KF_DL_CODE)
    if(NOT KF_DL_CODE EQUAL 0)
      message(FATAL_ERROR "Failed to download kissfft/${kf} — download the kissfft repo manually into ${KISSFFT_DIR}")
    endif()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Sources
#
# Globbing picks up every src/*.cpp, which is how win_compat.cpp joins the
# build automatically. The file is guarded by #ifdef _WIN32 internally and
# compiles to nothing elsewhere, so there is no per-platform source list to
# keep in sync.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MUISC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
set(KISSFFT_SOURCES "${KISSFFT_DIR}/kiss_fft.c" "${KISSFFT_DIR}/kiss_fftr.c")
set_source_files_properties(${KISSFFT_SOURCES} PROPERTIES LANGUAGE C)

add_executable(mousiki ${MUISC_SOURCES} ${KISSFFT_SOURCES})
target_include_directories(mousiki PRIVATE "${CMAKE_SOURCE_DIR}/src" "${MINIAUDIO_DIR}" "${KISSFFT_DIR}")

# ---------------------------------------------------------------------------
# Platform-specific audio backend wiring (compile-time selected)
# miniaudio auto-detects backends at runtime, but we pin the preferred
# backend per-platform here so the intended native API is what gets tried
# first: PipeWire on Linux, WASAPI on Windows, OpenSL ES on Android.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(mousiki PRIVATE Threads::Threads)

if(ANDROID)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_ANDROID=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_OPENSL=1)
  target_link_libraries(mousiki PRIVATE OpenSLES log android)
elseif(WIN32)
  target_compile_definitions(mousiki PRIVATE
    MUISC_PLATFORM_WINDOWS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_WASAPI=1
    # miniaudio drags in windows.h, whose min/max macros collide with the
    # std::min / std::max / std::clamp used throughout the renderer.
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    # fft_visualizer.cpp's Hann window needs M_PI, which is not standard C++
    # and which MSVC only defines behind this.
    _USE_MATH_DEFINES
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS)
  # ole32    -> CoTaskMemFree, and miniaudio's WASAPI/COM usage
  # winmm    -> miniaudio
  # shell32  -> SHGetKnownFolderPath (the profile and Music known folders)
  target_link_libraries(mousiki PRIVATE ole32 winmm shell32)
elseif(APPLE)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_MACOS=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_COREAUDIO=1)
  target_link_libraries(mousiki PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation" "-framework AudioUnit")
elseif(UNIX AND NOT APPLE)
  target_compile_definitions(mousiki PRIVATE MUISC_PLATFORM_LINUX=1 MA_ENABLE_ONLY_SPECIFIC_BACKENDS=1 MA_ENABLE_PULSEAUDIO=1 MA_ENABLE_ALSA=1)
  # Note: miniaudio talks to PipeWire through its PulseAudio *and* ALSA
  # compatibility shims (pipewire-pulse / pipewire-alsa) — there is no
  # separate native "ma_backend_pipewire" enum in upstream miniaudio, so we
  # enable both shims and let the PipeWire session manager intercept them.
  target_link_libraries(mousiki PRIVATE dl m)
endif()

if(MSVC)
  target_compile_options(mousiki PRIVATE
    # /utf-8 is mandatory, not cosmetic: disk_art.cpp embeds its braille
    # artwork as raw UTF-8 in the source, and settings.h carries the
    # box-drawing characters. Without this MSVC reads the source as the
    # system ANSI code page and silently mangles them.
    /utf-8
    /EHsc
    /W3
    # app.cpp exceeds the default object-file section limit.
    /bigobj
    # Make __cplusplus report the real standard rather than 199711L.
    /Zc:__cplusplus)
endif()

if(MINGW)
  # Produce an executable that does not need the GCC runtime DLLs alongside it.
  target_link_options(mousiki PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# The Python lyrics/search helpers must sit next to the binary: app.cpp's
# find_lyrics_script() looks in <exe dir>/scripts before falling back to the
# working directory, which is what lets mousiki be launched from anywhere.
# ---------------------------------------------------------------------------
add_custom_command(TARGET mousiki POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/scripts"
          "$<TARGET_FILE_DIR:mousiki>/scripts"
  COMMENT "Copying helper scripts next to the executable")

include(GNUInstallDirs)
install(TARGETS mousiki RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_SOURCE_DIR}/scripts/"
        DESTINATION ${CMAKE_INSTALL_BINDIR}/scripts
        FILES_MATCHING PATTERN "*.py")

message(STATUS "mousiki: building for ${CMAKE_SYSTEM_NAME}")
