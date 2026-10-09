# Clipper2 (polygon booleans and offsets for zone fills, doc 05 §36): the installed package if there is one
# (Homebrew `clipper2`), else the pinned release's sources compiled here. Its own CMake project is not used: it
# needs a C compiler, which C++-only toolchains (the CI container) do not have.
find_package(Clipper2 CONFIG QUIET)
if(NOT Clipper2_FOUND)
  include(FetchContent)
  FetchContent_Declare(Clipper2 GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git GIT_TAG Clipper2_2.0.1
                       GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
  FetchContent_MakeAvailable(Clipper2)
  file(GLOB clipper2_sources ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/*.cpp)
  add_library(tm_clipper2 STATIC ${clipper2_sources})
  target_include_directories(tm_clipper2 SYSTEM PUBLIC ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/include)
  target_compile_features(tm_clipper2 PUBLIC cxx_std_17)
  add_library(Clipper2::Clipper2 ALIAS tm_clipper2)
endif()

add_library(tm_geom STATIC shape.cpp poly_index.cpp clip.cpp)
add_library(tm::geom ALIAS tm_geom)
target_link_libraries(tm_geom PUBLIC tm::core PRIVATE Clipper2::Clipper2)
tm_set_warnings(tm_geom)
