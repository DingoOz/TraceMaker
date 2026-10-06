find_package(Catch2 3 REQUIRED)
include(Catch)

find_package(nlohmann_json 3 REQUIRED)
add_executable(tm_tests test_units.cpp test_rng.cpp test_gpu.cpp test_sexpr.cpp test_kicad_io.cpp test_rules.cpp test_netlist.cpp test_geom.cpp test_escape.cpp test_diff_pair.cpp)
target_link_libraries(tm_tests PRIVATE tm::core tm::gpu tm::sexpr tm::model tm::io tm::geom tm::route nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
target_compile_definitions(tm_tests PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
tm_set_warnings(tm_tests)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
catch_discover_tests(tm_tests REPORTER junit OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results OUTPUT_SUFFIX .xml)

# Integration: edited boards must read identically in KiCad (Docker) and TraceMaker.
add_test(NAME kicad_edit_roundtrip COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/edit_roundtrip.sh $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration)
set_tests_properties(kicad_edit_roundtrip PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 600)
add_test(NAME kicad_drc_parity COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/drc_parity.sh)
set_tests_properties(kicad_drc_parity PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 1200)
add_test(NAME kicad_drc_broken_parity COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/drc_broken_parity.sh)
set_tests_properties(kicad_drc_broken_parity PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 1800)
add_test(NAME drc_zone_index COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/drc_zone_index.sh $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/drc_zone_index)
set_tests_properties(drc_zone_index PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration" TIMEOUT 600)
add_test(NAME global_route COMMAND ${PROJECT_SOURCE_DIR}/tests/integration/global_route.sh $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/global_route)
set_tests_properties(global_route PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration;route" TIMEOUT 600)
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

# Component-rule catalogue: the committed JSON matches docs/component_rules.yaml and the rule ids in doc 15 (doc 15 §9.1 L0).
add_test(NAME crules_catalogue_sync COMMAND python3 ${PROJECT_SOURCE_DIR}/scripts/crules_catalogue.py --check)
set_tests_properties(crules_catalogue_sync PROPERTIES SKIP_RETURN_CODE 77 LABELS "crules" TIMEOUT 60)
# Component rules in routing: generated keep-outs honoured, never written into the output, sidecar .kicad_dru written.
add_test(NAME crules_route COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/crules_route.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/crules)
set_tests_properties(crules_route PROPERTIES SKIP_RETURN_CODE 77 LABELS "crules;integration" TIMEOUT 600)
add_test(NAME crules_dru_parses COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/crules_dru_parses.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/crules_dru)
set_tests_properties(crules_dru_parses PROPERTIES SKIP_RETURN_CODE 77 LABELS "kicad;integration" TIMEOUT 900)
add_test(NAME crules_two_stage COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/crules_two_stage.py $<TARGET_FILE:tracemaker-place> ${CMAKE_BINARY_DIR}/integration/crules_two_stage)
set_tests_properties(crules_two_stage PROPERTIES SKIP_RETURN_CODE 77 LABELS "integration" TIMEOUT 900)
# Reachability pre-check: every "unreachable" verdict re-checked by the full A* (skips without fixtures).
add_test(NAME reach_verify COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/reach_verify.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/reach_verify)
set_tests_properties(reach_verify PROPERTIES SKIP_RETURN_CODE 77 LABELS "route;integration" TIMEOUT 600)
# Determinism across thread counts (N3, D47): the same --work portfolio at 1, 2 and 4 threads gives identical bytes.
add_test(NAME thread_determinism COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/thread_determinism.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/thread_determinism)
set_tests_properties(thread_determinism PROPERTIES SKIP_RETURN_CODE 77 LABELS "route;integration" TIMEOUT 900 PROCESSORS 4)
# Coupled differential pairs (M12, D50): a USB hub's five pairs routed coupled, deterministic, KiCad-clean.
add_test(NAME diff_pair COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/integration/diff_pair.py $<TARGET_FILE:tracemaker> ${CMAKE_BINARY_DIR}/integration/diff_pair)
set_tests_properties(diff_pair PROPERTIES SKIP_RETURN_CODE 77 LABELS "route;kicad;integration" TIMEOUT 900)
