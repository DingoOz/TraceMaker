add_library(tm_sexpr STATIC sexpr.cpp)
add_library(tm::sexpr ALIAS tm_sexpr)
target_link_libraries(tm_sexpr PUBLIC tm::core)
tm_set_warnings(tm_sexpr)
