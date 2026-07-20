add_library(cind_async STATIC
  async/runtime.cpp
)
target_include_directories(cind_async PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cind_async PRIVATE PkgConfig::Libuv Threads::Threads)

add_library(cind_project_io STATIC
  project/project_files.cpp
  project/search_results.cpp
)
target_include_directories(cind_project_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(cind_document STATIC
  document/document.cpp
  document/line_index.cpp
  document/text.cpp
)
target_include_directories(cind_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cind_document PUBLIC cxx_std_23)

add_library(cind_lsp STATIC
  lsp/completion.cpp
  lsp/diagnostics.cpp
  lsp/json_rpc.cpp
  lsp/navigation.cpp
  lsp/protocol.cpp
  lsp/refactor.cpp
  lsp/registry.cpp
  lsp/session.cpp
)
target_include_directories(cind_lsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cind_lsp
  PUBLIC cind_async cind_document
  PRIVATE glaze::glaze
)

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
  cli/style_loader.cpp
)
target_link_libraries(cind_indent PUBLIC cind_syntax)

add_library(cind_script_async STATIC
  script/async_host.cpp
)
target_include_directories(cind_script_async PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cind_script_async
  PUBLIC cind_async
  PRIVATE cind_indent cind_project_io
)

add_library(cind_application STATIC
  editor/buffer.cpp
  editor/command.cpp
  editor/command_loop.cpp
  editor/completion.cpp
  editor/composed_view.cpp
  editor/input_state.cpp
  editor/input_strategy.cpp
  editor/interaction.cpp
  editor/jump.cpp
  editor/location_list.cpp
  editor/transaction_group.cpp
  editor/keymap.cpp
  editor/language.cpp
  editor/language_mechanism.cpp
  editor/mode.cpp
  editor/noun.cpp
  editor/project.cpp
  editor/resource_policy.cpp
  editor/runtime.cpp
  editor/settings.cpp
  editor/view.cpp
  editor/workbench.cpp
  editor/workbench_session.cpp
  editor/workspace_edit.cpp
  editor/window.cpp
)
target_link_libraries(cind_application PUBLIC cind_async cind_document PRIVATE PkgConfig::Utf8Proc)

add_library(cind_c_family STATIC
  editor/cpp_mode.cpp
)
target_include_directories(cind_c_family PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cind_c_family PUBLIC cind_application PRIVATE cind_indent)

add_library(cind_scheme STATIC
  editor/scheme_mode.cpp
)
target_include_directories(cind_scheme PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cind_scheme PUBLIC cind_application PRIVATE cind_indent)

