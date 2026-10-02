add_library(tm_geom STATIC shape.cpp)
add_library(tm::geom ALIAS tm_geom)
target_link_libraries(tm_geom PUBLIC tm::core)
tm_set_warnings(tm_geom)
