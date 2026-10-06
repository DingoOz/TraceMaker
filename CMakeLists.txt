cmake_minimum_required(VERSION 3.28)

project(TraceMaker VERSION 0.1.0 DESCRIPTION "Placement-aware PCB autorouter for KiCad" LANGUAGES CXX)

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
  # Set CUDA defaults before enabling the language; nvcc 12.4 accepts GCC <= 13.
  if(NOT DEFINED CMAKE_CUDA_HOST_COMPILER AND EXISTS /usr/bin/g++-13)
    set(CMAKE_CUDA_HOST_COMPILER /usr/bin/g++-13 CACHE FILEPATH "CUDA host compiler")
  endif()
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 60 70 CACHE STRING "Tesla P100 (sm_60) and V100 (sm_70)")
  endif()
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

# ccache is optional; keep user-supplied compiler launchers.
find_program(TM_CCACHE_EXECUTABLE ccache)
if(TM_CCACHE_EXECUTABLE)
  get_property(tm_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
  foreach(language IN LISTS tm_languages)
    if(NOT DEFINED CMAKE_${language}_COMPILER_LAUNCHER)
      set(CMAKE_${language}_COMPILER_LAUNCHER "${TM_CCACHE_EXECUTABLE}")
    endif()
  endforeach()
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
