cmake_minimum_required(VERSION 3.13)

project(MyTool)

set(CMAKE_CXX_STANDARD 14)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

include_directories(
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

add_definitions(${LLVM_DEFINITIONS})

add_executable(mytool tool.cpp)

target_link_libraries(mytool
    clangTooling
    clangBasic
    clangASTMatchers
)