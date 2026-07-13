add_library(cind_document STATIC
  document/document.cpp
  document/line_index.cpp
)
target_include_directories(cind_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cind_document PUBLIC cxx_std_23)

add_library(cind_lexer STATIC
  cpp_lexer/lexer.cpp
)
target_link_libraries(cind_lexer PUBLIC cind_document)

add_executable(indent-core
  cli/main.cpp
)
target_link_libraries(indent-core PRIVATE cind_lexer)
