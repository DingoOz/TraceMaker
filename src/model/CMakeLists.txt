add_library(tm_model STATIC board.cpp)
add_library(tm::model ALIAS tm_model)
target_link_libraries(tm_model PUBLIC tm::core tm::sexpr)
tm_set_warnings(tm_model)
