#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
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

// Helper for LLVM StringRef prefix check (API changed in LLVM 16)
inline bool str_startswith_insensitive(llvm::StringRef S, llvm::StringRef Prefix) {
#if LLVM_VERSION_MAJOR <= 15
  return S.startswith_insensitive(Prefix);
#else
  return S.starts_with_insensitive(Prefix);
#endif
}

/// Check if a memory instruction uses the specified register as an addressing register
/// Returns true if the register is found in the addressing mode
static bool MemInstUseRegAsAddr(const MachineInstr &MI, Register Reg) {
  // Get target instruction and register info from the MachineInstr
  const MachineFunction *MF = MI.getMF();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
  const TargetRegisterInfo *TRI = MF->getSubtarget().getRegisterInfo();

  // First try to get AddrMode, currently implemented only on X86/AArch64
  auto AM = TII->getAddrModeFromMemoryOp(MI, TRI);
  if (AM) {
    const Register BaseReg = AM->BaseReg, ScaledReg = AM->ScaledReg;
    return (BaseReg == Reg || ScaledReg == Reg);
  } else {  // Fallback to check each operand register
    for (const MachineOperand &MO : MI.explicit_uses()) {
      if (MO.isReg() && MO.isUse() && MO.getReg() == Reg) {
        // NOTE: over-approximate here as we don't know if reg used as address
        return true;
      }
    }
  }
  return false;
}

// A helper function to check if a machine instruction is a conditional move.
static inline bool isConditionalMove(const MachineInstr &MI) {
  // NOTE: target-specific header is not included here, have to use generic MIR opcode
  
  // Check if the instruction is a copy/move but not an unconditional one
  if (MI.isCopy())
    return false; // COPY is an unconditional move
    
  // If it's a branch or jump, it's not a conditional move
  if (MI.getDesc().isBranch() || MI.getDesc().isIndirectBranch())
    return false;
    
  bool NameHintsCmov = false;
  // Check if the name contains typical conditional move substrings
  // Support multiple architectures: X86, AArch64, ARM, MIPS
  if (isTargetSpecificOpcode(MI.getOpcode())) {
    const TargetInstrInfo *TII = MI.getMF()->getSubtarget().getInstrInfo();
    const Triple &TT = MI.getMF()->getSubtarget().getTargetTriple();
    StringRef OpcodeName = TII->getName(MI.getOpcode());

    // X86: cmov instructions
    if (TT.isX86() && str_startswith_insensitive(OpcodeName, "cmov")) {
      NameHintsCmov = true;
    }
    // AArch64: csel (conditional select), csinc, csinv, csneg
    else if (TT.isAArch64() && (str_startswith_insensitive(OpcodeName, "csel") ||
             str_startswith_insensitive(OpcodeName, "csinc") ||
             str_startswith_insensitive(OpcodeName, "csinv") ||
             str_startswith_insensitive(OpcodeName, "csneg"))) {
      NameHintsCmov = true;
    }
    // MIPS: movn, movz, movf, movt variants (MOVN_*, MOVZ_*, etc.)
    else if (TT.isMIPS() && (str_startswith_insensitive(OpcodeName, "movn") ||
             str_startswith_insensitive(OpcodeName, "movz") || 
             str_startswith_insensitive(OpcodeName, "movf") ||
             str_startswith_insensitive(OpcodeName, "movt"))) {
      NameHintsCmov = true;
    }
    // ARM: MOVCC variants (MOVCCr, MOVCCi, MOVCCsi, MOVCCsr)
    else if (TT.isARM() && str_startswith_insensitive(OpcodeName, "mov")) {
      // sometimes opcode already contains "MOVCC"
      if (str_startswith_insensitive(OpcodeName, "movcc")) NameHintsCmov = true;
      else if (str_startswith_insensitive(OpcodeName, "movr") || str_startswith_insensitive(OpcodeName, "movs")) {
        // sometimes opcode is normal "MOVR", check "killed $cpsr" in MI string representation
        std::string str;
        raw_string_ostream ss(str);
        ss << MI;
        if (str.find("killed $cpsr") != std::string::npos) NameHintsCmov = true;
      }
    }
    // Additional patterns: any instruction with "select" in the name (generic)
    else if (OpcodeName.contains_insensitive("select")) {
      NameHintsCmov = true;
    }
  }
  
  // Return true if it looks like a conditional move
  return NameHintsCmov;
}

/// Identifies conditional moves that are used by memory operations
/// Returns the memory instruction that uses the register defined by the cmov as reg, or nullptr if none found
/* example pattern
  CMOV REG1, REG2 
  MOV xx, [REG1] */
