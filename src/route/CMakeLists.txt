add_library(tm_route STATIC obstacles.cpp router.cpp)
add_library(tm::route ALIAS tm_route)
target_link_libraries(tm_route PUBLIC tm::drc tm::model tm::geom)
tm_set_warnings(tm_route)
