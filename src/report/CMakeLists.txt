find_package(nlohmann_json 3 REQUIRED)

# The run report (doc 18 D3, D136): its own sections of the route summary, and the renderer that writes the report
# file from the summary alone.
add_library(tm_report STATIC run_report.cpp render.cpp)
add_library(tm::report ALIAS tm_report)
target_include_directories(tm_report PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tm_report PUBLIC tm::model tm::core nlohmann_json::nlohmann_json PRIVATE tm::drc)
tm_set_warnings(tm_report)
