cmake_minimum_required(VERSION 3.13)
project(MyTool)

set(CMAKE_CXX_STANDARD 14)

# Auto-detect the newest installed LLVM/Clang dev package instead of
# hardcoding a version. Different machines in this cluster have ended up
# with different LLVM versions installed (14 on some, 18/19/21 on others),
# and a hardcoded path silently breaks this tool wherever that exact
# version isn't present. Override explicitly if needed:
#   cmake .. -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm-18/lib/cmake/clang
if(NOT LLVM_DIR)
    file(GLOB _llvm_cmake_dirs "/usr/lib/llvm-*/lib/cmake/llvm")
    if(_llvm_cmake_dirs)
        list(SORT _llvm_cmake_dirs)
        list(GET _llvm_cmake_dirs -1 LLVM_DIR)
    endif()
endif()
if(NOT Clang_DIR)
    file(GLOB _clang_cmake_dirs "/usr/lib/llvm-*/lib/cmake/clang")
    if(_clang_cmake_dirs)
        list(SORT _clang_cmake_dirs)
        list(GET _clang_cmake_dirs -1 Clang_DIR)
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} (LLVM_DIR=${LLVM_DIR})")

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