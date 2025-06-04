#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/PrintPasses.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include <llvm/Support/FileSystem.h>

using namespace llvm;

#define DEBUG_TYPE "mf-count-instr"

static cl::opt<bool>
  EnableCountInstructions("enable-mf-count-instr",
            cl::desc("Enable MachineFunction count instr."),
            cl::init(false), cl::Hidden);

static cl::opt<std::string>
  DumpDirectory("mf-count-instr-dump-dir",
            cl::desc("Directory to dump IR and MIR files only if specified."),
            cl::value_desc("directory"),
            cl::init(""), cl::Hidden);

namespace {

/// MachineFunctionCountInstr - This is a architecture-independent
/// pass to dump cjump instructions of a MachineFunction.
///
class MFCountInstructions : public MachineFunctionPass {
public:
  MFCountInstructions() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "MFCountInstr"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Pass identification, replacement for typeid.
  static char ID;
};
} // end anonymous namespace

using namespace myutils;

char MFCountInstructions::ID = 0;

// Run on each MachineFunction
bool MFCountInstructions::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  if (!isFunctionInPrintList(MF.getName()))
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
  // SmallVector<const Instruction*, 16> CondInsts;  // IR conditional branches
  SmallVector<const MachineInstr*, 16> CjumpInsts;  // MIR conditional branches
  // SmallVector<const Instruction*, 16> DivInsts;  // IR division instructions
  SmallVector<const MachineInstr*, 16> MDivInsts;  // MIR division instructions
  // SmallVector<const Instruction*, 16> MemInsts;  // IR memory operations
  SmallVector<const MachineInstr*, 16> MMemInsts;  // MIR memory operations
  // SmallVector<std::string, 16> CondSrcs;
  SmallVector<std::string, 16> CjumpSrcs;
  // SmallVector<std::string, 16> DivSrcs;
  SmallVector<std::string, 16> MDivSrcs;
  // SmallVector<std::string, 16> MemSrcs;
  SmallVector<std::string, 16> MMemSrcs;
  // SmallVector<unsigned, 16> CondLines;
  SmallVector<unsigned, 16> CjumpLines;
  SmallVector<unsigned, 16> CjumpCols;
  // SmallVector<unsigned, 16> DivLines;
  SmallVector<unsigned, 16> MDivLines;
  SmallVector<unsigned, 16> MDivCols;
  // SmallVector<unsigned, 16> MemLines;
  SmallVector<unsigned, 16> MMemLines;
  SmallVector<unsigned, 16> MMemCols;
  // SmallVector<std::string, 16> CondChars;
  SmallVector<std::string, 16> CjumpChars;
  // SmallVector<std::string, 16> DivChars;
  SmallVector<std::string, 16> MDivChars;
  // SmallVector<std::string, 16> MemChars;
  SmallVector<std::string, 16> MMemChars;

  // Add vectors for memory operations

  // Iterate through the LLVM IR to count branch, divisions, and memory operations
  const Function &F = MF.getFunction();
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (!I.getDebugLoc()) continue;
      // if (isa<SelectInst>(I)) {
      // if (isa<BranchInst>(I) && cast<BranchInst>(I).isConditional()) {
      //   CondInsts.push_back(&I);
      //   CondLines.push_back(getLineNumber(I.getDebugLoc()));
      //   CondSrcs.push_back(getLineSrc(I.getDebugLoc()));
      //   CondChars.push_back(getCharSrc(I.getDebugLoc()));
      //   continue;
      // }
      // IR division instructions (sdiv, udiv, fdiv), but not needed
      // if (auto *BinOp = dyn_cast<BinaryOperator>(&I)) {
      //   switch (BinOp->getOpcode()) {
      //   case Instruction::SDiv:
      //   case Instruction::UDiv:
      //   case Instruction::FDiv:
      //   case Instruction::URem:
      //   case Instruction::SRem:
      //   case Instruction::FRem:
      //     DivInsts.push_back(&I);
      //     DivLines.push_back(getLineNumber(I.getDebugLoc()));
      //     DivSrcs.push_back(getLineSrc(I.getDebugLoc()));
      //     DivChars.push_back(getCharSrc(I.getDebugLoc()));
      //   }
      // }
      // Detect memory operations in IR
      // if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
      // || isa<AtomicRMWInst>(&I) || isa<AtomicCmpXchgInst>(&I) || isa<GetElementPtrInst>(&I)) {
      //   MemInsts.push_back(&I);
      //   MemLines.push_back(getLineNumber(I.getDebugLoc()));
      //   MemSrcs.push_back(getLineSrc(I.getDebugLoc()));
      //   MemChars.push_back(getCharSrc(I.getDebugLoc()));
      // }
    }
  }

  // Iterate through all MachineBasicBlocks
  for (const MachineBasicBlock &MBB : MF) {
    // Iterate through all instructions in the block
    for (const MachineInstr &MI : MBB) {
      if (!MI.getDebugLoc()) continue;
      // Check if the instruction is a conditional jump (e.g., X86 jcc)
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        CjumpInsts.push_back(&MI);
        const auto DL = MI.getDebugLoc();
        CjumpLines.push_back(getLineNumber(DL));
        CjumpSrcs.push_back(getLineSrc(DL));
        CjumpCols.push_back(getLineCol(DL));
        CjumpChars.push_back(getCharSrc(DL));
      }
      // Detect division instructions in Machine IR
      if (isDivisionMachineInstruction(MI)) {
        MDivInsts.push_back(&MI);
        const auto DL = MI.getDebugLoc();
        MDivLines.push_back(getLineNumber(DL));
        MDivSrcs.push_back(getLineSrc(DL));
        MDivCols.push_back(getLineCol(DL));
        MDivChars.push_back(getCharSrc(DL));
      }
      // Detect memory operations in Machine IR
      if (MI.mayLoadOrStore()) {
        MMemInsts.push_back(&MI);
        const auto DL = MI.getDebugLoc();
        MMemLines.push_back(getLineNumber(DL));
        MMemSrcs.push_back(getLineSrc(DL));
        MMemCols.push_back(getLineCol(DL));
        MMemChars.push_back(getCharSrc(DL));
      }
    }
  }

  // Build the complete JSON string in memory before outputting
  std::string JsonOutput;
  raw_string_ostream JsonStream(JsonOutput);
  JsonStream << "{"
           << "\"function\": \"" << MF.getName() << "\", "
           << "\"file\": \"" << FileName << "\", "
           << "\"context\": \"" << getPassName() << "\", "
          // NOTE: will be printed in IRCountInstr instead
          //  << "\"cond_count\": " << CondInsts.size() << ", "
          //  << "\"cond_lines\": [" << join(CondLines) << "], "
          //  << "\"cond_insts\": [" << join(CondInsts) << "], "
          //  << "\"cond_srcs\": [" << join(CondSrcs) << "], "
          //  << "\"cond_chars\": [" << join(CondChars) << "], "
           << "\"cjump_count\": " << CjumpInsts.size() << ", "
           << "\"cjump_lines\": [" << join(CjumpLines) << "], "
           << "\"cjump_cols\": [" << join(CjumpCols) << "], "
           << "\"cjump_insts\": [" << join(CjumpInsts) << "], "
           << "\"cjump_srcs\": [" << join(CjumpSrcs) << "], "
           << "\"cjump_chars\": [" << join(CjumpChars) << "], "
           << "\"mdiv_count\": " << MDivInsts.size() << ", "
           << "\"mdiv_lines\": [" << join(MDivLines) << "], "
           << "\"mdiv_cols\": [" << join(MDivCols) << "], "
           << "\"mdiv_insts\": [" << join(MDivInsts) << "], "
           << "\"mdiv_srcs\": [" << join(MDivSrcs) << "], "
           << "\"mdiv_chars\": [" << join(MDivChars) << "], "
          //  << "\"mem_count\": " << MemInsts.size() << ", "
          //  << "\"mem_lines\": [" << join(MemLines) << "], "
          //  << "\"mem_insts\": [" << join(MemInsts) << "], "
          //  << "\"mem_srcs\": [" << join(MemSrcs) << "], "
          //  << "\"mem_chars\": [" << join(MemChars) << "], "
           << "\"mmem_count\": " << MMemInsts.size() << ", "
           << "\"mmem_lines\": [" << join(MMemLines) << "], "
           << "\"mmem_cols\": [" << join(MMemCols) << "], "
           << "\"mmem_insts\": [" << join(MMemInsts) << "], "
           << "\"mmem_srcs\": [" << join(MMemSrcs) << "], "
           << "\"mmem_chars\": [" << join(MMemChars) << "]"
           << "}\n";
  
  // Output JSON to stderr, which is unbuffered by default so no need to flush
  // Inter-Process output interleaving is prevented on build-system level (eg. make --output-sync)
  errs() << JsonOutput;

  // Only dump function and machine function if a dump directory is specified
  if (!DumpDirectory.empty()) {
    // Create dump directory if it doesn't exist
    sys::fs::create_directories(DumpDirectory);

    // Create unique file name = FunctionName-FileName.dump, FileName should not contain '/'
    std::string DumpFileStr = DumpDirectory + "/" + MF.getName().str() + "-" + 
                             FileName.rsplit('/').second.str() + ".dump";
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
  }

  return false;
}

// Required analysis information
void MFCountInstructions::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
}

INITIALIZE_PASS(MFCountInstructions, DEBUG_TYPE,
                "Count Conditional Move Instructions", false, false)

MachineFunctionPass *llvm::createMFCountInstructionsPass() {
  return new MFCountInstructions();
}
