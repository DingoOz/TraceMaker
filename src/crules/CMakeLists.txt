find_package(nlohmann_json 3 REQUIRED)

# The catalogue JSON (generated from docs/component_rules.yaml by scripts/crules_catalogue.py) is compiled in.
set(TM_CRULES_JSON ${CMAKE_CURRENT_SOURCE_DIR}/catalogue.json)
set(TM_CRULES_DATA ${CMAKE_CURRENT_BINARY_DIR}/catalogue_data.cpp)
add_custom_command(OUTPUT ${TM_CRULES_DATA}
                   COMMAND ${CMAKE_COMMAND} -DIN=${TM_CRULES_JSON} -DOUT=${TM_CRULES_DATA} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
                   DEPENDS ${TM_CRULES_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
                   COMMENT "Embedding the component-rule catalogue")

add_library(tm_crules STATIC catalogue.cpp names.cpp topology.cpp overrides.cpp detect.cpp proximity.cpp evaluate.cpp compile.cpp report.cpp impedance.cpp impedance_rules.cpp ${TM_CRULES_DATA})
add_library(tm::crules ALIAS tm_crules)
target_include_directories(tm_crules PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tm_crules PUBLIC tm::model tm::geom tm::core nlohmann_json::nlohmann_json)
tm_set_warnings(tm_crules)

if(TM_BUILD_TESTS)
  find_package(Catch2 3 REQUIRED)
  include(Catch)
  add_executable(tm_crules_tests test_crules.cpp test_impedance.cpp)
  target_link_libraries(tm_crules_tests PRIVATE tm::crules tm::io Catch2::Catch2WithMain)
  target_compile_definitions(tm_crules_tests PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  tm_set_warnings(tm_crules_tests)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
  catch_discover_tests(tm_crules_tests REPORTER junit OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results OUTPUT_SUFFIX .xml)
  get_property(tm_crules_test_includes DIRECTORY PROPERTY TEST_INCLUDE_FILES)
  set_property(DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY TEST_INCLUDE_FILES ${tm_crules_test_includes})
endif()
