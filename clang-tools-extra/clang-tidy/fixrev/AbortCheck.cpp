//===--- AbortCheck.cpp - clang-tidy --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AbortCheck.h"
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

// statementCountUpTo(unsigned N): modified from statementCountIs()
AST_MATCHER_P(CompoundStmt, statementCountUpTo, unsigned, N) {
  return Node.size() <= N;
}

// ignoringCasts: modified from IgnoreImplicitCasts()
AST_MATCHER_P(Expr, ignoringCasts, ast_matchers::internal::Matcher<Expr>,
              InnerMatcher) {
  return InnerMatcher.matches(*Node.IgnoreCasts(), Finder, Builder);
}

// concatBinaryOp: Matches concatenation of up to N binary operators
ast_matchers::internal::BindableMatcher<Stmt>
concatBinaryOp(const ast_matchers::internal::BindableMatcher<Stmt> Cond,
               unsigned N, const std::string &OpName = "||") {
  auto Conds = Cond;
  while (N--) {
    Conds = expr(anyOf(Cond, binaryOperator(hasOperatorName(OpName),
                                            hasOperands(Cond, Conds))));
  }
  return Conds;
}

// Modified from nullPointerConstant(), match cStyleCastExpr wrapped NULL on
// specific C programs
AST_MATCHER_FUNCTION(ast_matchers::internal::Matcher<Expr>, nullConstant) {
  return ignoringCasts(nullPointerConstant());
  // return anyOf(nullPointerConstant(), cStyleCastExpr(hasDescendant(expr(nullPointerConstant()))));
}

// Abort Pattern: revert if-statement whose small body has jump statement
RewriteRuleWith<std::string> abortRule() {
  const StringRef C = "C", T = "T"; // C=Condition, T=Then
  auto ArithOp = binaryOperator(hasAnyOperatorName("+", "-", "*", "/"));
  auto CmpOp = binaryOperator(hasAnyOperatorName("<", ">", "<=", ">="));
  auto RelOp = binaryOperator(hasAnyOperatorName("==", "!="));
  auto JmpStmt =
      stmt(anyOf(breakStmt(), continueStmt(), gotoStmt(), returnStmt()));
  auto NumLit = anyOf(integerLiteral(), nullConstant());
  auto NumVar = hasType(isInteger());
  auto NumTracer = anyOf(NumLit, NumVar, memberExpr(member(NumVar)));
  auto PtrVar = hasType(isAnyPointer());
  auto PtrTracer = anyOf(declRefExpr(PtrVar), memberExpr(member(PtrVar)));
  auto SingleCaller =
      anyOf(sizeOfExpr((ignoringParenImpCasts(declRefExpr()))),
            callExpr(argumentCountIs(1),
                     hasArgument(0, ignoringParenImpCasts(declRefExpr()))));
  auto NumVal = ignoringParenImpCasts(anyOf(NumLit, NumTracer, SingleCaller));
  auto NumOp =
      anyOf(NumVal, binaryOperator(ArithOp, hasOperands(NumVal, NumVal)));
  auto NumCmp = binaryOperator(anyOf(CmpOp, RelOp), hasOperands(NumOp, NumOp));
  auto PtrOp = expr(anyOf(
      PtrTracer, binaryOperator(ArithOp, hasOperands(NumVal, PtrTracer))));
  auto PtrCmp = binaryOperator(CmpOp, hasOperands(PtrOp, PtrOp));
  auto PtrRel = binaryOperator(RelOp, hasOperands(PtrOp, PtrOp));
  auto EqNull =
      binaryOperator(hasOperatorName("=="),
                     hasOperands(ignoringParenImpCasts(PtrTracer),
                                 ignoringParenImpCasts(nullConstant())));
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
