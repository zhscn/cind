add_executable(document_tests document/document_tests.cpp)
target_link_libraries(document_tests PRIVATE cind_document doctest::doctest)
add_test(NAME document_tests COMMAND document_tests)

add_executable(lexer_tests lexer/lexer_tests.cpp)
target_link_libraries(lexer_tests PRIVATE cind_lexer doctest::doctest)
add_test(NAME lexer_tests COMMAND lexer_tests)

add_executable(parser_tests syntax/parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME parser_tests COMMAND parser_tests)

add_executable(clang_format_style_tests formatting/clang_format_style_tests.cpp)
target_link_libraries(clang_format_style_tests PRIVATE cind_indent doctest::doctest)
add_test(NAME clang_format_style_tests COMMAND clang_format_style_tests)

add_executable(indentation_tests indentation/indentation_tests.cpp)
target_link_libraries(indentation_tests PRIVATE cind_indent doctest::doctest)
add_test(NAME indentation_tests COMMAND indentation_tests)

add_test(NAME fixture_tests COMMAND indent-core test ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
