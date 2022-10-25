#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_FIXREV_COMMON_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_FIXREV_COMMON_H

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace fixrev {

// statementCountUpTo(unsigned N): modified from statementCountIs()
inline AST_MATCHER_P(CompoundStmt, statementCountUpTo, unsigned, N) {
  return Node.size() <= N;
}

// ignoringCasts: modified from IgnoreImplicitCasts()
inline AST_MATCHER_P(Expr, ignoringCasts, ast_matchers::internal::Matcher<Expr>,
                     InnerMatcher) {
  return InnerMatcher.matches(*Node.IgnoreCasts(), Finder, Builder);
}

// concatBinaryOp: Matches [MinN, MaxN] binary operator concatenation of
// condition matcher
inline ast_matchers::internal::BindableMatcher<Stmt>
concatBinaryOp(const ast_matchers::internal::BindableMatcher<Stmt> Cond,
               unsigned MaxN, unsigned MinN = 0,
               const std::string &OpName = "||") {
  auto Conds = Cond;
  while (MinN--) {
    Conds = binaryOperator(hasOperatorName(OpName), hasOperands(Cond, Conds));
  }
  MaxN -= MinN;
  while (MaxN--) {
    Conds = expr(anyOf(Conds, binaryOperator(hasOperatorName(OpName),
                                             hasOperands(Cond, Conds))));
  }
  return Conds;
}

// Modified from nullPointerConstant(), match cStyleCastExpr wrapped NULL on
// specific C programs
AST_MATCHER_FUNCTION(ast_matchers::internal::Matcher<Expr>, nullConstant) {
  return ignoringCasts(nullPointerConstant());
  // return anyOf(nullPointerConstant(),
  // cStyleCastExpr(hasDescendant(expr(nullPointerConstant()))));
}

inline auto ArithOp = binaryOperator(
    hasAnyOperatorName("+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"));
inline auto CmpOp = binaryOperator(hasAnyOperatorName("<", ">", "<=", ">="));
inline auto RelOp = binaryOperator(hasAnyOperatorName("==", "!="));
inline auto JmpStmt =
    stmt(anyOf(breakStmt(), continueStmt(), gotoStmt(), returnStmt()));
inline auto NumLit = anyOf(integerLiteral(), nullConstant());
inline auto NumVar = hasType(isInteger());
inline auto NumTracer = anyOf(NumLit, NumVar, memberExpr(member(NumVar)));
inline auto PtrVar = hasType(isAnyPointer());
inline auto PtrTracer = anyOf(declRefExpr(PtrVar), memberExpr(member(PtrVar)));
inline auto SingleCaller =
    anyOf(sizeOfExpr((ignoringParenImpCasts(declRefExpr()))),
          callExpr(argumentCountIs(1),
                   hasArgument(0, ignoringParenImpCasts(declRefExpr()))));
inline auto NumVal =
    ignoringParenImpCasts(anyOf(NumLit, NumTracer, SingleCaller));
inline auto NumOp =
    ignoringParenImpCasts(anyOf(NumVal, binaryOperator(ArithOp, hasOperands(NumVal, NumVal))));
inline auto NumCmp =
    binaryOperator(anyOf(CmpOp, RelOp), hasEitherOperand(NumOp));
inline auto PtrOp = expr(
    anyOf(PtrTracer, binaryOperator(ArithOp, hasOperands(NumVal, PtrTracer))));
inline auto PtrCmp = binaryOperator(CmpOp, hasOperands(PtrOp, PtrOp));
inline auto PtrRel = binaryOperator(RelOp, hasOperands(PtrOp, PtrOp));
inline auto EqNull = binaryOperator(
    hasOperatorName("=="), hasOperands(ignoringParenImpCasts(PtrTracer),
                                       ignoringParenImpCasts(nullConstant())));
inline auto NotEqNull = binaryOperator(
    hasOperatorName("!="), hasOperands(ignoringParenImpCasts(PtrTracer),
                                       ignoringParenImpCasts(nullConstant())));

} // namespace fixrev
} // namespace tidy
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_FIXREV_COMMON_H