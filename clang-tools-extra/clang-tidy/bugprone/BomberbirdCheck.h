//===--- BomberbirdCheck.h - clang-tidy -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_BUGPRONE_BOMBERBIRDCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_BUGPRONE_BOMBERBIRDCHECK_H

// #include "../ClangTidyCheck.h"
#include "../utils/TransformerClangTidyCheck.h"
//#include "clang/Tooling/Transformer/Transformer.h"
#include "clang/Tooling/Transformer/RangeSelector.h"
#include "clang/Tooling/Transformer/RewriteRule.h"
#include "clang/Tooling/Transformer/Stencil.h"


namespace clang {
namespace tidy {
namespace bugprone {

using namespace clang::tidy::utils;
using namespace clang::transformer;
using namespace clang::ast_matchers;

auto BombRule = makeRule(declRefExpr(to(functionDecl(hasName("memcpy")))),
	changeTo(cat("memcpy_wrapper")),
    cat("memcpy has been renamed as memcpy_wrapper"));

/// FIXME: Write a short description.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/bugprone-bomberbird.html
class BomberbirdCheck : public TransformerClangTidyCheck {
// class BomberbirdCheck : public ClangTidyCheck {
public:
  BomberbirdCheck(StringRef Name, ClangTidyContext *Context)
      : TransformerClangTidyCheck(BombRule, Name, Context) {}
      // : ClangTidyCheck(Name, Context) {}
  // void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  // void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace bugprone
} // namespace tidy
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_BUGPRONE_BOMBERBIRDCHECK_H
