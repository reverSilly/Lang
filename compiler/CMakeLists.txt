cmake_minimum_required(VERSION 3.23)
project(compiler)

set(CMAKE_CXX_STANDARD 23)

include_directories(G:/LLVM/include)

link_directories("G:/LLVM/lib" "G:/Projects/lib")

add_executable(compiler main.cpp Parser.cpp Expression.cpp Expression.h Environment.h)

target_link_libraries(compiler libPEGParser.a libLLVMCore.a libLLVMCodegen.a)
