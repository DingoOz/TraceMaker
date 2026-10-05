find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
# One `route` run as a library function: shared by the CLI and the Python bindings (bindings/).
add_library(tm_app_route STATIC route_job.cpp)
add_library(tm::app_route ALIAS tm_app_route)
target_link_libraries(tm_app_route PUBLIC tm::route nlohmann_json::nlohmann_json PRIVATE tm::crules tm::core tm::gpu tm::io tm::learn tm::server tm::viz)
tm_set_warnings(tm_app_route)

add_executable(tracemaker main.cpp inspect.cpp defects.cpp)
target_link_libraries(tracemaker PRIVATE tm::app_route tm::crules tm::core tm::gpu tm::io tm::drc tm::route tm::learn tm::server tm::viz CLI11::CLI11 nlohmann_json::nlohmann_json)
tm_set_warnings(tracemaker)
