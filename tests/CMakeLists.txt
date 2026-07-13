add_executable(document_tests document/document_tests.cpp)
target_link_libraries(document_tests PRIVATE cind_document doctest::doctest)
add_test(NAME document_tests COMMAND document_tests)

add_executable(lexer_tests lexer/lexer_tests.cpp)
target_link_libraries(lexer_tests PRIVATE cind_lexer doctest::doctest)
add_test(NAME lexer_tests COMMAND lexer_tests)

add_executable(parser_tests syntax/parser_tests.cpp)
target_link_libraries(parser_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME parser_tests COMMAND parser_tests)
