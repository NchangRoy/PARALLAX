cmake_minimum_required(VERSION 3.13)
project(MyTool)

set(CMAKE_CXX_STANDARD 14)

# FORCE LLVM 14
set(LLVM_DIR "/usr/lib/llvm-14/lib/cmake/llvm")
set(Clang_DIR "/usr/lib/llvm-14/lib/cmake/clang")

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "LLVM: ${LLVM_PACKAGE_VERSION}")

include_directories(SYSTEM
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

add_definitions(${LLVM_DEFINITIONS})

add_executable(mytool parser.cpp)

target_link_libraries(mytool
    clangTooling
    clangFrontend
    clangBasic
    clangASTMatchers
    clangAST
    clangRewrite
    clangLex
    clangSema
    LLVM
)