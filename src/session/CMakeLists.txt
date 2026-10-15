find_package(nlohmann_json 3 REQUIRED)

# A board session an agent can drive, and its two transports (doc 19, M18). The engine stays offline: no model, key or network is in this library.
add_library(tm_session STATIC session.cpp transport.cpp)
add_library(tm::session ALIAS tm_session)
target_include_directories(tm_session PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tm_session PUBLIC tm::route tm::advice tm::drc tm::io tm::model tm::geom tm::core nlohmann_json::nlohmann_json)
tm_set_warnings(tm_session)
