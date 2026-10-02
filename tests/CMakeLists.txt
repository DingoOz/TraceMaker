find_package(Catch2 3 REQUIRED)
include(Catch)

find_package(nlohmann_json 3 REQUIRED)
add_executable(tm_tests test_units.cpp test_rng.cpp test_gpu.cpp test_sexpr.cpp test_kicad_io.cpp test_rules.cpp test_netlist.cpp)
target_link_libraries(tm_tests PRIVATE tm::core tm::gpu tm::sexpr tm::model tm::io nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
target_compile_definitions(tm_tests PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
tm_set_warnings(tm_tests)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
catch_discover_tests(tm_tests REPORTER junit OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results OUTPUT_SUFFIX .xml)

# Integration: edited boards must read identically in KiCad (Docker) and TraceMaker.
add_test(NAME kicad_edit_roundtrip COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/edit_roundtrip.sh $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration)
set_tests_properties(kicad_edit_roundtrip PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 600)
