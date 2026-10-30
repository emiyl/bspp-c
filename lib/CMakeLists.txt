add_library(bspp STATIC
    io.c
    workers.c
    datawin.c
    image.c
    audio.c
    stb_vorbis.c
    pipeline.c
    vita.c
)

if(ENABLE_VITA)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  # Use only upstream's sources, not its tools or directory-wide compiler flags.
  FetchContent_Declare(basis_universal
    URL https://github.com/BinomialLLC/basis_universal/archive/323239a6a5ffa57d6570cfc403be99156e33a8b0.tar.gz
    URL_HASH SHA256=ffe79665119680cb3a2a15c1725577f9fa97b223eba4125619ece28728a41508
    SOURCE_SUBDIR bspp-library-only
  )
  FetchContent_MakeAvailable(basis_universal)
  set(basis_sources
    encoder/basisu_backend.cpp
    encoder/basisu_basis_file.cpp
    encoder/basisu_comp.cpp
    encoder/basisu_enc.cpp
    encoder/basisu_etc.cpp
    encoder/basisu_frontend.cpp
    encoder/basisu_gpu_texture.cpp
    encoder/basisu_pvrtc1_4.cpp
    encoder/basisu_resampler.cpp
    encoder/basisu_resample_filters.cpp
    encoder/basisu_ssim.cpp
    encoder/basisu_uastc_enc.cpp
    encoder/basisu_bc7enc.cpp
    encoder/jpgd.cpp
    encoder/basisu_kernels_sse.cpp
    encoder/basisu_opencl.cpp
    encoder/pvpngreader.cpp
    encoder/basisu_uastc_hdr_4x4_enc.cpp
    encoder/basisu_astc_hdr_6x6_enc.cpp
    encoder/basisu_astc_hdr_common.cpp
    encoder/3rdparty/android_astc_decomp.cpp
    encoder/3rdparty/tinyexr.cpp
    transcoder/basisu_transcoder.cpp
  )
  list(TRANSFORM basis_sources PREPEND "${basis_universal_SOURCE_DIR}/")
  add_library(bspp-basis STATIC ${basis_sources})
  target_compile_features(bspp-basis PUBLIC cxx_std_17)
  target_compile_options(bspp-basis PRIVATE -fno-strict-aliasing)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(bspp-basis PRIVATE
      -Wno-unknown-warning-option -Wno-nontrivial-memcall)
  endif()
  target_compile_definitions(bspp-basis PUBLIC
    BASISU_SUPPORT_SSE=0 BASISU_SUPPORT_OPENCL=0 BASISD_SUPPORT_KTX2_ZSTD=0)
  target_include_directories(bspp-basis SYSTEM PUBLIC "${basis_universal_SOURCE_DIR}")
  target_link_libraries(bspp-basis PUBLIC Threads::Threads)
  target_sources(bspp PRIVATE basis_bridge.cpp)
  target_compile_features(bspp PRIVATE cxx_std_17)
  target_compile_options(bspp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-strict-aliasing>)
  target_link_libraries(bspp PUBLIC bspp-basis)
endif()

target_include_directories(bspp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bspp PUBLIC BSPP_ENABLE_VITA=$<BOOL:${ENABLE_VITA}>)
target_compile_definitions(bspp PUBLIC _POSIX_C_SOURCE=200809L)
if(APPLE)
  target_compile_definitions(bspp PUBLIC _DARWIN_C_SOURCE)
endif()
target_compile_options(bspp PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
target_compile_definitions(bspp PRIVATE STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API)
target_link_libraries(bspp PUBLIC PNG::PNG BZip2::BZip2 PkgConfig::PCRE2 m)
target_compile_definitions(bspp PUBLIC BSPP_ENABLE_PARALLEL=$<BOOL:${ENABLE_PARALLEL}>)
if(ENABLE_PARALLEL)
  target_link_libraries(bspp PUBLIC Threads::Threads)
endif()

if(ENABLE_SANITIZERS)
  target_compile_options(bspp PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(bspp PUBLIC -fsanitize=address,undefined)
endif()
