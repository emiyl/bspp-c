add_executable(bspp-cli preprocessor.c)

set_target_properties(bspp-cli PROPERTIES OUTPUT_NAME "bspp")

target_include_directories(bspp-cli PRIVATE ${PROJECT_SOURCE_DIR}/lib)
target_compile_definitions(bspp-cli PRIVATE _POSIX_C_SOURCE=200809L)
if(APPLE)
  target_compile_definitions(bspp-cli PRIVATE _DARWIN_C_SOURCE)
endif()
target_compile_options(bspp-cli PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(bspp-cli PRIVATE bspp)
