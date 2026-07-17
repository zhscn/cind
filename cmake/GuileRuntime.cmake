include_guard(GLOBAL)

include(ExternalProject)

function(cind_add_guile_runtime)
  set(guile_version 3.0.11)
  set(guile_prefix "${CMAKE_BINARY_DIR}/guile-runtime")
  set(guile_libdir "${guile_prefix}/lib")
  set(guile_library
      "${guile_libdir}/libguile-3.0${CMAKE_SHARED_LIBRARY_SUFFIX}")

  find_program(CIND_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
  find_program(CIND_AUTORECONF_EXECUTABLE NAMES autoreconf REQUIRED)
  find_library(CIND_BDW_GC_LIBRARY NAMES gc REQUIRED)

  ExternalProject_Add(cind_guile_external
    URL https://ftp.gnu.org/gnu/guile/guile-${guile_version}.tar.gz
    URL_HASH
      SHA256=3c9c16972a73bb792752f2e4f1cce7212d7638d5494b5f7e8e19f3819dbf3a19
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/guile-build"
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env
        "CC=${CMAKE_C_COMPILER}"
        <SOURCE_DIR>/configure
        --prefix=${guile_prefix}
        --libdir=${guile_libdir}
        --disable-static
        --enable-shared
        --disable-error-on-warning
        --without-libreadline-prefix
    BUILD_COMMAND ${CIND_MAKE_EXECUTABLE} -j4
    INSTALL_COMMAND ${CIND_MAKE_EXECUTABLE} install
    BUILD_BYPRODUCTS "${guile_library}"
  )

  set(guile_environment
    "PATH=${guile_prefix}/bin:$ENV{PATH}"
    "PKG_CONFIG_PATH=${guile_libdir}/pkgconfig:$ENV{PKG_CONFIG_PATH}"
    "LD_LIBRARY_PATH=${guile_libdir}:$ENV{LD_LIBRARY_PATH}"
    "GUILE=${guile_prefix}/bin/guile"
    "GUILE_CONFIG=${guile_prefix}/bin/guile-config"
    "GUILD=${guile_prefix}/bin/guild"
  )
  ExternalProject_Add(cind_fibers_external
    URL https://codeberg.org/guile/fibers/archive/v1.4.3.tar.gz
    URL_HASH
      SHA256=fd055e9cee7ec11f7d9a6009e5387c002e99beb9fa9d2da3eab1b4da92b5be91
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/fibers-build"
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env ${guile_environment}
        ${CIND_AUTORECONF_EXECUTABLE} -vif <SOURCE_DIR>
      COMMAND
      ${CMAKE_COMMAND} -E env ${guile_environment}
        "CC=${CMAKE_C_COMPILER}"
        <SOURCE_DIR>/configure
        --prefix=${guile_prefix}
        --libdir=${guile_libdir}
        --disable-static
        --enable-shared
        --disable-Werror
    BUILD_COMMAND
      ${CMAKE_COMMAND} -E env ${guile_environment}
        ${CIND_MAKE_EXECUTABLE} -j4
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E env ${guile_environment}
        ${CIND_MAKE_EXECUTABLE} install
    BUILD_BYPRODUCTS
      "${guile_prefix}/share/guile/site/3.0/fibers.scm"
      "${guile_libdir}/guile/3.0/extensions/fibers-epoll${CMAKE_SHARED_LIBRARY_SUFFIX}"
    DEPENDS cind_guile_external
  )

  file(MAKE_DIRECTORY "${guile_prefix}/include/guile/3.0")
  add_library(cind_private_guile SHARED IMPORTED GLOBAL)
  set_target_properties(cind_private_guile PROPERTIES
    IMPORTED_LOCATION "${guile_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${guile_prefix}/include/guile/3.0"
    INTERFACE_LINK_LIBRARIES
      "${CIND_BDW_GC_LIBRARY};Threads::Threads;${CMAKE_DL_LIBS}"
  )
  add_dependencies(cind_private_guile cind_guile_external)
  add_library(Cind::Guile ALIAS cind_private_guile)

  set(CIND_GUILE_PREFIX "${guile_prefix}" PARENT_SCOPE)
  set(CIND_GUILE_SITE_DIR "${guile_prefix}/share/guile/site/3.0" PARENT_SCOPE)
  set(CIND_GUILE_SITE_CCACHE_DIR
      "${guile_libdir}/guile/3.0/site-ccache" PARENT_SCOPE)
endfunction()
