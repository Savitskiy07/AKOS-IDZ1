cmake_minimum_required(VERSION 3.16)
project(hw3_gallery LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The gallery needs a Linux toolchain.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_compile_options(-Wall -Wextra -Wpedantic)

add_library(gallery_common STATIC
    src/gallery.h
    src/common.c
    src/ipc.c
    src/model.c
)
target_compile_definitions(gallery_common PUBLIC _POSIX_C_SOURCE=200809L)
target_link_libraries(gallery_common PUBLIC Threads::Threads rt)

add_executable(gallery-named src/day.c)
target_compile_definitions(gallery-named PRIVATE DAY_MODE=NAMED)
add_executable(gallery-unnamed src/day.c)
target_compile_definitions(gallery-unnamed PRIVATE DAY_MODE=UNNAMED)
add_executable(guard src/guard.c)
add_executable(visitor src/visitor.c)

foreach(program IN ITEMS gallery-named gallery-unnamed guard visitor)
    target_link_libraries(${program} PRIVATE gallery_common)
endforeach()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gallery.integration
            COMMAND "${CMAKE_COMMAND}" -E env "GALLERY_BIN_DIR=$<TARGET_FILE_DIR:gallery-named>"
                    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gallery.py")
    endif()
endif()
