cmake_minimum_required(VERSION 3.20)
project(LangCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lang main.cpp)
target_compile_options(lang PRIVATE -Wall -Wextra -Werror)

enable_testing()

foreach(test_name IN ITEMS test_step1 test_step2 test_step3 test_capabilities)
    add_executable(${test_name} tests/${test_name}.cpp)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_test(
    NAME test_layer1_e2e
    COMMAND lang ${CMAKE_CURRENT_SOURCE_DIR}/../tests/run_tests.lang
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)
