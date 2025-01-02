#include "X86.h"
#include "X86InstrInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FileSystem.h>
#include <string>
#include <fstream>
#include <iomanip>

using namespace llvm;

#define DEBUG_TYPE "x86-count-instr"

static cl::opt<bool>
    EnableCountInstructions(DEBUG_TYPE,
                        cl::desc("Enable the X86 count instr."),
                        cl::init(false), cl::Hidden);

namespace {
class X86CountInstructions : public MachineFunctionPass {
public:
  X86CountInstructions() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "X86CountInstr"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  
  bool isCmovInstruction(const MachineInstr &MI) const;
  
  /// Pass identification, replacement for typeid.
  static char ID;
};
} // end anonymous namespace

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

// get source line from debugloc as string by reading file and specific line
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
  return SourceLine;
}

unsigned getLineNumber(const DebugLoc &DL) {
  if (DL) {
    return DL.getLine();
  }
  return 0;
}

char X86CountInstructions::ID = 0;

// Run on each MachineFunction
bool X86CountInstructions::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  if (!EnableCountInstructions)
    return false;

  // Get the source file name
  StringRef FileName;
  if (!MF.getFunction().getSubprogram())
    FileName = "unknown";
  else
    FileName = MF.getFunction().getSubprogram()->getScope()->getFilename();
  // skip if file contains "include/" or ends with ".h"
  if (FileName.contains("include/") || FileName.ends_with(".h"))
    return false;

  // Instruction counters and line numbers
  SmallVector<const Instruction*, 16> SelectInsts;
  SmallVector<const MachineInstr*, 16> CmovInsts;
  SmallVector<const MachineInstr*, 16> CjumpInsts;
  SmallVector<std::string, 16> SelectSrcs;
  SmallVector<std::string, 16> CmovSrcs;
  SmallVector<std::string, 16> CjumpSrcs;
  SmallVector<unsigned, 16> SelectLines;
  SmallVector<unsigned, 16> CmovLines;
  SmallVector<unsigned, 16> CjumpLines;

  // Iterate through the LLVM IR instructions to count SelectInst
  const Function &F = MF.getFunction();
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (isa<SelectInst>(&I)) {
        SelectInsts.push_back(&I);
        SelectLines.push_back(getLineNumber(I.getDebugLoc()));
        SelectSrcs.push_back(getLineSrc(I.getDebugLoc()));
      }
    }
  }

  // Iterate through all MachineBasicBlocks
  for (const MachineBasicBlock &MBB : MF) {
    // Iterate through all instructions in the block
    for (const MachineInstr &MI : MBB) {
      // Check if the instruction is a conditional move (e.g., X86 cmov)
      if (isCmovInstruction(MI)) {
        CmovInsts.push_back(&MI);
        CmovLines.push_back(getLineNumber(MI.getDebugLoc()));
        CmovSrcs.push_back(getLineSrc(MI.getDebugLoc()));
      }
      // Check if the instruction is a conditional jump (e.g., X86 jcc)
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        CjumpInsts.push_back(&MI);
        CjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
        CjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
      }
    }
  }

  // count unique SelectLines and CmovLines
  // if they are the same, optimization is safe, skip
  // SmallSetVector<unsigned, 16> UniqueSelectLines(SelectLines.begin(), SelectLines.end());
  // SmallSetVector<unsigned, 16> UniqueCmovLines(CmovLines.begin(), CmovLines.end());
  // if (UniqueSelectLines.size() == UniqueCmovLines.size()) {
  //   return false;
  // }

  // Print JSON like output
  errs() << "{"
        << "\"function\": \"" << MF.getName() << "\", "
        << "\"file\": \"" << FileName << "\", "
        << "\"context\": \"" << getPassName() << "\", "
        << "\"select_count\": " << SelectInsts.size() << ", "
        << "\"select_lines\": [" << join(SelectLines) << "], "
        << "\"select_insts\": [" << join(SelectInsts) << "], "
        << "\"select_srcs\": [" << join(SelectSrcs) << "], "
        << "\"cmov_count\": " << CmovInsts.size() << ", "
        << "\"cmov_lines\": [" << join(CmovLines) << "], "
        << "\"cmov_insts\": [" << join(CmovInsts) << "], "
        << "\"cmov_srcs\": [" << join(CmovSrcs) << "], "
        << "\"cjump_count\": " << CjumpInsts.size() << ", "
        << "\"cjump_lines\": [" <<join(CjumpLines) << "], "
        << "\"cjump_insts\": [" << join(CjumpInsts) << "], "
        << "\"cjump_srcs\": [" << join(CjumpSrcs) << "]"
        << "}\n";

  // dump full function only if CjumpLines or CmovLines are not empty
  // if (CjumpLines.empty() && CmovLines.empty()) {
  //   return false;
  // }

  // get DUMP_DIR from env, otherwise use $PWD/dump/
  const char *dump_dir = getenv("DUMP_DIR");
  if (!dump_dir) {
    dump_dir = "./dumps/";
  }
  // create dump_dir if it does not exist
  sys::fs::create_directories(dump_dir);

  // dump F and MF to dump_dir/filename.txt
  // create unique file name = FunctionName-FileName.dump, FileName should not contain '/'
  std::string DumpFileStr = std::string(dump_dir) + MF.getName().str() + "-" + FileName.rsplit('/').second.str() + ".dump";
  std::error_code EC;
  raw_fd_ostream DumpFile(DumpFileStr, EC);
  if (!EC) {
    DumpFile << "=== Dump Function: " << F.getName() << "\n";
    F.print(DumpFile);
    DumpFile << "=== Dump MachineFunction: " << MF.getName() << "\n";
    MF.print(DumpFile);
  } else {
    errs() << "Error opening file: " << DumpFileStr << " " << EC.message() << "\n";
  }

  // errs() << "=== Dump Function: " << F.getName() << "\n";
  // F.dump();
  // errs() << "=== Dump MachineFunction: " << MF.getName() << "\n";
  // MF.dump();

  return false;
}


// Identify if an instruction is a cmov (example for X86 target)
bool X86CountInstructions::isCmovInstruction(const MachineInstr &MI) const {
  // Check the opcode. Add other opcodes as necessary.
  auto opcode = MI.getOpcode();
  if (opcode >= X86::CMOV16rm && opcode <= X86::CMOV_VR64) {
    return true;
  }
  return false;
  // switch (MI.getOpcode()) {
  // case X86::CMOV32rr: // Example for X86 cmov32rr
  // case X86::CMOV64rr: // Example for X86 cmov64rr
  //   return true;
  // default:
  //   return false;
  // }
}

// Required analysis information
void X86CountInstructions::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
}

INITIALIZE_PASS(X86CountInstructions, DEBUG_TYPE,
                "Count Conditional Move Instructions", false, false)

FunctionPass *llvm::createX86CountInstructionsPass() {
  return new X86CountInstructions();
}