static const MachineInstr* findMemInstUsingCmov(const MachineInstr &CmovMI, const MachineBasicBlock &MBB) {
  // Find the register defined by this cmov
  Register DefReg;
  for (const MachineOperand &MO : CmovMI.defs()) {
    if (MO.isReg() && MO.isDef() && MO.getReg()) {
      DefReg = MO.getReg();
      break;
    }
  }
  
  if (!DefReg)
    return nullptr;
  
  // Flag to indicate we've found the cmov instruction
  bool foundCmov = false;
  // Check if any memory operation AFTER this cmov uses this register
  for (const MachineInstr &MI : MBB) {
    // Skip instructions until we find the cmov
    if (&MI == &CmovMI) {
      foundCmov = true;
      continue; // Skip the cmov itself
    }
    // Only check instructions that come after the cmov
    if (!foundCmov)
      continue;

    if (!MI.getDebugLoc() || !MI.mayLoadOrStore())
      continue;
    // filter FP as mayLoadOrStore can still keep non-memory instructions like bne branch in MIPS
    if (MI.isReturn() || MI.isCall() || MI.isBranch() || MI.isPseudo() || MI.getFlag(llvm::MachineInstr::FrameDestroy))
      continue;
    // Check if this memory instruction uses the cmov-defined register as address
    if (MemInstUseRegAsAddr(MI, DefReg)) {
      return &MI; // Found a memory op using this register
    }
  }
  
  return nullptr;
}

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
  SmallVector<const MachineInstr*, 16> CmovInsts;  // MIR conditional moves
  // SmallVector<std::string, 16> CondSrcs;
  SmallVector<std::string, 16> CjumpSrcs;
  // SmallVector<std::string, 16> DivSrcs;
  SmallVector<std::string, 16> MDivSrcs;
  // SmallVector<std::string, 16> MemSrcs;
  SmallVector<std::string, 16> MMemSrcs;
  SmallVector<std::string, 16> CmovSrcs;
  // SmallVector<unsigned, 16> CondLines;
  SmallVector<unsigned, 16> CjumpLines;
  SmallVector<unsigned, 16> CjumpCols;
  // SmallVector<unsigned, 16> DivLines;
  SmallVector<unsigned, 16> MDivLines;
  SmallVector<unsigned, 16> MDivCols;
  // SmallVector<unsigned, 16> MemLines;
  SmallVector<unsigned, 16> MMemLines;
  SmallVector<unsigned, 16> MMemCols;
  SmallVector<unsigned, 16> CmovLines;
  SmallVector<unsigned, 16> CmovCols;
  // SmallVector<std::string, 16> CondChars;
  SmallVector<std::string, 16> CjumpChars;
  // SmallVector<std::string, 16> DivChars;
  SmallVector<std::string, 16> MDivChars;
  // SmallVector<std::string, 16> MemChars;
  SmallVector<std::string, 16> MMemChars;
  SmallVector<std::string, 16> CmovChars;

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
      
      // Now we first find cmove instructions and only collect memory operations that use cmov-defined registers
      if (isConditionalMove(MI)) {
        const MachineInstr *MemMI = findMemInstUsingCmov(MI, *MI.getParent());
        if (!MemMI)
          continue; // Skip cmovs not used by memory operations
        
        // Store the cmov instruction
        CmovInsts.push_back(&MI);
        const auto DL = MI.getDebugLoc();
        CmovLines.push_back(getLineNumber(DL));
        CmovSrcs.push_back(getLineSrc(DL));
        CmovCols.push_back(getLineCol(DL));
        CmovChars.push_back(getCharSrc(DL));
        
        // Store the memory instruction that uses the cmov-defined register
        // NOTE: there are some FP where mmem inst use cmov-defined register much later
        // we can actually check if cmov/mmem insts are in same source line
        MMemInsts.push_back(MemMI);
        const auto MemDL = MemMI->getDebugLoc();
        MMemLines.push_back(getLineNumber(MemDL));
        MMemSrcs.push_back(getLineSrc(MemDL));
        MMemCols.push_back(getLineCol(MemDL));
        MMemChars.push_back(getCharSrc(MemDL));
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
           << "\"cmov_count\": " << CmovInsts.size() << ", "
           << "\"cmov_lines\": [" << join(CmovLines) << "], "
           << "\"cmov_cols\": [" << join(CmovCols) << "], "
           << "\"cmov_insts\": [" << join(CmovInsts) << "], "
           << "\"cmov_srcs\": [" << join(CmovSrcs) << "], "
           << "\"cmov_chars\": [" << join(CmovChars) << "], "
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
