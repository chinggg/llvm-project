//===-- MachineFunctionPass.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the definitions of the MachineFunctionPass members.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/IVUsers.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PrintPasses.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace llvm;
using namespace ore;

Pass *MachineFunctionPass::createPrinterPass(raw_ostream &O,
                                             const std::string &Banner) const {
  return createMachineFunctionPrinterPass(O, Banner);
}

// get source line from debugloc as string by reading file and specific line
std::string getLineSrc(const DebugLoc &DL) {
  if (!DL) {
    return "[getDebugLoc returns null]";
  }
  StringRef FileName = DL->getFilename();
  unsigned Line = DL.getLine();
  std::string SourceLine;
  std::error_code EC;
  std::ifstream File(FileName.str());
  for (unsigned i = 0; i < Line; ++i) {
    std::getline(File, SourceLine);
  }
  return SourceLine;
}

unsigned getLineNumber(const DebugLoc &DL) {
  if (DL) {
    return DL.getLine();
  }
  return 0;
}

template <typename T>
std::string join(const SmallVectorImpl<T> &vec, const std::string &sep = ", ") {
  std::ostringstream sss;
  for (size_t i = 0; i < vec.size(); ++i) {
    std::string str;
    raw_string_ostream ss(str);
    if constexpr (std::is_pointer<T>::value) {
      ss << *vec[i];
    } else {
      ss << vec[i];
    }
    str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
    if constexpr (std::is_integral<T>::value) {
      sss << str;
    } else {  // wrap each element with ""
      sss << std::quoted(str);
    }
    if (i != vec.size() - 1) {
      sss << sep;
    }
  }
  return sss.str();
}

bool dumpCjumppInsts(const MachineFunction &MF, StringRef Context, bool IsBefore) {
  SmallVector<const MachineInstr*, 16> CjumpInsts;
  SmallVector<std::string, 16> CjumpSrcs;
  SmallVector<unsigned, 16> CjumpLines;

  if (Context == "X86CountInstr") return false;

  auto FileName = MF.getFunction().getSubprogram()->getFilename();

  for (const MachineBasicBlock &MBB : MF) {
    // Iterate through all instructions in the block
    for (const MachineInstr &MI : MBB) {
      // Check if the instruction is a conditional jump (e.g., X86 jcc)
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        CjumpInsts.push_back(&MI);
        CjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
        CjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
      }
    }
  }
  if (IsBefore)
    errs() << "{"
          << "\"function\": \"" << MF.getName() << "\", "
          << "\"file\": \"" << FileName << "\", "
          << "\"context\": \"" << Context << "\", "
          << "\"cjump_count_before\": " << CjumpInsts.size() << ", "
          << "\"cjump_lines_before\": [" <<join(CjumpLines) << "], "
          << "\"cjump_insts_before\": [" << join(CjumpInsts) << "], "
          << "\"cjump_srcs_before\": [" << join(CjumpSrcs) << "], ";
  else
    errs() << "\"cjump_count_after\": " << CjumpInsts.size() << ", "
          << "\"cjump_lines_after\": [" <<join(CjumpLines) << "], "
          << "\"cjump_insts_after\": [" << join(CjumpInsts) << "], "
          << "\"cjump_srcs_after\": [" << join(CjumpSrcs) << "]"
          << "}\n";
  return true;
}

