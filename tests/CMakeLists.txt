add_executable(document_tests document/document_tests.cpp)
target_link_libraries(document_tests PRIVATE cind_document doctest::doctest)
add_test(NAME document_tests COMMAND document_tests)

add_executable(text_tests document/text_tests.cpp)
target_link_libraries(text_tests PRIVATE cind_document doctest::doctest)
add_test(NAME text_tests COMMAND text_tests)

add_executable(reparse_tests syntax/reparse_tests.cpp)
target_link_libraries(reparse_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME reparse_tests COMMAND reparse_tests)

add_executable(green_tree_tests syntax/green_tree_tests.cpp)
target_link_libraries(green_tree_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME green_tree_tests COMMAND green_tree_tests)

add_executable(token_buffer_tests lexer/token_buffer_tests.cpp)
target_link_libraries(token_buffer_tests PRIVATE cind_lexer doctest::doctest)
add_test(NAME token_buffer_tests COMMAND token_buffer_tests)

add_executable(relex_tests lexer/relex_tests.cpp)
target_link_libraries(relex_tests PRIVATE cind_lexer doctest::doctest)
add_test(NAME relex_tests COMMAND relex_tests)

add_executable(lexer_tests lexer/lexer_tests.cpp)
target_link_libraries(lexer_tests PRIVATE cind_lexer doctest::doctest)
add_test(NAME lexer_tests COMMAND lexer_tests)

add_executable(parser_tests syntax/parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME parser_tests COMMAND parser_tests)

add_executable(structure_tests syntax/structure_tests.cpp)
target_link_libraries(structure_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME structure_tests COMMAND structure_tests)

add_executable(clang_format_style_tests formatting/clang_format_style_tests.cpp)
target_link_libraries(clang_format_style_tests PRIVATE cind_indent doctest::doctest)
add_test(NAME clang_format_style_tests COMMAND clang_format_style_tests)

add_executable(indentation_tests indentation/indentation_tests.cpp)
target_link_libraries(indentation_tests PRIVATE cind_indent doctest::doctest)
add_test(NAME indentation_tests COMMAND indentation_tests)

add_test(NAME fixture_tests COMMAND indent-core test ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
