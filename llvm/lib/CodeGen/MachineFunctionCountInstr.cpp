#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
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
  SmallVector<const MachineInstr*, 16> CjumpInsts;
  SmallVector<std::string, 16> SelectSrcs;
  SmallVector<std::string, 16> CjumpSrcs;
  SmallVector<unsigned, 16> SelectLines;
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
      // Check if the instruction is a conditional jump (e.g., X86 jcc)
      if (MI.getDesc().isBranch() && MI.getDesc().isConditionalBranch()) {
        CjumpInsts.push_back(&MI);
        CjumpLines.push_back(getLineNumber(MI.getDebugLoc()));
        CjumpSrcs.push_back(getLineSrc(MI.getDebugLoc()));
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
           << "\"select_count\": " << SelectInsts.size() << ", "
           << "\"select_lines\": [" << join(SelectLines) << "], "
           << "\"select_insts\": [" << join(SelectInsts) << "], "
           << "\"select_srcs\": [" << join(SelectSrcs) << "], "
           << "\"cjump_count\": " << CjumpInsts.size() << ", "
           << "\"cjump_lines\": [" << join(CjumpLines) << "], "
           << "\"cjump_insts\": [" << join(CjumpInsts) << "], "
           << "\"cjump_srcs\": [" << join(CjumpSrcs) << "]"
           << "}\n";
  
  // Flush the stream to ensure all content is in the string
  JsonStream.flush();
  
  // Output the complete JSON string - no lock needed as each write to errs() is atomic
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
