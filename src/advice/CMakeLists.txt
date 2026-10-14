find_package(nlohmann_json 3 REQUIRED)

# The advice file, the stuck report and the advisor protocol (doc 17, issue #26). The engine reads advice like any other
# input; no model, network or credentials are anywhere in this library.
add_library(tm_advice STATIC advice.cpp stuck_report.cpp advisor.cpp)
add_library(tm::advice ALIAS tm_advice)
target_include_directories(tm_advice PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tm_advice PUBLIC tm::route tm::model tm::geom tm::core nlohmann_json::nlohmann_json)
tm_set_warnings(tm_advice)
