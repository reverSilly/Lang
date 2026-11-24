cmake_minimum_required(VERSION 3.20)
project(LangCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

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

# Locate cereal (BSD-3-Clause; see THIRD_PARTY_LICENSES.md), with FetchContent fallback
find_package(cereal QUIET)
if(NOT cereal_FOUND)
    find_path(CEREAL_INCLUDE_DIR NAMES cereal/cereal.hpp)
    if(CEREAL_INCLUDE_DIR)
        add_library(cereal::cereal INTERFACE IMPORTED)
        target_include_directories(cereal::cereal SYSTEM INTERFACE ${CEREAL_INCLUDE_DIR})
    else()
        set(JUST_INSTALL_CEREAL ON CACHE BOOL "" FORCE)
        set(SKIP_PORTABILITY_TEST ON CACHE BOOL "" FORCE)
        set(BUILD_DOC OFF CACHE BOOL "" FORCE)
        set(BUILD_SANDBOX OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            cereal
            GIT_REPOSITORY https://github.com/USCiLab/cereal.git
            GIT_TAG v1.3.2
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(cereal)
        if(TARGET cereal::cereal)
            get_target_property(_cereal_inc cereal::cereal INTERFACE_INCLUDE_DIRECTORIES)
            if(_cereal_inc)
                set_target_properties(cereal::cereal PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cereal_inc}")
            endif()
        endif()
    endif()
endif()

# Locate Boost.PFR (BSL-1.0; see THIRD_PARTY_LICENSES.md), with FetchContent fallback
find_path(BOOST_PFR_INCLUDE_DIR NAMES boost/pfr.hpp)
if(BOOST_PFR_INCLUDE_DIR)
    add_library(boost_pfr INTERFACE IMPORTED)
    target_include_directories(boost_pfr SYSTEM INTERFACE ${BOOST_PFR_INCLUDE_DIR})
else()
    FetchContent_Declare(
        boost_pfr
        GIT_REPOSITORY https://github.com/boostorg/pfr.git
        GIT_TAG 2.2.0
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(boost_pfr)
    if(NOT boost_pfr_POPULATED)
        FetchContent_Populate(boost_pfr)
    endif()
    add_library(boost_pfr INTERFACE)
    target_include_directories(boost_pfr SYSTEM INTERFACE ${boost_pfr_SOURCE_DIR}/include)
endif()

add_library(lang_core STATIC
    ast/AstCodec.cpp
    runtime/FieldMap.cpp
    runtime/Shape.cpp
    runtime/FrameStack.cpp
    runtime/Prototype.cpp
    runtime/FfiDispatch.cpp
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
target_link_libraries(lang_core PRIVATE cereal::cereal boost_pfr)
target_compile_options(lang_core PRIVATE -Wall -Wextra)

add_executable(lang main.cpp)
target_link_libraries(lang PRIVATE lang_core)
target_compile_options(lang PRIVATE -Wall -Wextra)

enable_testing()

foreach(test_name IN ITEMS test_step1 test_step2 test_step3 test_capabilities test_ffi test_ast_codec)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE lang_core)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -UNDEBUG)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_test(
    NAME test_layer1_e2e
    COMMAND lang ${CMAKE_CURRENT_SOURCE_DIR}/../tests/run_tests.lang
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(
    NAME test_run_layer1
    COMMAND lang ${CMAKE_CURRENT_SOURCE_DIR}/../lib/std/run.lang ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_layer1.lang
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)
