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
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
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

// This is a cache for file lines to avoid reading the same file multiple times
static StringMap<SmallVector<std::string>> FileCache;

static SmallVector<std::string>& getFileLines(StringRef FileName) {
  auto it = FileCache.find(FileName);
  if (it != FileCache.end()) {
    return it->second;
  }

  std::string Line;
  std::ifstream File(FileName.str());
  SmallVector<std::string>& Lines = FileCache[FileName];
  while (std::getline(File, Line)) {
    Lines.push_back(Line);
  }
  return Lines;
}

// Helper function to convert MachineInstr to string  
std::string getInstStr(const MachineInstr &MI) {
  std::string instStr;
  raw_string_ostream ss(instStr);
  ss << MI;
  return instStr;
}

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
  StringRef FileName = DL->getFilename();
  unsigned Line = DL.getLine();
  
  const auto& Lines = getFileLines(FileName);
  if (Line == 0 || Line > Lines.size()) {
    return "";
  }
  
  // Normalize the line only when it's actually needed
  std::string SourceLine = Lines[Line - 1];
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

unsigned getLineCol(const DebugLoc &DL) {
  if (DL) {
    return DL.getCol();
  }
  return 0;
}

std::string getCharSrc(const DebugLoc &DL) {
  if (!DL) {
    return "[getDebugLoc returns null]";
  }
  std::string LineSrc = getLineSrc(DL);
  unsigned Col = getLineCol(DL);
  if (Col > 0 && Col <= LineSrc.size()) {
    return std::string{LineSrc[Col - 1]};  // Column is 1-based, convert char to string
  }
  return std::string("");  // Invalid column, use empty character
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

/// Helper function to check if a machine instruction is a division
bool isDivisionMachineInstruction(const MachineInstr &MI) {
  unsigned Opcode = MI.getOpcode();
  
  // Check for generic division opcodes defined in TargetOpcodes.def
  switch (Opcode) {
  case TargetOpcode::G_SDIV:     // Generic signed division
  case TargetOpcode::G_UDIV:     // Generic unsigned division
  case TargetOpcode::G_FDIV:     // Generic floating-point division
  case TargetOpcode::G_SREM:     // Generic signed remainder
  case TargetOpcode::G_UREM:     // Generic unsigned remainder
  case TargetOpcode::G_FREM:     // Generic floating-point remainder
  case TargetOpcode::G_SDIVREM:  // Generic signed divrem
  case TargetOpcode::G_UDIVREM:  // Generic unsigned divrem
  case TargetOpcode::G_SDIVFIX:  // Generic signed fixed point division
  case TargetOpcode::G_UDIVFIX:  // Generic unsigned fixed point division
  case TargetOpcode::G_SDIVFIXSAT: // Generic signed saturating fixed point division
  case TargetOpcode::G_UDIVFIXSAT: // Generic unsigned saturating fixed point division
    return true;
  default:
    // For target-specific division instructions, we can still fall back to name checking
    // but only for opcodes that are not generic
    if (isTargetSpecificOpcode(Opcode)) {
      const TargetInstrInfo *TII = MI.getParent()->getParent()->getSubtarget().getInstrInfo();
      StringRef OpcodeName = TII->getName(Opcode);
      return OpcodeName.contains_insensitive("div");
    }
    return false;
  }
}

// Helper function to find differences between before and after instruction sets
template<typename T>
void findInstructionDifferences(const SmallVectorImpl<T>& beforeInsts,
                                const SmallVectorImpl<unsigned>& beforeLines,
                                const SmallVectorImpl<unsigned>& beforeCols,
                                const SmallVectorImpl<std::string>& beforeSrcs,
                                const SmallVectorImpl<std::string>& beforeChars,
                                const SmallVectorImpl<T>& afterInsts,
                                const SmallVectorImpl<unsigned>& afterLines,
                                const SmallVectorImpl<unsigned>& afterCols,
                                const SmallVectorImpl<std::string>& afterSrcs,
                                const SmallVectorImpl<std::string>& afterChars,
                                SmallVectorImpl<unsigned>& addedLines,
                                SmallVectorImpl<T>& addedInsts,
                                SmallVectorImpl<std::string>& addedSrcs,
                                SmallVectorImpl<unsigned>& addedCols,
                                SmallVectorImpl<std::string>& addedChars,
                                SmallVectorImpl<unsigned>& removedLines,
                                SmallVectorImpl<T>& removedInsts,
                                SmallVectorImpl<std::string>& removedSrcs,
                                SmallVectorImpl<unsigned>& removedCols,
                                SmallVectorImpl<std::string>& removedChars) {
  // Find added instructions - use line+col combination for precise matching
  for (size_t i = 0; i < afterLines.size(); ++i) {
    bool found = false;
    for (size_t j = 0; j < beforeLines.size(); ++j) {
      if (afterLines[i] == beforeLines[j] && afterCols[i] == beforeCols[j]) {
        found = true;
        break;
      }
    }
    if (!found) {
      addedLines.push_back(afterLines[i]);
      // addedInsts.push_back(afterInsts[i]);
      addedSrcs.push_back(afterSrcs[i]);
      addedCols.push_back(afterCols[i]);
      addedChars.push_back(afterChars[i]);
    }
  }

  // Find removed instructions - use line+col combination for precise matching
  for (size_t i = 0; i < beforeLines.size(); ++i) {
    bool found = false;
    for (size_t j = 0; j < afterLines.size(); ++j) {
      if (beforeLines[i] == afterLines[j] && beforeCols[i] == afterCols[j]) {
        found = true;
        break;
      }
    }
    if (!found) {
      removedLines.push_back(beforeLines[i]);
      // removedInsts.push_back(beforeInsts[i]);
      removedSrcs.push_back(beforeSrcs[i]);
      removedCols.push_back(beforeCols[i]);
      removedChars.push_back(beforeChars[i]);
    }
  }
}

} // namespace myutils

