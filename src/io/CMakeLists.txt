find_package(nlohmann_json 3 REQUIRED)
add_library(tm_io STATIC kicad/board_reader.cpp kicad/project_reader.cpp kicad/board_editor.cpp kicad/netlist_reader.cpp
            srj/srj_reader.cpp srj/srj_to_kicad.cpp srj/srj_export.cpp)
add_library(tm::io ALIAS tm_io)
# tm::drc: pad shapes for the SRJ export (drc does not depend on io).
target_link_libraries(tm_io PUBLIC tm::model tm::sexpr PRIVATE tm::drc tm::geom nlohmann_json::nlohmann_json)
tm_set_warnings(tm_io)