bool MachineFunctionPass::runOnFunction(Function &F) {
  // Do not codegen any 'available_externally' functions at all, they have
  // definitions outside the translation unit.
  if (F.hasAvailableExternallyLinkage())
    return false;

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();
  MachineFunction &MF = MMI.getOrCreateMachineFunction(F);

  MachineFunctionProperties &MFProps = MF.getProperties();

#ifndef NDEBUG
  if (!MFProps.verifyRequiredProperties(RequiredProperties)) {
    errs() << "MachineFunctionProperties required by " << getPassName()
           << " pass are not met by function " << F.getName() << ".\n"
           << "Required properties: ";
    RequiredProperties.print(errs());
    errs() << "\nCurrent properties: ";
    MFProps.print(errs());
    errs() << "\n";
    llvm_unreachable("MachineFunctionProperties check failed");
  }
#endif
  // Collect the MI count of the function before the pass.
  unsigned CountBefore, CountAfter;

  // Check if the user asked for size remarks.
  bool ShouldEmitSizeRemarks =
      F.getParent()->shouldEmitInstrCountChangedRemark();

  // If we want size remarks, collect the number of MachineInstrs in our
  // MachineFunction before the pass runs.
  if (ShouldEmitSizeRemarks)
    CountBefore = MF.getInstructionCount();

  // For --print-changed, if the function name is a candidate, save the
  // serialized MF to be compared later.
  SmallString<0> BeforeStr, AfterStr;
  StringRef PassID;
  if (PrintChanged != ChangePrinter::None) {
    if (const PassInfo *PI = Pass::lookupPassInfo(getPassID()))
      PassID = PI->getPassArgument();
  }
  const bool IsInterestingPass = isPassInPrintList(PassID);
  const bool ShouldPrintChanged = PrintChanged != ChangePrinter::None &&
                                  IsInterestingPass &&
                                  isFunctionInPrintList(MF.getName());
  if (ShouldPrintChanged) {
    raw_svector_ostream OS(BeforeStr);
    MF.print(OS);
  }

  MFProps.reset(ClearedProperties);

  dumpCjumppInsts(MF, getPassName(), true);
  bool RV = runOnMachineFunction(MF);
  dumpCjumppInsts(MF, getPassName(), false);

  if (ShouldEmitSizeRemarks) {
    // We wanted size remarks. Check if there was a change to the number of
    // MachineInstrs in the module. Emit a remark if there was a change.
    CountAfter = MF.getInstructionCount();
    if (CountBefore != CountAfter) {
      MachineOptimizationRemarkEmitter MORE(MF, nullptr);
      MORE.emit([&]() {
        int64_t Delta = static_cast<int64_t>(CountAfter) -
                        static_cast<int64_t>(CountBefore);
        MachineOptimizationRemarkAnalysis R("size-info", "FunctionMISizeChange",
                                            MF.getFunction().getSubprogram(),
                                            &MF.front());
        R << NV("Pass", getPassName())
          << ": Function: " << NV("Function", F.getName()) << ": "
          << "MI Instruction count changed from "
          << NV("MIInstrsBefore", CountBefore) << " to "
          << NV("MIInstrsAfter", CountAfter)
          << "; Delta: " << NV("Delta", Delta);
        return R;
      });
    }
  }

  MFProps.set(SetProperties);

  // For --print-changed, print if the serialized MF has changed. Modes other
  // than quiet/verbose are unimplemented and treated the same as 'quiet'.
  if (ShouldPrintChanged || !IsInterestingPass) {
    if (ShouldPrintChanged) {
      raw_svector_ostream OS(AfterStr);
      MF.print(OS);
    }
    if (IsInterestingPass && BeforeStr != AfterStr) {
      errs() << ("*** IR Dump After " + getPassName() + " (" + PassID +
                 ") on " + MF.getName() + " ***\n");
      switch (PrintChanged) {
      case ChangePrinter::None:
        llvm_unreachable("");
      case ChangePrinter::Quiet:
      case ChangePrinter::Verbose:
      case ChangePrinter::DotCfgQuiet:   // unimplemented
      case ChangePrinter::DotCfgVerbose: // unimplemented
        errs() << AfterStr;
        break;
      case ChangePrinter::DiffQuiet:
      case ChangePrinter::DiffVerbose:
      case ChangePrinter::ColourDiffQuiet:
      case ChangePrinter::ColourDiffVerbose: {
        bool Color = llvm::is_contained(
            {ChangePrinter::ColourDiffQuiet, ChangePrinter::ColourDiffVerbose},
            PrintChanged.getValue());
        StringRef Removed = Color ? "\033[31m-%l\033[0m\n" : "-%l\n";
        StringRef Added = Color ? "\033[32m+%l\033[0m\n" : "+%l\n";
        StringRef NoChange = " %l\n";
        errs() << doSystemDiff(BeforeStr, AfterStr, Removed, Added, NoChange);
        break;
      }
      }
    } else if (llvm::is_contained({ChangePrinter::Verbose,
                                   ChangePrinter::DiffVerbose,
                                   ChangePrinter::ColourDiffVerbose},
                                  PrintChanged.getValue())) {
      const char *Reason =
          IsInterestingPass ? " omitted because no change" : " filtered out";
      errs() << "*** IR Dump After " << getPassName();
      if (!PassID.empty())
        errs() << " (" << PassID << ")";
      errs() << " on " << MF.getName() + Reason + " ***\n";
    }
  }
  return RV;
}

void MachineFunctionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MachineModuleInfoWrapperPass>();
  AU.addPreserved<MachineModuleInfoWrapperPass>();

  // MachineFunctionPass preserves all LLVM IR passes, but there's no
  // high-level way to express this. Instead, just list a bunch of
  // passes explicitly. This does not include setPreservesCFG,
  // because CodeGen overloads that to mean preserving the MachineBasicBlock
  // CFG in addition to the LLVM IR CFG.
  AU.addPreserved<BasicAAWrapperPass>();
  AU.addPreserved<DominanceFrontierWrapperPass>();
  AU.addPreserved<DominatorTreeWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addPreserved<GlobalsAAWrapperPass>();
  AU.addPreserved<IVUsersWrapperPass>();
  AU.addPreserved<LoopInfoWrapperPass>();
  AU.addPreserved<MemoryDependenceWrapperPass>();
  AU.addPreserved<ScalarEvolutionWrapperPass>();
  AU.addPreserved<SCEVAAWrapperPass>();

  FunctionPass::getAnalysisUsage(AU);
}