using namespace myutils;

static cl::opt<bool>
    EnableMFPassDump("mfpass-dump",
                      cl::desc("Dump MachineFunctionPbss changed cjump instructions, can use -filter-print-funcs to filter functions."),
                      cl::init(false), cl::Hidden);

// This will collect conditional jump information and store it for later comparison
static SmallVector<std::string, 16> BeforeCjumpInsts;
static SmallVector<std::string, 16> BeforeCjumpSrcs;
static SmallVector<unsigned, 16> BeforeCjumpLines;
static SmallVector<unsigned, 16> BeforeCjumpCols;
static SmallVector<std::string, 16> BeforeCjumpChars;
static SmallVector<std::string, 16> BeforeMDivInsts;
static SmallVector<std::string, 16> BeforeMDivSrcs;
static SmallVector<unsigned, 16> BeforeMDivLines;
static SmallVector<unsigned, 16> BeforeMDivCols;
static SmallVector<std::string, 16> BeforeMDivChars;
static SmallVector<std::string, 16> BeforeMMemInsts;
static SmallVector<std::string, 16> BeforeMMemSrcs;
static SmallVector<unsigned, 16> BeforeMMemLines;
static SmallVector<unsigned, 16> BeforeMMemCols;
static SmallVector<std::string, 16> BeforeMMemChars;
static std::string BeforeFunction;
static std::string BeforeFileName;
static std::string BeforeContext;

