find_package(Eigen3 3.4 REQUIRED NO_MODULE)
find_package(nlohmann_json 3 REQUIRED)
find_package(Threads REQUIRED)

add_library(tm_place STATIC problem.cpp legality.cpp wirelength.cpp lower_bound.cpp global.cpp legalize.cpp congestion.cpp anneal.cpp
            tempering.cpp lns.cpp placer.cpp routable.cpp)
add_library(tm::place ALIAS tm_place)
target_link_libraries(tm_place PUBLIC tm::model tm::geom tm::core nlohmann_json::nlohmann_json PRIVATE tm::drc tm::io Eigen3::Eigen Threads::Threads)
tm_set_warnings(tm_place)

find_package(CLI11 CONFIG REQUIRED)
add_executable(tracemaker-place place_main.cpp)
target_link_libraries(tracemaker-place PRIVATE tm::place tm::io tm::route CLI11::CLI11 nlohmann_json::nlohmann_json)
tm_set_warnings(tracemaker-place)

if(TM_BUILD_TESTS)
  find_package(Catch2 3 REQUIRED)
  include(Catch)
  add_executable(tm_place_tests test_place.cpp)
  target_link_libraries(tm_place_tests PRIVATE tm::place tm::io Catch2::Catch2WithMain)
  target_compile_definitions(tm_place_tests PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  tm_set_warnings(tm_place_tests)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
  catch_discover_tests(tm_place_tests REPORTER junit OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results OUTPUT_SUFFIX .xml)
  # Register the discovered tests with the top-level directory (enable_testing() runs after src/ is added),
  # as src/server does.
  get_property(tm_place_test_includes DIRECTORY PROPERTY TEST_INCLUDE_FILES)
  set_property(DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY TEST_INCLUDE_FILES ${tm_place_test_includes})
endif()
