cmake_minimum_required(VERSION 3.13)
project(MyTool)

set(CMAKE_CXX_STANDARD 14)

# Auto-detect an installed LLVM/Clang dev package instead of hardcoding a
# version. Different machines in this cluster have ended up with different
# LLVM versions installed (14 on some, 18/19/21 on others), and a hardcoded
# path silently breaks this tool wherever that exact version isn't present.
#
# Picking the newest version isn't enough on its own: a version can be
# present as a CMake package (llvm-N-dev installed) while its actual libs
# are missing — find_package() then fails with a confusing "imported target
# references a file that does not exist" error deep inside
# ClangTargets.cmake instead of a clean "not found". So this walks
# candidates newest-first and only picks one with a usable Clang lib.
#
# Two packaging styles exist in the wild and both are accepted here:
#   - per-component static/shared libs (libclangBasic.a / .so, ...)
#   - a single combined dylib (libclang-cpp.so — Debian/Kali's apt.llvm.org
#     packages ship only this; a version can have this present while still
#     lacking every per-component .a)
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
        file(GLOB _clang_component_lib "${_lib_dir}/libclangBasic.*")
        file(GLOB _clang_dylib "${_lib_dir}/libclang-cpp.so*")
        if((_clang_component_lib OR _clang_dylib) AND EXISTS "${_clang_cmake_dir}")
            set(LLVM_DIR "${_llvm_cmake_dir}")
            set(Clang_DIR "${_clang_cmake_dir}")
            message(STATUS "Selected LLVM/Clang install: ${_lib_dir}")
            break()
        else()
            message(STATUS "Skipping incomplete LLVM/Clang install (no clang lib found): ${_lib_dir}")
        endif()
    endforeach()

    if(NOT LLVM_DIR)
        message(FATAL_ERROR "No complete LLVM/Clang dev install found under /usr/lib/llvm-*. "
                             "Install one (with libs, not just the runtime), or pass "
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

# Some distro packages (e.g. Kali/Debian's apt.llvm.org builds) only ship
# Clang as a single combined libclang-cpp.so ("dylib mode") rather than
# per-component static archives — link whichever form this install has.
if(TARGET clang-cpp)
    message(STATUS "Linking against combined clang-cpp (dylib-mode Clang install)")
    target_link_libraries(mytool clang-cpp LLVM)
else()
    message(STATUS "Linking against individual clang component libraries")
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
endif()