set(CIND_SCHEME_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/scheme")
set(CIND_ARES_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/ares.scm")
set(CIND_ARES_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/ares.scm")
set(CIND_ASYNC_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/async.scm")
set(CIND_ASYNC_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/async.scm")
set(CIND_CORE_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/core.scm")
set(CIND_CORE_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/core.scm")
set(CIND_COMMAND_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/command.scm")
set(CIND_COMMAND_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/command.scm")
set(CIND_DEVELOPMENT_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/development.scm")
set(CIND_DEVELOPMENT_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/development.scm")
set(CIND_EMACS_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/emacs.scm")
set(CIND_EMACS_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/emacs.scm")
set(CIND_EXTENSION_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/extension.scm")
set(CIND_EXTENSION_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/extension.scm")
set(CIND_HELIX_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/helix.scm")
set(CIND_HELIX_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/helix.scm")
set(CIND_INPUT_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/input.scm")
set(CIND_INPUT_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/input.scm")
set(CIND_INTROSPECT_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/introspect.scm")
set(CIND_INTROSPECT_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/introspect.scm")
set(CIND_LIFECYCLE_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/lifecycle.scm")
set(CIND_LIFECYCLE_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/lifecycle.scm")
set(CIND_LSP_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/lsp.scm")
set(CIND_LSP_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/lsp.scm")
set(CIND_MEOW_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/meow.scm")
set(CIND_MEOW_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/meow.scm")
set(CIND_MINIBUFFER_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/minibuffer.scm")
set(CIND_MINIBUFFER_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/minibuffer.scm")
set(CIND_POINTER_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/pointer.scm")
set(CIND_POINTER_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/pointer.scm")
set(CIND_PAREDIT_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/paredit.scm")
set(CIND_PAREDIT_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/paredit.scm")
set(CIND_STRUCTURAL_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/structural.scm")
set(CIND_STRUCTURAL_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/structural.scm")
set(CIND_VIM_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/vim.scm")
set(CIND_VIM_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/vim.scm")
set(CIND_TOY_MODAL_SCHEME_SOURCE "${PROJECT_SOURCE_DIR}/scheme/cind/toy-modal.scm")
set(CIND_TOY_MODAL_SCHEME_OUTPUT "${CIND_SCHEME_OUTPUT_DIR}/cind/toy-modal.scm")
add_custom_command(
  OUTPUT "${CIND_ARES_SCHEME_OUTPUT}"
         "${CIND_ASYNC_SCHEME_OUTPUT}"
         "${CIND_CORE_SCHEME_OUTPUT}" "${CIND_COMMAND_SCHEME_OUTPUT}"
         "${CIND_DEVELOPMENT_SCHEME_OUTPUT}"
         "${CIND_EMACS_SCHEME_OUTPUT}" "${CIND_EXTENSION_SCHEME_OUTPUT}"
         "${CIND_HELIX_SCHEME_OUTPUT}"
         "${CIND_INPUT_SCHEME_OUTPUT}" "${CIND_INTROSPECT_SCHEME_OUTPUT}"
         "${CIND_LIFECYCLE_SCHEME_OUTPUT}"
         "${CIND_LSP_SCHEME_OUTPUT}"
         "${CIND_MEOW_SCHEME_OUTPUT}" "${CIND_MINIBUFFER_SCHEME_OUTPUT}"
         "${CIND_POINTER_SCHEME_OUTPUT}"
         "${CIND_PAREDIT_SCHEME_OUTPUT}"
         "${CIND_STRUCTURAL_SCHEME_OUTPUT}"
         "${CIND_VIM_SCHEME_OUTPUT}"
         "${CIND_TOY_MODAL_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CIND_SCHEME_OUTPUT_DIR}/cind"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_ARES_SCHEME_SOURCE}" "${CIND_ARES_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_ASYNC_SCHEME_SOURCE}" "${CIND_ASYNC_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_CORE_SCHEME_SOURCE}" "${CIND_CORE_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_COMMAND_SCHEME_SOURCE}" "${CIND_COMMAND_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_DEVELOPMENT_SCHEME_SOURCE}" "${CIND_DEVELOPMENT_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_EMACS_SCHEME_SOURCE}" "${CIND_EMACS_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_EXTENSION_SCHEME_SOURCE}" "${CIND_EXTENSION_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_HELIX_SCHEME_SOURCE}" "${CIND_HELIX_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_INPUT_SCHEME_SOURCE}" "${CIND_INPUT_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_INTROSPECT_SCHEME_SOURCE}" "${CIND_INTROSPECT_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_LIFECYCLE_SCHEME_SOURCE}" "${CIND_LIFECYCLE_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_LSP_SCHEME_SOURCE}" "${CIND_LSP_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_MEOW_SCHEME_SOURCE}" "${CIND_MEOW_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_MINIBUFFER_SCHEME_SOURCE}" "${CIND_MINIBUFFER_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_POINTER_SCHEME_SOURCE}" "${CIND_POINTER_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_PAREDIT_SCHEME_SOURCE}" "${CIND_PAREDIT_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_STRUCTURAL_SCHEME_SOURCE}" "${CIND_STRUCTURAL_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_VIM_SCHEME_SOURCE}" "${CIND_VIM_SCHEME_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CIND_TOY_MODAL_SCHEME_SOURCE}" "${CIND_TOY_MODAL_SCHEME_OUTPUT}"
  DEPENDS "${CIND_ARES_SCHEME_SOURCE}" "${CIND_ASYNC_SCHEME_SOURCE}"
          "${CIND_CORE_SCHEME_SOURCE}" "${CIND_COMMAND_SCHEME_SOURCE}"
          "${CIND_DEVELOPMENT_SCHEME_SOURCE}"
          "${CIND_EMACS_SCHEME_SOURCE}" "${CIND_EXTENSION_SCHEME_SOURCE}"
          "${CIND_HELIX_SCHEME_SOURCE}"
          "${CIND_INPUT_SCHEME_SOURCE}" "${CIND_INTROSPECT_SCHEME_SOURCE}"
          "${CIND_LIFECYCLE_SCHEME_SOURCE}"
          "${CIND_LSP_SCHEME_SOURCE}"
          "${CIND_MEOW_SCHEME_SOURCE}" "${CIND_MINIBUFFER_SCHEME_SOURCE}"
          "${CIND_POINTER_SCHEME_SOURCE}"
          "${CIND_PAREDIT_SCHEME_SOURCE}"
          "${CIND_STRUCTURAL_SCHEME_SOURCE}"
          "${CIND_VIM_SCHEME_SOURCE}"
          "${CIND_TOY_MODAL_SCHEME_SOURCE}"
  VERBATIM
)
add_custom_target(cind_scheme_resources
  DEPENDS "${CIND_ARES_SCHEME_OUTPUT}"
          "${CIND_ASYNC_SCHEME_OUTPUT}"
          "${CIND_CORE_SCHEME_OUTPUT}" "${CIND_COMMAND_SCHEME_OUTPUT}"
          "${CIND_DEVELOPMENT_SCHEME_OUTPUT}"
          "${CIND_EMACS_SCHEME_OUTPUT}" "${CIND_EXTENSION_SCHEME_OUTPUT}"
          "${CIND_HELIX_SCHEME_OUTPUT}"
          "${CIND_INPUT_SCHEME_OUTPUT}" "${CIND_INTROSPECT_SCHEME_OUTPUT}"
          "${CIND_LIFECYCLE_SCHEME_OUTPUT}"
          "${CIND_LSP_SCHEME_OUTPUT}"
          "${CIND_MEOW_SCHEME_OUTPUT}" "${CIND_MINIBUFFER_SCHEME_OUTPUT}"
          "${CIND_POINTER_SCHEME_OUTPUT}"
          "${CIND_PAREDIT_SCHEME_OUTPUT}"
          "${CIND_STRUCTURAL_SCHEME_OUTPUT}"
          "${CIND_VIM_SCHEME_OUTPUT}"
          "${CIND_TOY_MODAL_SCHEME_OUTPUT}"
)

add_library(cind_guile STATIC
  script/guile_async_bridge.cpp
  script/guile_runtime.cpp
)
target_include_directories(cind_guile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cind_guile PRIVATE
  CIND_BUNDLED_SCHEME_DIR="${CIND_SCHEME_OUTPUT_DIR}"
  CIND_ARES_SCHEME_DIR="${PROJECT_SOURCE_DIR}/third_party/guile-ares-rs/src/guile"
  CIND_GUILE_SITE_DIR="${CIND_GUILE_SITE_DIR}"
  CIND_GUILE_SITE_CCACHE_DIR="${CIND_GUILE_SITE_CCACHE_DIR}"
)
add_dependencies(cind_guile cind_scheme_resources cind_fibers_external)
target_link_libraries(cind_guile
  PUBLIC cind_application cind_c_family cind_scheme cind_script_async
  PRIVATE Cind::Guile
)

add_library(cind_editor STATIC
  cli/session.cpp
  editor/editing_mechanisms.cpp
  editor/editor_application.cpp
  editor/noun_evaluator.cpp
  editor/project_service.cpp
)
target_link_libraries(cind_editor
  PUBLIC cind_async cind_indent cind_application cind_c_family cind_guile cind_project_io
  PRIVATE cind_lsp cind_ui Threads::Threads
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
  target_link_libraries(cind_skia_ui
    PUBLIC cind_ui
    PRIVATE Skia::Skia Fontconfig::Fontconfig Freetype::Freetype
  )

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
  if (APPLE)
    find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
    target_sources(cind-gui PRIVATE gui/mac_scroll_bridge.mm)
    set_source_files_properties(gui/mac_scroll_bridge.mm PROPERTIES
      COMPILE_OPTIONS "-fobjc-arc"
    )
    target_link_libraries(cind-gui PRIVATE "${APPKIT_FRAMEWORK}")
  endif()
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