bool dumpInsts(const MachineFunction &MF, StringRef Context, bool IsBefore) {
#if LLVM_VERSION_MAJOR <= 15
  if (Context.endswith("CountInstr")) return false;
#else
  if (Context.ends_with("CountInstr")) return false;
#endif

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
    BeforeCjumpCols.clear();
    BeforeCjumpChars.clear();
    BeforeMDivInsts.clear();
    BeforeMDivSrcs.clear();
    BeforeMDivLines.clear();
    BeforeMDivCols.clear();
    BeforeMDivChars.clear();
    BeforeMMemInsts.clear();
    BeforeMMemSrcs.clear();
    BeforeMMemLines.clear();
    BeforeMMemCols.clear();
    BeforeMMemChars.clear();

    // Collect all conditional jumps, divisions, and memory operations
    for (const MachineBasicBlock &MBB : MF) {
      for (const MachineInstr &MI : MBB) {
        if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
          // BeforeCjumpInsts.push_back(getInstStr(MI));
          BeforeCjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
          BeforeCjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
          BeforeCjumpCols.push_back(getLineCol(MI.getDebugLoc()));
          BeforeCjumpChars.push_back(getCharSrc(MI.getDebugLoc()));
        }
        // Check for division instructions
        if (isDivisionMachineInstruction(MI)) {
          // BeforeMDivInsts.push_back(getInstStr(MI));
          BeforeMDivSrcs.push_back(getLineSrc(MI.getDebugLoc()));
          BeforeMDivLines.push_back(getLineNumber(MI.getDebugLoc()));
          BeforeMDivCols.push_back(getLineCol(MI.getDebugLoc()));
          BeforeMDivChars.push_back(getCharSrc(MI.getDebugLoc()));
        }
        // Check for memory operations
        if (MI.mayLoadOrStore() && !MI.isReturn() && !MI.isCall() && !MI.hasImplicitDef()) {
          // BeforeMMemInsts.push_back(getInstStr(MI));
          BeforeMMemSrcs.push_back(getLineSrc(MI.getDebugLoc()));
          BeforeMMemLines.push_back(getLineNumber(MI.getDebugLoc()));
          BeforeMMemCols.push_back(getLineCol(MI.getDebugLoc()));
          BeforeMMemChars.push_back(getCharSrc(MI.getDebugLoc()));
        }
      }
    }
    return true;
  }

  // After pass execution - collect current state
  SmallVector<std::string, 16> AfterCjumpInsts;
  SmallVector<std::string, 16> AfterCjumpSrcs;
  SmallVector<unsigned, 16> AfterCjumpLines;
  SmallVector<unsigned, 16> AfterCjumpCols;
  SmallVector<std::string, 16> AfterCjumpChars;
  SmallVector<std::string, 16> AfterMDivInsts;
  SmallVector<std::string, 16> AfterMDivSrcs;
  SmallVector<unsigned, 16> AfterMDivLines;
  SmallVector<unsigned, 16> AfterMDivCols;
  SmallVector<std::string, 16> AfterMDivChars;
  SmallVector<std::string, 16> AfterMMemInsts;
  SmallVector<std::string, 16> AfterMMemSrcs;
  SmallVector<unsigned, 16> AfterMMemLines;
  SmallVector<unsigned, 16> AfterMMemCols;
  SmallVector<std::string, 16> AfterMMemChars;

  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        // AfterCjumpInsts.push_back(getInstStr(MI));
        AfterCjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
        AfterCjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
        AfterCjumpCols.push_back(getLineCol(MI.getDebugLoc()));
        AfterCjumpChars.push_back(getCharSrc(MI.getDebugLoc()));
      }
      if (isDivisionMachineInstruction(MI)) {
        // AfterMDivInsts.push_back(getInstStr(MI));
        AfterMDivSrcs.push_back(getLineSrc(MI.getDebugLoc()));
        AfterMDivLines.push_back(getLineNumber(MI.getDebugLoc()));
        AfterMDivCols.push_back(getLineCol(MI.getDebugLoc()));
        AfterMDivChars.push_back(getCharSrc(MI.getDebugLoc()));
      }
      if (MI.mayLoadOrStore() && !MI.isReturn() && !MI.isCall() && !MI.hasImplicitDef()) {
        // AfterMMemInsts.push_back(getInstStr(MI));
        AfterMMemSrcs.push_back(getLineSrc(MI.getDebugLoc()));
        AfterMMemLines.push_back(getLineNumber(MI.getDebugLoc()));
        AfterMMemCols.push_back(getLineCol(MI.getDebugLoc()));
        AfterMMemChars.push_back(getCharSrc(MI.getDebugLoc()));
      }
    }
  }

  // Find added and removed instructions using helper function
  SmallVector<std::string, 16> AddedCjumpInsts, RemovedCjumpInsts;
  SmallVector<unsigned, 16> AddedCjumpLines, RemovedCjumpLines;
  SmallVector<unsigned, 16> AddedCjumpCols, RemovedCjumpCols;
  SmallVector<std::string, 16> AddedCjumpSrcs, RemovedCjumpSrcs;
  SmallVector<std::string, 16> AddedCjumpChars, RemovedCjumpChars;

  SmallVector<std::string, 16> AddedMDivInsts, RemovedMDivInsts;
  SmallVector<unsigned, 16> AddedMDivLines, RemovedMDivLines;
  SmallVector<unsigned, 16> AddedMDivCols, RemovedMDivCols;
  SmallVector<std::string, 16> AddedMDivSrcs, RemovedMDivSrcs;
  SmallVector<std::string, 16> AddedMDivChars, RemovedMDivChars;

  SmallVector<std::string, 16> AddedMMemInsts, RemovedMMemInsts;
  SmallVector<unsigned, 16> AddedMMemLines, RemovedMMemLines;
  SmallVector<unsigned, 16> AddedMMemCols, RemovedMMemCols;
  SmallVector<std::string, 16> AddedMMemSrcs, RemovedMMemSrcs;
  SmallVector<std::string, 16> AddedMMemChars, RemovedMMemChars;

  // Find added/removed conditional jumps
  findInstructionDifferences(BeforeCjumpInsts, BeforeCjumpLines, BeforeCjumpCols, BeforeCjumpSrcs, BeforeCjumpChars,
                            AfterCjumpInsts, AfterCjumpLines, AfterCjumpCols, AfterCjumpSrcs, AfterCjumpChars,
                            AddedCjumpLines, AddedCjumpInsts, AddedCjumpSrcs, AddedCjumpCols, AddedCjumpChars,
                            RemovedCjumpLines, RemovedCjumpInsts, RemovedCjumpSrcs, RemovedCjumpCols, RemovedCjumpChars);

  // Find added/removed divisions
  findInstructionDifferences(BeforeMDivInsts, BeforeMDivLines, BeforeMDivCols, BeforeMDivSrcs, BeforeMDivChars,
                            AfterMDivInsts, AfterMDivLines, AfterMDivCols, AfterMDivSrcs, AfterMDivChars,
                            AddedMDivLines, AddedMDivInsts, AddedMDivSrcs, AddedMDivCols, AddedMDivChars,
                            RemovedMDivLines, RemovedMDivInsts, RemovedMDivSrcs, RemovedMDivCols, RemovedMDivChars);

  // Find added/removed memory operations
  findInstructionDifferences(BeforeMMemInsts, BeforeMMemLines, BeforeMMemCols, BeforeMMemSrcs, BeforeMMemChars,
                            AfterMMemInsts, AfterMMemLines, AfterMMemCols, AfterMMemSrcs, AfterMMemChars,
                            AddedMMemLines, AddedMMemInsts, AddedMMemSrcs, AddedMMemCols, AddedMMemChars,
                            RemovedMMemLines, RemovedMMemInsts, RemovedMMemSrcs, RemovedMMemCols, RemovedMMemChars);

  // Only print if there were changes
  if (!AddedCjumpLines.empty() || !RemovedCjumpLines.empty() || 
      !AddedMDivLines.empty() || !RemovedMDivLines.empty() ||
      !AddedMMemLines.empty() || !RemovedMMemLines.empty()) {
    // Build the complete JSON string in memory before outputting
    std::string JsonOutput;
    raw_string_ostream JsonStream(JsonOutput);
    
    JsonStream << "{"
      << "\"function\": \"" << BeforeFunction << "\","
      << "\"file\": \"" << BeforeFileName << "\","
      << "\"context\": \"" << BeforeContext << "\","
      // Conditional jump stats 
      << "\"cjump_count_before\": " << BeforeCjumpInsts.size() << ","
      << "\"cjump_count_after\": " << AfterCjumpInsts.size() << ","
      << "\"removed_cjump_count\": " << RemovedCjumpLines.size() << ","
      << "\"added_cjump_count\": " << AddedCjumpLines.size();
    // Removed conditional jumps
    if (!RemovedCjumpLines.empty()) {
      JsonStream << ",\"removed_cjump_lines\": [" << join(RemovedCjumpLines) << "]"
      << ",\"removed_cjump_cols\": [" << join(RemovedCjumpCols) << "]"
      << ",\"removed_cjump_insts\": [" << join(RemovedCjumpInsts) << "]"
      << ",\"removed_cjump_srcs\": [" << join(RemovedCjumpSrcs) << "]"
      << ",\"removed_cjump_chars\": [" << join(RemovedCjumpChars) << "]";
    }
    // Added conditional jumps  
    if (!AddedCjumpLines.empty()) {
      JsonStream << ",\"added_cjump_lines\": [" << join(AddedCjumpLines) << "]"
      << ",\"added_cjump_cols\": [" << join(AddedCjumpCols) << "]"
      << ",\"added_cjump_insts\": [" << join(AddedCjumpInsts) << "]"
      << ",\"added_cjump_srcs\": [" << join(AddedCjumpSrcs) << "]"
      << ",\"added_cjump_chars\": [" << join(AddedCjumpChars) << "]";
    }
    // Division instruction stats
    JsonStream << ",\"mdiv_count_before\": " << BeforeMDivInsts.size() << ","
      << "\"mdiv_count_after\": " << AfterMDivInsts.size() << ","
      << "\"removed_mdiv_count\": " << RemovedMDivLines.size() << ","
      << "\"added_mdiv_count\": " << AddedMDivLines.size();
    // Removed division instructions
    if (!RemovedMDivLines.empty()) {
      JsonStream << ",\"removed_mdiv_lines\": [" << join(RemovedMDivLines) << "]"
      << ",\"removed_mdiv_cols\": [" << join(RemovedMDivCols) << "]"
      << ",\"removed_mdiv_insts\": [" << join(RemovedMDivInsts) << "]"
      << ",\"removed_mdiv_srcs\": [" << join(RemovedMDivSrcs) << "]"
      << ",\"removed_mdiv_chars\": [" << join(RemovedMDivChars) << "]";
    }
    // Added division instructions 
    if (!AddedMDivLines.empty()) {
      JsonStream << ",\"added_mdiv_lines\": [" << join(AddedMDivLines) << "]"
      << ",\"added_mdiv_cols\": [" << join(AddedMDivCols) << "]"
      << ",\"added_mdiv_insts\": [" << join(AddedMDivInsts) << "]"
      << ",\"added_mdiv_srcs\": [" << join(AddedMDivSrcs) << "]"
      << ",\"added_mdiv_chars\": [" << join(AddedMDivChars) << "]";
    }
    // Memory operation stats
    JsonStream << ",\"mmem_count_before\": " << BeforeMMemInsts.size() << ","
      << "\"mmem_count_after\": " << AfterMMemInsts.size() << ","
      << "\"removed_mmem_count\": " << RemovedMMemLines.size() << ","
      << "\"added_mmem_count\": " << AddedMMemLines.size();
    // Removed memory operations
    if (!RemovedMMemLines.empty()) {
      JsonStream << ",\"removed_mmem_lines\": [" << join(RemovedMMemLines) << "]"
      << ",\"removed_mmem_cols\": [" << join(RemovedMMemCols) << "]"
      << ",\"removed_mmem_insts\": [" << join(RemovedMMemInsts) << "]"
      << ",\"removed_mmem_srcs\": [" << join(RemovedMMemSrcs) << "]"
      << ",\"removed_mmem_chars\": [" << join(RemovedMMemChars) << "]";
    }
    // Added memory operations
    if (!AddedMMemLines.empty()) {
      JsonStream << ",\"added_mmem_lines\": [" << join(AddedMMemLines) << "]"
      << ",\"added_mmem_cols\": [" << join(AddedMMemCols) << "]"
      << ",\"added_mmem_insts\": [" << join(AddedMMemInsts) << "]"
      << ",\"added_mmem_srcs\": [" << join(AddedMMemSrcs) << "]"
      << ",\"added_mmem_chars\": [" << join(AddedMMemChars) << "]";
    }
    JsonStream << "}\n";
    
    // Output the complete JSON string - no lock needed as each write to errs() is atomic
    errs() << JsonOutput;
    
    return true;
  }
  
  // No changes to cjumps, divisions, or memory operations
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

  if (EnableMFPassDump) {
    dumpInsts(MF, getPassName(), true);
  }
  bool RV = runOnMachineFunction(MF);
  if (EnableMFPassDump) {
    dumpInsts(MF, getPassName(), false);
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
