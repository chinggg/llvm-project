#include "clang/StaticAnalyzer/Checkers/BuiltinCheckerRegistration.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"

using namespace clang;
using namespace ento;

namespace {

// 检查器的类名不必与在Checkers.td文件中定义的检查器的注册名称保持一致
class MainCallChecker : public Checker<check::PreCall> {
public:
  void checkPreCall(const CallEvent &Call, CheckerContext &Ctx) const;

private:
  mutable std::unique_ptr<BugType> BT;
};

} // anonymous namespace

void MainCallChecker::checkPreCall(const CallEvent &Call,
                                   CheckerContext &Ctx) const {
  if (const IdentifierInfo *II = Call.getCalleeIdentifier()) {
    if (II->isStr("main")) {
      if (!BT) {
        BT.reset(new BugType(this, "Call to main", "Example checker"));
      }
      ExplodedNode *Node = Ctx.generateErrorNode();
      auto Report
        = std::make_unique<PathSensitiveBugReport>(*BT, BT->getDescription(), Node);
      Ctx.emitReport(std::move(Report));
    }
  }
}

// void ento::registerXXX(CheckerManager &Mgr)中的XXX必须与在Checkers.td文件中定义的检查器的注册名称保持一致
void ento::registerMainCallChecker(CheckerManager &Mgr) {
  Mgr.registerChecker<MainCallChecker>();
}

// ento::shouldRegisterXXX(const CheckerManager &amp;mgr)中的XXX必须与在Checkers.td文件中定义的检查器的注册名称保持一致。
bool ento::shouldRegisterMainCallChecker(const CheckerManager &mgr) {
  return true;
}
