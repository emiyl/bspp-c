cmake_minimum_required(VERSION 3.20)
project(ButterscotchPreprocessorC VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PNG REQUIRED)
find_package(BZip2 REQUIRED)
option(ENABLE_PARALLEL "Enable parallel image and audio processing (requires pthreads)" ON)
if(ENABLE_PARALLEL)
  set(THREADS_PREFER_PTHREAD_FLAG TRUE)
  find_package(Threads REQUIRED)
  if(NOT CMAKE_USE_PTHREADS_INIT)
    message(FATAL_ERROR "Parallel processing requires pthreads; configure with -DENABLE_PARALLEL=OFF for a sequential build")
  endif()
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

option(ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_subdirectory(lib)
add_subdirectory(cli)
