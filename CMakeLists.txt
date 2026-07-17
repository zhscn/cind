cmake_minimum_required(VERSION 3.28)
project(cind LANGUAGES CXX)

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
pkg_check_modules(Guile REQUIRED IMPORTED_TARGET guile-3.0)
find_package(Threads REQUIRED)

if (CIND_BUILD_GUI)
  include(cmake/WebKitSkia.cmake)
  cind_add_webkit_skia()
endif()

include(CTest)
if (BUILD_TESTING)
  include(FetchContent)
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
