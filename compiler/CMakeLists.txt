cmake_minimum_required(VERSION 3.20)
project(LangCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate system libffi (MIT License; see THIRD_PARTY_LICENSES.md)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBFFI QUIET IMPORTED_TARGET libffi)
endif()

if(NOT LIBFFI_FOUND)
    find_path(LIBFFI_INCLUDE_DIR NAMES ffi.h PATH_SUFFIXES ffi)
    find_library(LIBFFI_LIBRARY NAMES ffi libffi)
    if(LIBFFI_INCLUDE_DIR AND LIBFFI_LIBRARY)
        add_library(PkgConfig::LIBFFI INTERFACE IMPORTED)
        target_include_directories(PkgConfig::LIBFFI INTERFACE ${LIBFFI_INCLUDE_DIR})
        target_link_libraries(PkgConfig::LIBFFI INTERFACE ${LIBFFI_LIBRARY})
        set(LIBFFI_FOUND TRUE)
    endif()
endif()

if(NOT LIBFFI_FOUND)
    message(FATAL_ERROR
        "libffi not found. Please install libffi development headers "
        "(e.g., `apt install libffi-dev`, `pacman -S libffi`, or `brew install libffi`).")
endif()

add_library(lang_core STATIC
    runtime/FieldMap.cpp
    runtime/Shape.cpp
    runtime/FrameStack.cpp
    runtime/Prototype.cpp
    runtime/Runtime.cpp
    parser/Peg.cpp
    parser/Layer0.cpp
    eval/Evaluator.cpp
    eval/Capabilities.cpp
    eval/RuntimeCaps.cpp
    eval/FfiCaps.cpp
    eval/CompilerCaps.cpp
)
target_link_libraries(lang_core PUBLIC PkgConfig::LIBFFI ${CMAKE_DL_LIBS})
target_compile_options(lang_core PRIVATE -Wall -Wextra)

add_executable(lang main.cpp)
target_link_libraries(lang PRIVATE lang_core)
target_compile_options(lang PRIVATE -Wall -Wextra)

enable_testing()

foreach(test_name IN ITEMS test_step1 test_step2 test_step3 test_capabilities test_ffi)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE lang_core)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_test(
    NAME test_layer1_e2e
    COMMAND lang ${CMAKE_CURRENT_SOURCE_DIR}/../tests/run_tests.lang
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)
