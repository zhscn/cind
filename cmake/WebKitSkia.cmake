include_guard(GLOBAL)

include(CheckCXXCompilerFlag)

# The vendored Skia target is maintained by WebKit as part of WebKit's CMake
# build. It uses three small WebKit compiler-flag helpers and the WebKit Epoxy
# package finder. This adapter supplies that surrounding contract without
# configuring the rest of WebKit.
function(cind_add_webkit_skia)
  set(skia_source_dir "${PROJECT_SOURCE_DIR}/third_party/skia")
  if (NOT EXISTS "${skia_source_dir}/CMakeLists.txt")
    message(FATAL_ERROR "Vendored Skia is missing ${skia_source_dir}/CMakeLists.txt")
  endif()

  function(WEBKIT_CHECK_COMPILER_FLAGS compiler result)
    if (NOT compiler STREQUAL "CXX")
      set(${result} FALSE PARENT_SCOPE)
      return()
    endif()
    set(supported TRUE)
    foreach(flag IN LISTS ARGN)
      string(MAKE_C_IDENTIFIER "CIND_CXX_SUPPORTS_${flag}" cache_variable)
      check_cxx_compiler_flag("${flag}" ${cache_variable})
      if (NOT ${cache_variable})
        set(supported FALSE)
        break()
      endif()
    endforeach()
    set(${result} ${supported} PARENT_SCOPE)
  endfunction()

  function(WEBKIT_ADD_COMPILER_FLAGS compiler kind subject)
    foreach(flag IN LISTS ARGN)
      WEBKIT_CHECK_COMPILER_FLAGS(${compiler} supported "${flag}")
      if (supported)
        set_property(${kind} ${subject} APPEND PROPERTY COMPILE_OPTIONS "${flag}")
      endif()
    endforeach()
  endfunction()

  macro(WEBKIT_ADD_TARGET_CXX_FLAGS target)
    WEBKIT_ADD_COMPILER_FLAGS(CXX TARGET ${target} ${ARGN})
  endmacro()

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(COMPILER_IS_CLANG ON)
  endif()
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(WTF_CPU_X86_64 ON)
  elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(WTF_CPU_ARM64 ON)
  elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(loongarch64|loong64)$")
    set(WTF_CPU_LOONGARCH64 ON)
  endif()

  list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/modules")
  if (NOT APPLE)
    find_package(Epoxy 1.4.0 REQUIRED)
  endif()
  find_package(Freetype 2.9.0 REQUIRED)
  find_package(Fontconfig 2.13.0 REQUIRED)
  find_package(ICU REQUIRED COMPONENTS uc i18n)
  find_package(JPEG REQUIRED)
  find_package(PNG REQUIRED)
  pkg_check_modules(HarfBuzz REQUIRED IMPORTED_TARGET harfbuzz)

  if (APPLE)
    set(USE_LIBEPOXY OFF)
  else()
    set(USE_LIBEPOXY ON)
  endif()
  set(USE_ANGLE_EGL OFF)
  set(USE_SKIA_ENCODERS OFF)
  set(USE_SKIA_OPENTYPE_SVG OFF)
  set(Skia_FRAMEWORK_HEADERS_DIR "${CMAKE_BINARY_DIR}/Skia/Headers")

  add_subdirectory("${skia_source_dir}" "${CMAKE_BINARY_DIR}/webkit-skia"
                   EXCLUDE_FROM_ALL)

  # WebKit's SVG option also enables these modules, but the editor needs text
  # shaping without pulling the SVG stack into the build.
  target_sources(Skia PRIVATE
    "${skia_source_dir}/modules/skunicode/src/SkUnicode.cpp"
    "${skia_source_dir}/modules/skunicode/src/SkUnicode_hardcoded.cpp"
    "${skia_source_dir}/modules/skunicode/src/SkUnicode_icu.cpp"
    "${skia_source_dir}/modules/skunicode/src/SkUnicode_icu_bidi.cpp"
    "${skia_source_dir}/modules/skunicode/src/SkUnicode_icu_builtin.cpp"
    "${skia_source_dir}/modules/skunicode/src/SkBidiFactory_icu_full.cpp"
    "${skia_source_dir}/modules/skshaper/src/SkShaper.cpp"
    "${skia_source_dir}/modules/skshaper/src/SkShaper_factory.cpp"
    "${skia_source_dir}/modules/skshaper/src/SkShaper_harfbuzz.cpp"
    "${skia_source_dir}/modules/skshaper/src/SkShaper_primitive.cpp"
    "${skia_source_dir}/modules/skshaper/src/SkShaper_skunicode.cpp"
  )
  target_compile_definitions(Skia PRIVATE
    SK_SHAPER_HARFBUZZ_AVAILABLE
    SK_SHAPER_PRIMITIVE_AVAILABLE
    SK_SHAPER_UNICODE_AVAILABLE
    SK_UNICODE_AVAILABLE
    SK_UNICODE_ICU_IMPLEMENTATION
    SKSHAPER_IMPLEMENTATION=1
    SKUNICODE_IMPLEMENTATION=1
    U_USING_ICU_NAMESPACE=0
  )
  target_link_libraries(Skia PRIVATE ICU::uc ICU::i18n PkgConfig::HarfBuzz)

  # Consumers compile against Skia headers as third-party system headers while
  # retaining the project's warning policy for their own translation units.
  get_target_property(skia_interface_includes Skia INTERFACE_INCLUDE_DIRECTORIES)
  set_property(TARGET Skia APPEND PROPERTY
               INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${skia_interface_includes}")

  # Project warning policy remains strict for cind sources. The vendored target
  # receives compatibility suppressions for the project's conversion checks.
  target_compile_options(Skia PRIVATE -Wno-conversion -Wno-shadow)
endfunction()
