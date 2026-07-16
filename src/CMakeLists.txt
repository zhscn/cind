add_library(cind_async STATIC
  async/runtime.cpp
)
target_include_directories(cind_async PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cind_async PRIVATE PkgConfig::Libuv Threads::Threads)

add_library(cind_project_io STATIC
  project/project_files.cpp
)
target_include_directories(cind_project_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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

add_library(cind_ui STATIC
  ui/char_width.cpp
  ui/compose_line.cpp
  ui/editor_scene.cpp
  ui/line_signs.cpp
  ui/scene_damage.cpp
  ui/scene_layout.cpp
  ui/view_tree.cpp
  ui/ansi_renderer.cpp
  ui/text_position.cpp
)
target_link_libraries(cind_ui PUBLIC cind_lexer PkgConfig::Utf8Proc)

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
  commands/file_io.cpp
  formatting/clang_format_style.cpp
)
target_link_libraries(cind_indent PUBLIC cind_syntax)

add_library(cind_application STATIC
  editor/buffer.cpp
  editor/command.cpp
  editor/command_loop.cpp
  editor/cpp_mode.cpp
  editor/default_keymap.cpp
  editor/interaction.cpp
  editor/keymap.cpp
  editor/language.cpp
  editor/mode.cpp
  editor/project.cpp
  editor/runtime.cpp
  editor/settings.cpp
  editor/text_input.cpp
  editor/view.cpp
  editor/window.cpp
)
target_link_libraries(cind_application PUBLIC cind_async cind_document PRIVATE PkgConfig::Utf8Proc)

add_library(cind_editor STATIC
  cli/session.cpp
  cli/style_loader.cpp
  editor/basic_commands.cpp
  editor/editor_application.cpp
  editor/project_service.cpp
  editor/search_commands.cpp
)
target_link_libraries(cind_editor
  PUBLIC cind_async cind_indent cind_application cind_project_io
  PRIVATE cind_ui Threads::Threads
)

add_library(cind_tui_terminal STATIC
  tui/terminal.cpp
)
target_include_directories(cind_tui_terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(indent-core
  cli/main.cpp
  cli/bench.cpp
  cli/fixture_runner.cpp
  cli/repl.cpp
  tui/editor.cpp
)
target_link_libraries(indent-core PRIVATE cind_editor cind_ui cind_tui_terminal)

if (CIND_BUILD_GUI)
  find_package(SDL3 CONFIG REQUIRED)

  add_library(cind_inspection STATIC
    gui/inspection.cpp
    gui/inspect_server.cpp
  )
  target_include_directories(cind_inspection PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(cind_inspection PUBLIC cind_ui Threads::Threads)

  add_library(cind_gui_model STATIC
    gui/editor_model.cpp
  )
  target_include_directories(cind_gui_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(cind_gui_model PUBLIC cind_editor cind_ui)

  add_library(cind_skia_ui STATIC
    gui/skia_presenter.cpp
  )
  target_include_directories(cind_skia_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(cind_skia_ui PUBLIC cind_ui PRIVATE Skia::Skia)

  add_library(cind_gui_presentation STATIC
    gui/frame_controller.cpp
    gui/motion.cpp
    gui/scroll_timeline.cpp
  )
  target_include_directories(cind_gui_presentation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(cind_gui_presentation PUBLIC cind_skia_ui)

  add_executable(cind-gui
    gui/main.cpp
  )
  target_link_libraries(cind-gui PRIVATE
    cind_gui_model
    cind_gui_presentation
    cind_inspection
    cind_skia_ui
    SDL3::SDL3
  )

  add_executable(cind-ui-inspect
    gui/inspect_cli.cpp
  )
  target_link_libraries(cind-ui-inspect PRIVATE cind_inspection)
endif()
