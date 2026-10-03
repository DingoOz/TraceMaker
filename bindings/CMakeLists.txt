# Python module `tracemaker` (roadmap M11). Built into ${CMAKE_BINARY_DIR}/python so that
# `PYTHONPATH=build/release/python python3 -c "import tracemaker"` works without installing anything.
pybind11_add_module(tm_python MODULE tracemaker_module.cpp)
set_target_properties(tm_python PROPERTIES OUTPUT_NAME tracemaker LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python)
target_link_libraries(tm_python PRIVATE tm::app_route tm::core tm::io tm::drc tm::learn)
tm_set_warnings(tm_python)
