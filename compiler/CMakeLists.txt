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
            get_target_property(_cereal_real cereal::cereal ALIAS_TARGET)
            if(NOT _cereal_real)
                set(_cereal_real cereal::cereal)
            endif()
            get_target_property(_cereal_inc ${_cereal_real} INTERFACE_INCLUDE_DIRECTORIES)
            if(_cereal_inc)
                set_target_properties(${_cereal_real} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cereal_inc}")
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
        SOURCE_SUBDIR _nonexistent_subdir
    )
    FetchContent_MakeAvailable(boost_pfr)
    add_library(boost_pfr INTERFACE)
    target_include_directories(boost_pfr SYSTEM INTERFACE ${boost_pfr_SOURCE_DIR}/include)
endif()

# ----------------------------------------------------------------------------
# Module 1: lang_runtime — minimal target runtime (AST codec + evaluator + FFI)
# Zero parser and zero compile-time capabilities.
# ----------------------------------------------------------------------------
add_library(lang_runtime STATIC
    ast/AstCodec.cpp
    runtime/FieldMap.cpp
    runtime/Shape.cpp
    runtime/FrameStack.cpp
    runtime/Prototype.cpp
    runtime/FfiDispatch.cpp
    runtime/Runtime.cpp
    eval/Evaluator.cpp
    eval/CliArgs.cpp
)
target_include_directories(lang_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lang_runtime PUBLIC PkgConfig::LIBFFI ${CMAKE_DL_LIBS})
target_link_libraries(lang_runtime PRIVATE cereal::cereal boost_pfr)
target_compile_options(lang_runtime PRIVATE -Wall -Wextra)

# Minimal packed-executable stub linking ONLY lang_runtime
add_executable(lang_stub stub_main.cpp)
target_link_libraries(lang_stub PRIVATE lang_runtime)
target_compile_options(lang_stub PRIVATE -Wall -Wextra)

# Build-time tool that embeds lang_stub bytes into generated/EmbeddedStub.cpp
add_executable(embed_stub tools/embed_stub.cpp)
target_compile_options(embed_stub PRIVATE -Wall -Wextra)

set(EMBEDDED_STUB_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedStub.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${EMBEDDED_STUB_CPP}"
    COMMAND embed_stub "$<TARGET_FILE:lang_stub>" "${EMBEDDED_STUB_CPP}"
    DEPENDS embed_stub lang_stub
    COMMENT "Embedding lang_stub into EmbeddedStub.cpp"
    VERBATIM
)

# ----------------------------------------------------------------------------
# Module 2: lang_compiler — parser + compile-time capabilities + embedded stub
# ----------------------------------------------------------------------------
add_library(lang_compiler STATIC
    parser/Peg.cpp
    parser/Layer0.cpp
    eval/Capabilities.cpp
    eval/RuntimeCaps.cpp
    eval/FfiCaps.cpp
    eval/CompilerCaps.cpp
    "${EMBEDDED_STUB_CPP}"
)
target_link_libraries(lang_compiler PUBLIC lang_runtime)
target_compile_options(lang_compiler PRIVATE -Wall -Wextra)

add_executable(lang main.cpp)
target_link_libraries(lang PRIVATE lang_compiler)
target_compile_options(lang PRIVATE -Wall -Wextra)

enable_testing()

add_executable(test_ast_codec tests/test_ast_codec.cpp)
target_link_libraries(test_ast_codec PRIVATE lang_runtime)
target_compile_options(test_ast_codec PRIVATE -Wall -Wextra -UNDEBUG)
add_test(NAME test_ast_codec COMMAND test_ast_codec)

foreach(test_name IN ITEMS test_step1 test_step2 test_step3 test_capabilities test_ffi)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE lang_compiler)
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
