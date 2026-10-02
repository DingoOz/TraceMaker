find_package(CLI11 CONFIG REQUIRED)
add_executable(tracemaker main.cpp)
target_link_libraries(tracemaker PRIVATE tm::core tm::gpu CLI11::CLI11)
tm_set_warnings(tracemaker)
