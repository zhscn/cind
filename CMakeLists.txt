cmake_minimum_required(VERSION 3.28)
project(cind LANGUAGES C CXX)

if (CIND_BUILD_GUI AND APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wconversion -Wshadow)

option(CIND_BUILD_GUI "Build the Skia/SDL3 graphical editor frontend" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(Utf8Proc REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(Libuv REQUIRED IMPORTED_TARGET libuv)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  glaze
  URL https://github.com/stephenberry/glaze/archive/refs/tags/v7.9.1.tar.gz
  URL_HASH SHA256=d6dee391276f5375672c35d06058e4fd8f1f30f62bae163a004b3bd13a4e2ae3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(glaze)

include(cmake/GuileRuntime.cmake)
cind_add_guile_runtime()

list(APPEND CMAKE_BUILD_RPATH "${CIND_GUILE_PREFIX}/lib")

if (CIND_BUILD_GUI)
  include(cmake/WebKitSkia.cmake)
  cind_add_webkit_skia()
endif()

include(CTest)
if (BUILD_TESTING)
  FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(doctest)
endif()

add_subdirectory(src)

if (BUILD_TESTING)
  add_subdirectory(tests)
endif()
