add_library(cind_document STATIC
  document/document.cpp
  document/line_index.cpp
)
target_include_directories(cind_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cind_document PUBLIC cxx_std_23)
