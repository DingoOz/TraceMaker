# Clipper2 (polygon booleans and offsets for zone fills, doc 05 §36): the installed package if there is one
# (Homebrew `clipper2`), else the pinned release built with the project.
find_package(Clipper2 CONFIG QUIET)
if(NOT Clipper2_FOUND)
  include(FetchContent)
  set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
  set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
  set(CLIPPER2_USINGZ OFF CACHE STRING "" FORCE)
  FetchContent_Declare(Clipper2 GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git GIT_TAG Clipper2_2.0.1
                       GIT_SHALLOW TRUE SOURCE_SUBDIR CPP SYSTEM)
  FetchContent_MakeAvailable(Clipper2)
  if(NOT TARGET Clipper2::Clipper2)
    add_library(Clipper2::Clipper2 ALIAS Clipper2)
  endif()
endif()

add_library(tm_geom STATIC shape.cpp poly_index.cpp clip.cpp)
add_library(tm::geom ALIAS tm_geom)
target_link_libraries(tm_geom PUBLIC tm::core PRIVATE Clipper2::Clipper2)
tm_set_warnings(tm_geom)
