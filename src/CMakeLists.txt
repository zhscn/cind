add_library(cind_document STATIC
  document/document.cpp
  document/line_index.cpp
  document/text.cpp
)
target_include_directories(cind_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cind_document PUBLIC cxx_std_23)

add_library(cind_lexer STATIC
  cpp_lexer/lexer.cpp
  cpp_lexer/token_buffer.cpp
)
target_link_libraries(cind_lexer PUBLIC cind_document)

add_library(cind_syntax STATIC
  syntax/parser.cpp
  syntax/syntax_tree.cpp
  syntax/green_node.cpp
  syntax/structure.cpp
  syntax/analysis.cpp
)
target_link_libraries(cind_syntax PUBLIC cind_lexer)

add_library(cind_indent STATIC
  indentation/indentation_service.cpp
  indentation/expression_continuation.cpp
  commands/editor_commands.cpp
  formatting/clang_format_style.cpp
)
target_link_libraries(cind_indent PUBLIC cind_syntax)

add_executable(indent-core
  cli/main.cpp
  cli/bench.cpp
  cli/session.cpp
  cli/style_loader.cpp
  cli/fixture_runner.cpp
  cli/repl.cpp
  tui/terminal.cpp
  tui/editor.cpp
)
target_link_libraries(indent-core PRIVATE cind_indent)
