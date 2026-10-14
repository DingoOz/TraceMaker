find_package(nlohmann_json 3 REQUIRED)

# Read-only audits of a board's copper that KiCad's DRC cannot make (issue #7): the reference return of high-speed nets.
add_library(tm_analysis STATIC return_path.cpp footprint_ref.cpp footprint_check.cpp)
add_library(tm::analysis ALIAS tm_analysis)
target_include_directories(tm_analysis PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tm_analysis PUBLIC tm::model tm::geom tm::drc tm::core tm::io nlohmann_json::nlohmann_json)
tm_set_warnings(tm_analysis)
