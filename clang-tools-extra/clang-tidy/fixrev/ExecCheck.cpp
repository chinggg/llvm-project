//===--- ExecCheck.cpp - clang-tidy ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ExecCheck.h"
#include "Common.cpp"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Tooling/Transformer/Stencil.h"

using namespace clang::ast_matchers;
using namespace clang::ast_matchers;
using namespace clang::transformer;

namespace clang {
namespace tidy {
namespace fixrev {

// Exec Pattern: revert if/for/while statement whose body has no jump statement
RewriteRuleWith<std::string> execRule() {
  const StringRef C = "C", T = "T"; // C=Condition, T=Then
  auto ExecCond = expr(anyOf(NotEqNull, PtrCmp, NumCmp));
  auto ExecConds = concatBinaryOp(ExecCond, 4, 1, "&&").bind(C);
  auto ExecBody = stmt(unless(hasDescendant(JmpStmt))).bind(T);
  auto LoopStmt = stmt(anyOf(whileStmt(), forStmt()));
  auto ExecEdit = changeTo(cat(statement(C.str()), statement(T.str())));
  auto ExecMeta = cat("revert exec condition: ", statement(C.str()));
  const auto IfRule = makeRule(
      ifStmt(hasCondition(ExecConds), hasThen(ExecBody)), ExecEdit, ExecMeta);
  const auto ForRule = makeRule(
      forStmt(hasCondition(ExecConds), hasBody(ExecBody)), ExecEdit, ExecMeta);
  const auto WhileRule = makeRule(
      forStmt(hasCondition(ExecConds), hasBody(ExecBody)), ExecEdit, ExecMeta);
  return applyFirst({IfRule, ForRule, WhileRule});
}

ExecCheck::ExecCheck(StringRef Name, ClangTidyContext *Context)
    : TransformerClangTidyCheck(execRule(), Name, Context) {}

} // namespace fixrev
} // namespace tidy
} // namespace clang
