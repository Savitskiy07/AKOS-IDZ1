cmake_minimum_required(VERSION 3.16)

project(gallery16 VERSION 1.0 LANGUAGES C)

# Программа написана на языке C11 и использует последовательную
# дискретно-событийную имитацию для Linux/WSL.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(GALLERY_SOURCES
    src/main.c
    src/config.c
    src/io.c
    src/random.c
    src/simulation.c
)

add_executable(gallery ${GALLERY_SOURCES})
target_include_directories(gallery PRIVATE include)
target_compile_definitions(gallery PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(gallery PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
find_package(Python3 COMPONENTS Interpreter)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(
        NAME gallery_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
                $<TARGET_FILE:gallery>
    )
endif()

add_custom_target(gallery_test
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS gallery
    USES_TERMINAL
)
