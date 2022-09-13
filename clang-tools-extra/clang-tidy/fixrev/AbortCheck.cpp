//===--- AbortCheck.cpp - clang-tidy --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AbortCheck.h"
#include "Common.cpp"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Tooling/Transformer/RewriteRule.h"
#include "clang/Tooling/Transformer/Stencil.h"
#include <string>

using namespace clang::tidy::utils;
using namespace clang::ast_matchers;
using namespace clang::transformer;

namespace clang {
namespace tidy {
namespace fixrev {

// Abort Pattern: revert if-statement whose small body has jump statement
RewriteRuleWith<std::string> abortRule() {
  const StringRef C = "C", T = "T"; // C=Condition, T=Then
  auto AbortCond = expr(anyOf(EqNull, PtrCmp, NumCmp));
  auto AbortConds = concatBinaryOp(AbortCond, 4).bind(C);
  auto AbortBody = stmt(anyOf(JmpStmt, compoundStmt(statementCountUpTo(3),
                                                    hasDescendant(JmpStmt))))
                       .bind(T);
  auto AbortEdit = changeTo(cat("if (0) ", statement(T.str())));
  auto AbortMeta = cat("revert abort condition: ", statement(C.str()));
  const auto AbortRule =
      makeRule(ifStmt(hasCondition(AbortConds), hasThen(AbortBody)), AbortEdit,
               AbortMeta);
  return AbortRule;
}

AbortCheck::AbortCheck(StringRef Name, ClangTidyContext *Context)
    : TransformerClangTidyCheck(abortRule(), Name, Context) {}

} // namespace fixrev
} // namespace tidy
} // namespace clang
