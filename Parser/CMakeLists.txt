cmake_minimum_required(VERSION 3.13)
project(MyTool)

set(CMAKE_CXX_STANDARD 14)

# Auto-detect an installed LLVM/Clang dev package instead of hardcoding a
# version. Different machines in this cluster have ended up with different
# LLVM versions installed (14 on some, 18/19/21 on others), and a hardcoded
# path silently breaks this tool wherever that exact version isn't present.
#
# Picking the newest version isn't enough on its own: a version can be
# present as a CMake package (llvm-N-dev installed) while its actual static
# libs are missing (e.g. pulled in only as an automatic dependency of some
# other package, not a full toolchain install) — find_package() then fails
# with a confusing "imported target references a file that does not exist"
# error deep inside ClangTargets.cmake instead of a clean "not found". So
# this walks candidates newest-first and only picks one that actually has
# libclangBasic present.
#
# Override explicitly if needed:
#   cmake .. -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DClang_DIR=/usr/lib/llvm-18/lib/cmake/clang
if(NOT LLVM_DIR OR NOT Clang_DIR)
    file(GLOB _llvm_cmake_dirs "/usr/lib/llvm-*/lib/cmake/llvm")
    if(_llvm_cmake_dirs)
        list(SORT _llvm_cmake_dirs)
        list(REVERSE _llvm_cmake_dirs)
    endif()

    foreach(_llvm_cmake_dir ${_llvm_cmake_dirs})
        string(REPLACE "/lib/cmake/llvm" "/lib" _lib_dir "${_llvm_cmake_dir}")
        string(REPLACE "/cmake/llvm" "/cmake/clang" _clang_cmake_dir "${_llvm_cmake_dir}")
        file(GLOB _clang_basic_lib "${_lib_dir}/libclangBasic.*")
        if(_clang_basic_lib AND EXISTS "${_clang_cmake_dir}")
            set(LLVM_DIR "${_llvm_cmake_dir}")
            set(Clang_DIR "${_clang_cmake_dir}")
            message(STATUS "Selected LLVM/Clang install: ${_lib_dir}")
            break()
        else()
            message(STATUS "Skipping incomplete LLVM/Clang install (no libclangBasic): ${_lib_dir}")
        endif()
    endforeach()

    if(NOT LLVM_DIR)
        message(FATAL_ERROR "No complete LLVM/Clang dev install found under /usr/lib/llvm-*. "
                             "Install one (with static libs, not just the runtime), or pass "
                             "-DLLVM_DIR=... -DClang_DIR=... explicitly.")
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