set(LLVM_LINK_COMPONENTS
  Support
  )

set(LLVM_EXPORTED_SYMBOL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/Plugin.exports)
add_llvm_library(CheckerMainCallPlugin MODULE BUILDTREE_ONLY CheckerMainCall.cpp PLUGIN_TOOL clang)

clang_target_link_libraries(CheckerMainCallPlugin PRIVATE
  clangAnalysis
  clangAST
  clangStaticAnalyzerCore
  clangStaticAnalyzerFrontend
  )
