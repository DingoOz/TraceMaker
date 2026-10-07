cmake_minimum_required(VERSION 3.28)

# Compilers. The presets do not pin them, so another machine can choose its own:
#   C++:        -DCMAKE_CXX_COMPILER=..., else $CXX, else the newest of /usr/bin/g++-15, -14, -13, else CMake's default.
#   CUDA host:  -DCMAKE_CUDA_HOST_COMPILER=..., else $TM_CUDA_HOST_CXX, else /usr/bin/g++-13 or g++-12.
# The system compilers are preferred over whatever `g++` is first on PATH: on the development machine that is a Nix
# toolchain whose linker cannot see the system CUDA libraries. All of this must happen before project().
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  foreach(tm_cxx /usr/bin/g++-15 /usr/bin/g++-14 /usr/bin/g++-13)
    if(EXISTS ${tm_cxx})
      set(CMAKE_CXX_COMPILER ${tm_cxx} CACHE FILEPATH "C++ compiler")
      break()
    endif()
  endforeach()
endif()
# nvcc 12.4 rejects GCC > 13 as host compiler (verified 2026-10-02).
if(NOT DEFINED CMAKE_CUDA_HOST_COMPILER)
  if(DEFINED ENV{TM_CUDA_HOST_CXX})
    set(CMAKE_CUDA_HOST_COMPILER $ENV{TM_CUDA_HOST_CXX} CACHE FILEPATH "CUDA host compiler")
  else()
    foreach(tm_cxx /usr/bin/g++-13 /usr/bin/g++-12)
      if(EXISTS ${tm_cxx})
        set(CMAKE_CUDA_HOST_COMPILER ${tm_cxx} CACHE FILEPATH "CUDA host compiler")
        break()
      endif()
    endforeach()
  endif()
endif()
# ccache when it is installed (it used to be required by the presets).
find_program(TM_CCACHE ccache)
if(TM_CCACHE)
  if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${TM_CCACHE} CACHE STRING "C++ compiler launcher")
  endif()
  if(NOT DEFINED CMAKE_CUDA_COMPILER_LAUNCHER)
    set(CMAKE_CUDA_COMPILER_LAUNCHER ${TM_CCACHE} CACHE STRING "CUDA compiler launcher")
  endif()
endif()
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 60 70 CACHE STRING "Tesla P100 (sm_60) and V100 (sm_70)")
endif()

project(TraceMaker VERSION 0.8.0 DESCRIPTION "Placement-aware PCB autorouter for KiCad" LANGUAGES CXX)

if(APPLE)
  option(TM_ENABLE_CUDA "Build CUDA kernels (CPU reference paths are always built)" OFF)
  option(TM_ENABLE_METAL "Build Metal kernels (CPU reference paths are always built)" ON)
else()
  option(TM_ENABLE_CUDA "Build CUDA kernels (CPU reference paths are always built)" ON)
  option(TM_ENABLE_METAL "Build Metal kernels (CPU reference paths are always built)" OFF)
endif()
if(TM_ENABLE_CUDA AND TM_ENABLE_METAL)
  message(FATAL_ERROR "TM_ENABLE_CUDA and TM_ENABLE_METAL are mutually exclusive")
endif()
if(TM_ENABLE_METAL AND NOT APPLE)
  message(FATAL_ERROR "TM_ENABLE_METAL requires Apple platforms")
endif()
option(TM_BUILD_TESTS "Build unit tests" ON)
option(TM_WERROR "Treat warnings as errors" ON)
option(TM_BUILD_PYTHON "Build the Python module `tracemaker` (needs pybind11; skipped with a message if missing)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # no C++20 modules; skip dependency scanning

if(TM_ENABLE_CUDA)
  enable_language(CUDA)
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  find_package(CUDAToolkit REQUIRED)
  # The CUDA host compiler is GCC 13 but C++ code and system libraries (Catch2 etc.) use GCC 15's
  # libstdc++. Keep GCC 13's library directory off the link line so the newer, backward-compatible
  # libstdc++ is used.
  foreach(var CMAKE_CUDA_HOST_IMPLICIT_LINK_DIRECTORIES CMAKE_CUDA_IMPLICIT_LINK_DIRECTORIES)
    list(FILTER ${var} EXCLUDE REGEX "/gcc/[^/]+/[0-9]+/?$")
  endforeach()
endif()
if(TM_ENABLE_METAL)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

# ccache for Objective-C++ too (C++ and CUDA are handled before project()).
if(TM_ENABLE_METAL AND TM_CCACHE AND NOT DEFINED CMAKE_OBJCXX_COMPILER_LAUNCHER)
  set(CMAKE_OBJCXX_COMPILER_LAUNCHER ${TM_CCACHE})
endif()

include(cmake/Warnings.cmake)

if(TM_BUILD_PYTHON AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
  # A sanitizer-instrumented extension cannot be loaded into an uninstrumented interpreter.
  message(STATUS "Sanitizer build: Python bindings disabled")
  set(TM_BUILD_PYTHON OFF)
endif()
if(TM_BUILD_PYTHON)
  # FindPython (not the deprecated FindPythonLibs); the presets put /usr/bin first, so the system interpreter
  # (whose headers are installed) is chosen over any Nix python on PATH.
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 CONFIG QUIET)
  if(pybind11_FOUND)
    # The engine's static libraries are linked into the Python extension, so they must be position independent.
    # -fno-semantic-interposition keeps GCC inlining across global functions as it does without -fPIC.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-semantic-interposition>)
    endif()
  else()
    message(STATUS "pybind11 not found: Python bindings disabled")
    set(TM_BUILD_PYTHON OFF)
  endif()
endif()

add_subdirectory(src)
if(TM_BUILD_PYTHON)
  add_subdirectory(bindings)
endif()

if(TM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
