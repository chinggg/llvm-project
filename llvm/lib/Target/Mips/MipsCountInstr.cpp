#include "Mips.h"
#include "MipsInstrInfo.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FileSystem.h>
#include <string>

using namespace llvm;

#define DEBUG_TYPE "mips-count-instr"

static cl::opt<bool>
  EnableCountInstructions("enable-mips-count-instr",
            cl::desc("Enable the MIPS count instr."),
            cl::init(false), cl::Hidden);

namespace {
class MipsCountInstructions : public MachineFunctionPass {
public:
  MipsCountInstructions() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "MipsCountInstr"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool isCmovInstruction(const MachineInstr &MI) const;

  static char ID;
};
} // end anonymous namespace

using namespace myutils;

char MipsCountInstructions::ID = 0;

bool MipsCountInstructions::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  if (!EnableCountInstructions)
    return false;
  if (isNameTrivial(MF.getName()))
    return false;

  // Get the source file name
  auto *SP = MF.getFunction().getSubprogram();
  if (!SP) return false;  // no debug info available if not compiled with -g
  auto FileName = SP->getFilename();
  if (isNameTrivial(FileName))
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
      if (!I.getDebugLoc()) continue;
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
      if (!MI.getDebugLoc()) continue;
      // Check if the instruction is a conditional move
      if (isCmovInstruction(MI)) {
        CmovInsts.push_back(&MI);
        CmovLines.push_back(getLineNumber(MI.getDebugLoc()));
        CmovSrcs.push_back(getLineSrc(MI.getDebugLoc()));
      }
      // Check if the instruction is a conditional jump
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        CjumpInsts.push_back(&MI);
        CjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
        CjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
      }
    }
  }

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
  return false;
}

bool MipsCountInstructions::isCmovInstruction(const MachineInstr &MI) const {
  // Check for MIPS select instructions which implement conditional moves
  // NOTE: not sure if these are really "conditional move" in Mips
  // see: https://www.cs.cmu.edu/afs/cs/academic/class/15740-f97/public/doc/mips-isa.pdf
  auto opcode = MI.getOpcode();
  if (opcode >= Mips::MOVF_D32 && opcode <= Mips::MOVZ_I_S_MM) {
    return true;
  }
  return false;
}

void MipsCountInstructions::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
}

INITIALIZE_PASS(MipsCountInstructions, DEBUG_TYPE,
                "Count Conditional Move Instructions", false, false)

FunctionPass *llvm::createMipsCountInstructionsPass() {
  return new MipsCountInstructions();
}
