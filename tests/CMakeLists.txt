add_executable(document_tests document/document_tests.cpp)
target_link_libraries(document_tests PRIVATE cind_document doctest::doctest)
add_test(NAME document_tests COMMAND document_tests)
