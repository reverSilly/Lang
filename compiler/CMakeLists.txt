cmake_minimum_required(VERSION 3.20)
project(LangCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
)
target_compile_options(lang_core PRIVATE -Wall -Wextra)

add_executable(lang main.cpp)
target_link_libraries(lang PRIVATE lang_core)
target_compile_options(lang PRIVATE -Wall -Wextra)

enable_testing()

foreach(test_name IN ITEMS test_step1 test_step2 test_step3 test_capabilities)
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
