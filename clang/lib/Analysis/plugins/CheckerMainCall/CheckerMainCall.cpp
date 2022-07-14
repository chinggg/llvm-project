#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CallEvent.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"
#include "clang/StaticAnalyzer/Frontend/CheckerRegistry.h"

using namespace clang;
using namespace ento;

namespace {

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

// Register plugin!
extern "C" void clang_registerCheckers(CheckerRegistry &registry) {
  registry.addChecker<MainCallChecker>(
      "plugin.MainCallChecker", "Disallows calls to functions called main",
      "");
}

extern "C" const char clang_analyzerAPIVersionString[] =
    CLANG_ANALYZER_API_VERSION_STRING;
