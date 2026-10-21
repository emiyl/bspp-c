add_library(bspp STATIC
    io.c
    datawin.c
    image.c
    audio.c
    pipeline.c
)

target_include_directories(bspp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bspp PUBLIC _POSIX_C_SOURCE=200809L)
if(APPLE)
  target_compile_definitions(bspp PUBLIC _DARWIN_C_SOURCE)
endif()
target_compile_options(bspp PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(bspp PUBLIC PNG::PNG BZip2::BZip2 PkgConfig::VORBIS PkgConfig::PCRE2 m)

if(ENABLE_SANITIZERS)
  target_compile_options(bspp PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(bspp PUBLIC -fsanitize=address,undefined)
endif()
