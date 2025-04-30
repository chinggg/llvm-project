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

// needed by myutils functions
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace llvm;
using namespace ore;

namespace myutils {
// Implementation of utils used by custom mutli-arch MachineFunctionPasses

template <typename T>
std::string join(const SmallVectorImpl<T> &vec, const std::string &sep) {
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

// Explicit template instantiation to avoid undefined reference linker errors
// see https://isocpp.org/wiki/faq/templates#separate-template-fn-defn-from-decl
template
std::string join(const SmallVectorImpl<std::string> &vec, const std::string &sep);

template
std::string join(const SmallVectorImpl<const Instruction*> &vec, const std::string &sep);

template
std::string join(const SmallVectorImpl<const MachineInstr*> &vec, const std::string &sep);

template
std::string join(const SmallVectorImpl<unsigned> &vec, const std::string &sep);

std::string getLineSrc(const DebugLoc &DL) {
  if (!DL) {
    return "[getDebugLoc returns null]";
  }
  StringRef FileName = DL->getScope()->getFilename();
  unsigned Line = DL.getLine();
  std::string SourceLine;
  std::error_code EC;
  std::ifstream File(FileName.str());
  for (unsigned i = 0; i < Line; ++i) {
    std::getline(File, SourceLine);
  }
  // escape \t in SourceLine with 4 spaces
  std::replace(SourceLine.begin(), SourceLine.end(), '\t', ' ');
  // trim \n and \r from SourceLine
  SourceLine.erase(std::remove(SourceLine.begin(), SourceLine.end(), '\n'), SourceLine.end());
  SourceLine.erase(std::remove(SourceLine.begin(), SourceLine.end(), '\r'), SourceLine.end());
  return SourceLine;
}

unsigned getLineNumber(const DebugLoc &DL) {
  if (DL) {
    return DL.getLine();
  }
  return 0;
}

bool isNameTrivial(const StringRef &Name) {
  const std::string TrivialKeywords[] = {".h", "include/", "third_party", "third-party", "fuzz", "test", "helper"};
  for (const auto &Keyword : TrivialKeywords) {
    if (Name.lower().find(Keyword) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace myutils

using namespace myutils;

static cl::opt<bool>
    EnableMFPassDump("mfpass-dump",
                      cl::desc("Dump MachineFunctionPbss changed cjump instructions, can use -filter-print-funcs to filter functions."),
                      cl::init(false), cl::Hidden);

// This will collect conditional jump information and store it for later comparison
static SmallVector<const MachineInstr*, 16> BeforeCjumpInsts;
static SmallVector<std::string, 16> BeforeCjumpSrcs;
static SmallVector<unsigned, 16> BeforeCjumpLines;
static std::string BeforeFunction;
static std::string BeforeFileName;
static std::string BeforeContext;

bool dumpCjumppInsts(const MachineFunction &MF, StringRef Context, bool IsBefore) {
  if (Context.ends_with("CountInstr")) return false;

  // Skip if function is not in print list
  if (!isFunctionInPrintList(MF.getName()))
    return false;

  auto *SP = MF.getFunction().getSubprogram();
  if (!SP) return false;  // no debug info available if not compiled with -g
  auto FileName = SP->getFilename();

  if (IsBefore) {
    // Clear previous state and collect current state
    BeforeFunction = MF.getName().str();
    BeforeFileName = FileName.str();
    BeforeContext = Context.str();
    BeforeCjumpInsts.clear();
    BeforeCjumpSrcs.clear();
    BeforeCjumpLines.clear();

    // Collect all conditional jumps
    for (const MachineBasicBlock &MBB : MF) {
      for (const MachineInstr &MI : MBB) {
        if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
          BeforeCjumpInsts.push_back(&MI);
          BeforeCjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
          BeforeCjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
        }
      }
    }
    return true;
  }

  // After pass execution - collect current state
  SmallVector<const MachineInstr*, 16> AfterCjumpInsts;
  SmallVector<std::string, 16> AfterCjumpSrcs;
  SmallVector<unsigned, 16> AfterCjumpLines;

  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        AfterCjumpInsts.push_back(&MI);
        AfterCjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
        AfterCjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
      }
    }
  }

  // Find added and removed jumps based on line numbers
  // (using line numbers as a proxy for identifying the same jump)
  SmallVector<unsigned, 16> AddedLines;
  SmallVector<const MachineInstr*, 16> AddedInsts;
  SmallVector<std::string, 16> AddedSrcs;

  SmallVector<unsigned, 16> RemovedLines;
  SmallVector<const MachineInstr*, 16> RemovedInsts;
  SmallVector<std::string, 16> RemovedSrcs;

  // Find added jumps
  for (size_t i = 0; i < AfterCjumpLines.size(); ++i) {
    if (std::find(BeforeCjumpLines.begin(), BeforeCjumpLines.end(), 
                 AfterCjumpLines[i]) == BeforeCjumpLines.end()) {
      AddedLines.push_back(AfterCjumpLines[i]);
      AddedInsts.push_back(AfterCjumpInsts[i]);
      AddedSrcs.push_back(AfterCjumpSrcs[i]);
    }
  }

  // Find removed jumps
  for (size_t i = 0; i < BeforeCjumpLines.size(); ++i) {
    if (std::find(AfterCjumpLines.begin(), AfterCjumpLines.end(), 
                 BeforeCjumpLines[i]) == BeforeCjumpLines.end()) {
      RemovedLines.push_back(BeforeCjumpLines[i]);
      RemovedInsts.push_back(BeforeCjumpInsts[i]);
      RemovedSrcs.push_back(BeforeCjumpSrcs[i]);
    }
  }

  // Only print if there were changes
  if (!AddedLines.empty() || !RemovedLines.empty()) {
    // Build the complete JSON string in memory before outputting
    std::string JsonOutput;
    raw_string_ostream JsonStream(JsonOutput);
    
    JsonStream << "{"
              << "\"function\": \"" << BeforeFunction << "\", "
              << "\"file\": \"" << BeforeFileName << "\", "
              << "\"context\": \"" << BeforeContext << "\", "
              << "\"cjump_count_before\": " << BeforeCjumpInsts.size() << ", "
              << "\"cjump_count_after\": " << AfterCjumpInsts.size() << ", "
              << "\"removed_cjump_count\": " << RemovedLines.size() << ", "
              << "\"removed_cjump_lines\": [" << join(RemovedLines) << "], "
              << "\"removed_cjump_insts\": [" << join(RemovedInsts) << "], "
              << "\"removed_cjump_srcs\": [" << join(RemovedSrcs) << "], "
              << "\"added_cjump_count\": " << AddedLines.size() << ", "
              << "\"added_cjump_lines\": [" << join(AddedLines) << "], "
              << "\"added_cjump_insts\": [" << join(AddedInsts) << "], "
              << "\"added_cjump_srcs\": [" << join(AddedSrcs) << "]"
              << "}\n";
    
    // Flush the stream to ensure all content is in the string
    JsonStream.flush();
    
    // Output the complete JSON string - no lock needed as each write to errs() is atomic
    errs() << JsonOutput;
    
    return true;
  }
  
  // No changes to cjumps
  return false;
}

Pass *MachineFunctionPass::createPrinterPass(raw_ostream &O,
                                             const std::string &Banner) const {
  return createMachineFunctionPrinterPass(O, Banner);
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

  if (EnableMFPassDump) {
    dumpCjumppInsts(MF, getPassName(), true);
  }
  bool RV = runOnMachineFunction(MF);
  if (EnableMFPassDump) {
    dumpCjumppInsts(MF, getPassName(), false);
  }

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
  MFProps.reset(ClearedProperties);

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
