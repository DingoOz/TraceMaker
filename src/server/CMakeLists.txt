find_package(nlohmann_json 3 REQUIRED)

# Message builders for the viewer protocol: no networking, so the router can depend on them alone.
add_library(tm_viz_messages STATIC messages.cpp)
add_library(tm::viz ALIAS tm_viz_messages)
target_link_libraries(tm_viz_messages PUBLIC tm::model tm::drc PRIVATE nlohmann_json::nlohmann_json)
tm_set_warnings(tm_viz_messages)

# HTTP + WebSocket server (Boost.Beast) serving the viewer and streaming messages.
find_package(Boost 1.81 CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_library(tm_server STATIC viewer_server.cpp)
add_library(tm::server ALIAS tm_server)
target_link_libraries(tm_server PUBLIC tm::core tm::viz PRIVATE Boost::headers Threads::Threads nlohmann_json::nlohmann_json)
target_compile_definitions(tm_server PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}" BOOST_ASIO_NO_DEPRECATED)
tm_set_warnings(tm_server)

find_package(CLI11 CONFIG REQUIRED)
add_executable(tracemaker-view view_main.cpp)
target_link_libraries(tracemaker-view PRIVATE tm::server tm::viz tm::io tm::drc CLI11::CLI11)
tm_set_warnings(tracemaker-view)

if(TM_BUILD_TESTS)
  find_package(Catch2 3 REQUIRED)
  include(Catch)
  add_executable(tm_server_tests test_server.cpp)
  target_link_libraries(tm_server_tests PRIVATE tm::server tm::viz tm::io Boost::headers Threads::Threads
                                                nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
  target_compile_definitions(tm_server_tests PRIVATE TM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  tm_set_warnings(tm_server_tests)
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-results)
  catch_discover_tests(tm_server_tests REPORTER junit OUTPUT_DIR ${CMAKE_BINARY_DIR}/test-results OUTPUT_SUFFIX .xml)
  # The top-level enable_testing() runs after src/ is added, so ctest never reads this directory's test file.
  # Register the discovered tests with the top-level directory instead (without editing the top-level CMake).
  get_property(tm_server_test_includes DIRECTORY PROPERTY TEST_INCLUDE_FILES)
  set_property(DIRECTORY ${PROJECT_SOURCE_DIR} APPEND PROPERTY TEST_INCLUDE_FILES ${tm_server_test_includes})
endif()
