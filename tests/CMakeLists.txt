add_executable(async_runtime_tests async/runtime_tests.cpp)
target_link_libraries(async_runtime_tests PRIVATE cind_async doctest::doctest)
add_test(NAME async_runtime_tests COMMAND async_runtime_tests)

add_executable(document_tests document/document_tests.cpp)
target_link_libraries(document_tests PRIVATE cind_document doctest::doctest)
add_test(NAME document_tests COMMAND document_tests)

add_executable(text_tests document/text_tests.cpp)
target_link_libraries(text_tests PRIVATE cind_document doctest::doctest)
add_test(NAME text_tests COMMAND text_tests)

add_executable(application_tests editor/application_tests.cpp)
target_link_libraries(application_tests PRIVATE cind_application doctest::doctest)
add_test(NAME application_tests COMMAND application_tests)

add_executable(editor_application_tests editor/editor_application_tests.cpp)
target_link_libraries(editor_application_tests PRIVATE cind_editor doctest::doctest)
add_test(NAME editor_application_tests COMMAND editor_application_tests)

add_executable(terminal_tests tui/terminal_tests.cpp)
target_link_libraries(terminal_tests PRIVATE cind_tui_terminal doctest::doctest)
add_test(NAME terminal_tests COMMAND terminal_tests)

add_executable(reparse_tests syntax/reparse_tests.cpp)
target_link_libraries(reparse_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME reparse_tests COMMAND reparse_tests)

add_executable(green_tree_tests syntax/green_tree_tests.cpp)
target_link_libraries(green_tree_tests PRIVATE cind_syntax doctest::doctest)
add_test(NAME green_tree_tests COMMAND green_tree_tests)

add_executable(syntax_concurrency_tests syntax/concurrency_tests.cpp)
target_link_libraries(syntax_concurrency_tests PRIVATE cind_syntax doctest::doctest Threads::Threads)
add_test(NAME syntax_concurrency_tests COMMAND syntax_concurrency_tests)

add_executable(token_buffer_tests lexer/token_buffer_tests.cpp)
target_link_libraries(token_buffer_tests PRIVATE cind_lexer doctest::doctest)
add_test(NAME token_buffer_tests COMMAND token_buffer_tests)

add_executable(ui_tests ui/ui_tests.cpp)
target_link_libraries(ui_tests PRIVATE cind_ui doctest::doctest)
add_test(NAME ui_tests COMMAND ui_tests)

add_executable(scene_damage_tests ui/scene_damage_tests.cpp)
target_link_libraries(scene_damage_tests PRIVATE cind_ui doctest::doctest)
add_test(NAME scene_damage_tests COMMAND scene_damage_tests)

if (CIND_BUILD_GUI)
  add_executable(editor_model_tests gui/editor_model_tests.cpp)
  target_link_libraries(editor_model_tests PRIVATE
    cind_gui_model
    cind_gui_presentation
    doctest::doctest
  )
  add_test(NAME editor_model_tests COMMAND editor_model_tests)

  add_executable(inspection_tests ui/inspection_tests.cpp)
  target_link_libraries(inspection_tests PRIVATE cind_inspection doctest::doctest)
  add_test(NAME inspection_tests COMMAND inspection_tests)

  add_executable(skia_presenter_tests ui/skia_presenter_tests.cpp)
  target_link_libraries(skia_presenter_tests PRIVATE
    cind_skia_ui
    doctest::doctest
    Fontconfig::Fontconfig
    Skia::Skia
  )
  add_test(NAME skia_presenter_tests COMMAND skia_presenter_tests)

  add_executable(frame_controller_tests gui/frame_controller_tests.cpp)
  target_link_libraries(frame_controller_tests PRIVATE
    cind_gui_presentation
    doctest::doctest
    Fontconfig::Fontconfig
    Skia::Skia
  )
  add_test(NAME frame_controller_tests COMMAND frame_controller_tests)
endif()

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

add_executable(file_io_tests commands/file_io_tests.cpp)
target_link_libraries(file_io_tests PRIVATE cind_indent doctest::doctest)
add_test(NAME file_io_tests COMMAND file_io_tests)

add_test(NAME fixture_tests COMMAND indent-core test ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
