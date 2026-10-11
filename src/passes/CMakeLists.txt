find_package(nlohmann_json 3 REQUIRED)
# After-route passes (doc 05 §39): power track widening, the style metric, stitching vias.
add_library(tm_passes STATIC style.cpp widen.cpp stitch.cpp)
add_library(tm::passes ALIAS tm_passes)
target_link_libraries(tm_passes PUBLIC tm::route tm::drc tm::model tm::geom nlohmann_json::nlohmann_json)
tm_set_warnings(tm_passes)
