//===--- BugproneTidyModule.cpp - clang-tidy ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../ClangTidy.h"
#include "../ClangTidyModule.h"
#include "../ClangTidyModuleRegistry.h"
#include "../cppcoreguidelines/NarrowingConversionsCheck.h"
#include "AbortCheck.h"


namespace clang {
namespace tidy {
namespace fixrev {

class FixrevModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<AbortCheck>(
        "fixrev-abort");
  }
};

} // namespace fixrev

// Register the FixrevTidyModule using this statically initialized variable.
static ClangTidyModuleRegistry::Add<fixrev::FixrevModule>
    X("fixrev-module", "Adds checks for fixreverter code constructs.");

// This anchor is used to force the linker to link in the generated object file
// and thus register the FixrevTidyModule.
volatile int FixrevModuleAnchorSource = 0;

} // namespace tidy
} // namespace clang
