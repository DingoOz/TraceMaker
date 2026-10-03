find_package(Catch2 3 REQUIRED)
include(Catch)

find_package(nlohmann_json 3 REQUIRED)
add_executable(tm_tests test_units.cpp test_rng.cpp test_gpu.cpp test_sexpr.cpp test_kicad_io.cpp test_rules.cpp test_netlist.cpp test_geom.cpp)
target_link_libraries(tm_tests PRIVATE tm::core tm::gpu tm::sexpr tm::model tm::io tm::geom nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
target_compile_definitions(tm_tests PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
tm_set_warnings(tm_tests)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
catch_discover_tests(tm_tests REPORTER junit OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results OUTPUT_SUFFIX .xml)

# Integration: edited boards must read identically in KiCad (Docker) and TraceMaker.
add_test(NAME kicad_edit_roundtrip COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/edit_roundtrip.sh $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration)
set_tests_properties(kicad_edit_roundtrip PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 600)
add_test(NAME kicad_drc_parity COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/drc_parity.sh)
set_tests_properties(kicad_drc_parity PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 1200)
add_test(NAME record_replay COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/record_replay.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/replay)
set_tests_properties(record_replay PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration" TIMEOUT 600)

# Python bindings: read, route, DRC and emit-items through the `tracemaker` module (skips if it was not built).
if(NOT Python_EXECUTABLE)
  set(Python_EXECUTABLE python3)  # bindings disabled: the test runs only to report the skip
endif()
add_test(NAME python_bindings COMMAND ${Python_EXECUTABLE} ${PROJECT_SOURCE_DIR}/bindings/test_bindings.py ${CMAKE_BINARY_DIR}/python $<TARGET_FILE:tracemaker>)
set_tests_properties(python_bindings PROPERTIES SKIP_RETURN_CODE 77 LABELS "python;integration" TIMEOUT 600)

# KiCad PCM package: build the archive and check its layout and metadata.
add_test(NAME pcm_package COMMAND sh -c "python3 '${PROJECT_SOURCE_DIR}/scripts/make_pcm_package.py' --out '${CMAKE_BINARY_DIR}/pcm-test' && python3 '${PROJECT_SOURCE_DIR}/scripts/validate_pcm.py' '${CMAKE_BINARY_DIR}/pcm-test/tracemaker-${PROJECT_VERSION}.zip'")
set_tests_properties(pcm_package PROPERTIES LABELS "kicad" TIMEOUT 120)
add_test(NAME length_tuning COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/length_tuning.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/length)
set_tests_properties(length_tuning PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 600)
