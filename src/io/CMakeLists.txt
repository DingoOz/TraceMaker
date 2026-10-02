add_library(tm_io STATIC kicad/board_reader.cpp)
add_library(tm::io ALIAS tm_io)
target_link_libraries(tm_io PUBLIC tm::model tm::sexpr)
tm_set_warnings(tm_io)
