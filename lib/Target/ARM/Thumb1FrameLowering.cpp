//===-- Thumb1FrameLowering.cpp - Thumb1 Frame Information ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the Thumb1 implementation of TargetFrameLowering class.
//
//===----------------------------------------------------------------------===//

#include "Thumb1FrameLowering.h"
#include "ARMMachineFunctionInfo.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

Thumb1FrameLowering::Thumb1FrameLowering(const ARMSubtarget &sti)
    : ARMFrameLowering(sti) {}

bool Thumb1FrameLowering::hasReservedCallFrame(const MachineFunction &MF) const{
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  unsigned CFSize = MFI.getMaxCallFrameSize();
  // It's not always a good idea to include the call frame as part of the
  // stack frame. ARM (especially Thumb) has small immediate offset to
  // address the stack frame. So a large call frame can cause poor codegen
  // and may even makes it impossible to scavenge a register.
  if (CFSize >= ((1 << 8) - 1) * 4 / 2) // Half of imm8 * 4
    return false;

  return !MFI.hasVarSizedObjects();
}

static void emitSPUpdate(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator &MBBI,
                         const TargetInstrInfo &TII, const DebugLoc &dl,
                         const ThumbRegisterInfo &MRI, int NumBytes,
                         unsigned MIFlags = MachineInstr::NoFlags) {
  emitThumbRegPlusImmediate(MBB, MBBI, dl, ARM::SP, ARM::SP, NumBytes, TII,
                            MRI, MIFlags);
}


MachineBasicBlock::iterator Thumb1FrameLowering::
eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I) const {
  const Thumb1InstrInfo &TII =
      *static_cast<const Thumb1InstrInfo *>(STI.getInstrInfo());
  const ThumbRegisterInfo *RegInfo =
      static_cast<const ThumbRegisterInfo *>(STI.getRegisterInfo());
  if (!hasReservedCallFrame(MF)) {
    // If we have alloca, convert as follows:
    // ADJCALLSTACKDOWN -> sub, sp, sp, amount
    // ADJCALLSTACKUP   -> add, sp, sp, amount
    MachineInstr &Old = *I;
    DebugLoc dl = Old.getDebugLoc();
    unsigned Amount = Old.getOperand(0).getImm();
    if (Amount != 0) {
      // We need to keep the stack aligned properly.  To do this, we round the
      // amount of space needed for the outgoing arguments up to the next
      // alignment boundary.
      unsigned Align = getStackAlignment();
      Amount = (Amount+Align-1)/Align*Align;

      // Replace the pseudo instruction with a new instruction...
      unsigned Opc = Old.getOpcode();
      if (Opc == ARM::ADJCALLSTACKDOWN || Opc == ARM::tADJCALLSTACKDOWN) {
        emitSPUpdate(MBB, I, TII, dl, *RegInfo, -Amount);
      } else {
        assert(Opc == ARM::ADJCALLSTACKUP || Opc == ARM::tADJCALLSTACKUP);
        emitSPUpdate(MBB, I, TII, dl, *RegInfo, Amount);
      }
    }
  }
  return MBB.erase(I);
}

void Thumb1FrameLowering::emitPrologue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  MachineBasicBlock::iterator MBBI = MBB.begin();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  MachineModuleInfo &MMI = MF.getMMI();
  const MCRegisterInfo *MRI = MMI.getContext().getRegisterInfo();
  const ThumbRegisterInfo *RegInfo =
      static_cast<const ThumbRegisterInfo *>(STI.getRegisterInfo());
  const Thumb1InstrInfo &TII =
      *static_cast<const Thumb1InstrInfo *>(STI.getInstrInfo());

  unsigned ArgRegsSaveSize = AFI->getArgRegsSaveSize();
  unsigned NumBytes = MFI.getStackSize();
  assert(NumBytes >= ArgRegsSaveSize &&
         "ArgRegsSaveSize is included in NumBytes");
  const std::vector<CalleeSavedInfo> &CSI = MFI.getCalleeSavedInfo();

  // Debug location must be unknown since the first debug location is used
  // to determine the end of the prologue.
  DebugLoc dl;
  
  unsigned FramePtr = RegInfo->getFrameRegister(MF);
  unsigned BasePtr = RegInfo->getBaseRegister();
  int CFAOffset = 0;

  // Thumb add/sub sp, imm8 instructions implicitly multiply the offset by 4.
  NumBytes = (NumBytes + 3) & ~3;
  MFI.setStackSize(NumBytes);

  // Determine the sizes of each callee-save spill areas and record which frame
  // belongs to which callee-save spill areas.
  unsigned GPRCS1Size = 0, GPRCS2Size = 0, DPRCSSize = 0;
  int FramePtrSpillFI = 0;

  if (ArgRegsSaveSize) {
    emitSPUpdate(MBB, MBBI, TII, dl, *RegInfo, -ArgRegsSaveSize,
                 MachineInstr::FrameSetup);
    CFAOffset -= ArgRegsSaveSize;
    unsigned CFIIndex = MMI.addFrameInst(
        MCCFIInstruction::createDefCfaOffset(nullptr, CFAOffset));
    BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
        .addCFIIndex(CFIIndex)
        .setMIFlags(MachineInstr::FrameSetup);
  }

  if (!AFI->hasStackFrame()) {
    if (NumBytes - ArgRegsSaveSize != 0) {
      emitSPUpdate(MBB, MBBI, TII, dl, *RegInfo, -(NumBytes - ArgRegsSaveSize),
                   MachineInstr::FrameSetup);
      CFAOffset -= NumBytes - ArgRegsSaveSize;
      unsigned CFIIndex = MMI.addFrameInst(
          MCCFIInstruction::createDefCfaOffset(nullptr, CFAOffset));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex)
          .setMIFlags(MachineInstr::FrameSetup);
    }
    return;
  }

  for (unsigned i = 0, e = CSI.size(); i != e; ++i) {
    unsigned Reg = CSI[i].getReg();
    int FI = CSI[i].getFrameIdx();
    switch (Reg) {
    case ARM::R8:
    case ARM::R9:
    case ARM::R10:
    case ARM::R11:
      if (STI.splitFramePushPop(MF)) {
        GPRCS2Size += 4;
        break;
      }
      LLVM_FALLTHROUGH;
    case ARM::R4:
    case ARM::R5:
    case ARM::R6:
    case ARM::R7:
    case ARM::LR:
      if (Reg == FramePtr)
        FramePtrSpillFI = FI;
      GPRCS1Size += 4;
      break;
    default:
      DPRCSSize += 8;
    }
  }

  if (MBBI != MBB.end() && MBBI->getOpcode() == ARM::tPUSH) {
    ++MBBI;
  }

  // Determine starting offsets of spill areas.
  unsigned DPRCSOffset  = NumBytes - ArgRegsSaveSize - (GPRCS1Size + GPRCS2Size + DPRCSSize);
  unsigned GPRCS2Offset = DPRCSOffset + DPRCSSize;
  unsigned GPRCS1Offset = GPRCS2Offset + GPRCS2Size;
  bool HasFP = hasFP(MF);
  if (HasFP)
    AFI->setFramePtrSpillOffset(MFI.getObjectOffset(FramePtrSpillFI) +
                                NumBytes);
  AFI->setGPRCalleeSavedArea1Offset(GPRCS1Offset);
  AFI->setGPRCalleeSavedArea2Offset(GPRCS2Offset);
  AFI->setDPRCalleeSavedAreaOffset(DPRCSOffset);
  NumBytes = DPRCSOffset;

  int FramePtrOffsetInBlock = 0;
  unsigned adjustedGPRCS1Size = GPRCS1Size;
  if (GPRCS1Size > 0 && GPRCS2Size == 0 &&
      tryFoldSPUpdateIntoPushPop(STI, MF, &*std::prev(MBBI), NumBytes)) {
    FramePtrOffsetInBlock = NumBytes;
    adjustedGPRCS1Size += NumBytes;
    NumBytes = 0;
  }

  if (adjustedGPRCS1Size) {
    CFAOffset -= adjustedGPRCS1Size;
    unsigned CFIIndex = MMI.addFrameInst(
        MCCFIInstruction::createDefCfaOffset(nullptr, CFAOffset));
    BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
        .addCFIIndex(CFIIndex)
        .setMIFlags(MachineInstr::FrameSetup);
  }
  for (std::vector<CalleeSavedInfo>::const_iterator I = CSI.begin(),
         E = CSI.end(); I != E; ++I) {
    unsigned Reg = I->getReg();
    int FI = I->getFrameIdx();
    switch (Reg) {
    case ARM::R8:
    case ARM::R9:
    case ARM::R10:
    case ARM::R11:
    case ARM::R12:
      if (STI.splitFramePushPop(MF))
        break;
      // fallthough
    case ARM::R0:
    case ARM::R1:
    case ARM::R2:
    case ARM::R3:
    case ARM::R4:
    case ARM::R5:
    case ARM::R6:
    case ARM::R7:
    case ARM::LR:
      unsigned CFIIndex = MMI.addFrameInst(MCCFIInstruction::createOffset(
          nullptr, MRI->getDwarfRegNum(Reg, true), MFI.getObjectOffset(FI)));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex)
          .setMIFlags(MachineInstr::FrameSetup);
      break;
    }
  }

  // Adjust FP so it point to the stack slot that contains the previous FP.
  if (HasFP) {
    FramePtrOffsetInBlock +=
        MFI.getObjectOffset(FramePtrSpillFI) + GPRCS1Size + ArgRegsSaveSize;
    AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tADDrSPi), FramePtr)
      .addReg(ARM::SP).addImm(FramePtrOffsetInBlock / 4)
      .setMIFlags(MachineInstr::FrameSetup));
    if(FramePtrOffsetInBlock) {
      CFAOffset += FramePtrOffsetInBlock;
      unsigned CFIIndex = MMI.addFrameInst(MCCFIInstruction::createDefCfa(
          nullptr, MRI->getDwarfRegNum(FramePtr, true), CFAOffset));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex)
          .setMIFlags(MachineInstr::FrameSetup);
    } else {
      unsigned CFIIndex =
          MMI.addFrameInst(MCCFIInstruction::createDefCfaRegister(
              nullptr, MRI->getDwarfRegNum(FramePtr, true)));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex)
          .setMIFlags(MachineInstr::FrameSetup);
    }
    if (NumBytes > 508)
      // If offset is > 508 then sp cannot be adjusted in a single instruction,
      // try restoring from fp instead.
      AFI->setShouldRestoreSPFromFP(true);
  }

  // Skip past the spilling of r8-r11, which could consist of multiple tPUSH
  // and tMOVr instructions. We don't need to add any call frame information
  // in-between these instructions, because they do not modify the high
  // registers.
  while (true) {
    MachineBasicBlock::iterator OldMBBI = MBBI;
    // Skip a run of tMOVr instructions
    while (MBBI != MBB.end() && MBBI->getOpcode() == ARM::tMOVr)
      MBBI++;
    if (MBBI != MBB.end() && MBBI->getOpcode() == ARM::tPUSH) {
      MBBI++;
    } else {
      // We have reached an instruction which is not a push, so the previous
      // run of tMOVr instructions (which may have been empty) was not part of
      // the prologue. Reset MBBI back to the last PUSH of the prologue.
      MBBI = OldMBBI;
      break;
    }
  }

  // Emit call frame information for the callee-saved high registers.
  for (auto &I : CSI) {
    unsigned Reg = I.getReg();
    int FI = I.getFrameIdx();
    switch (Reg) {
    case ARM::R8:
    case ARM::R9:
    case ARM::R10:
    case ARM::R11:
    case ARM::R12: {
      unsigned CFIIndex = MMI.addFrameInst(MCCFIInstruction::createOffset(
          nullptr, MRI->getDwarfRegNum(Reg, true), MFI.getObjectOffset(FI)));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex)
          .setMIFlags(MachineInstr::FrameSetup);
      break;
    }
    default:
      break;
    }
  }

  if (NumBytes) {
    // Insert it after all the callee-save spills.
    emitSPUpdate(MBB, MBBI, TII, dl, *RegInfo, -NumBytes,
                 MachineInstr::FrameSetup);
    if (!HasFP) {
      CFAOffset -= NumBytes;
      unsigned CFIIndex = MMI.addFrameInst(
          MCCFIInstruction::createDefCfaOffset(nullptr, CFAOffset));
      BuildMI(MBB, MBBI, dl, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(CFIIndex)
          .setMIFlags(MachineInstr::FrameSetup);
    }
  }

  if (STI.isTargetELF() && HasFP)
    MFI.setOffsetAdjustment(MFI.getOffsetAdjustment() -
                            AFI->getFramePtrSpillOffset());

  AFI->setGPRCalleeSavedArea1Size(GPRCS1Size);
  AFI->setGPRCalleeSavedArea2Size(GPRCS2Size);
  AFI->setDPRCalleeSavedAreaSize(DPRCSSize);

  // Thumb1 does not currently support dynamic stack realignment.  Report a
  // fatal error rather then silently generate bad code.
  if (RegInfo->needsStackRealignment(MF))
      report_fatal_error("Dynamic stack realignment not supported for thumb1.");

  // If we need a base pointer, set it up here. It's whatever the value
  // of the stack pointer is at this point. Any variable size objects
  // will be allocated after this, so we can still use the base pointer
  // to reference locals.
  if (RegInfo->hasBasePointer(MF))
    AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr), BasePtr)
                   .addReg(ARM::SP));

  // If the frame has variable sized objects then the epilogue must restore
  // the sp from fp. We can assume there's an FP here since hasFP already
  // checks for hasVarSizedObjects.
  if (MFI.hasVarSizedObjects())
    AFI->setShouldRestoreSPFromFP(true);
}

static bool isCSRestore(MachineInstr &MI, const MCPhysReg *CSRegs) {
  if (MI.getOpcode() == ARM::tLDRspi && MI.getOperand(1).isFI() &&
      isCalleeSavedRegister(MI.getOperand(0).getReg(), CSRegs))
    return true;
  else if (MI.getOpcode() == ARM::tPOP) {
    return true;
  } else if (MI.getOpcode() == ARM::tMOVr) {
    unsigned Dst = MI.getOperand(0).getReg();
    unsigned Src = MI.getOperand(1).getReg();
    return ((ARM::tGPRRegClass.contains(Src) || Src == ARM::LR) &&
            ARM::hGPRRegClass.contains(Dst));
  }
  return false;
}

void Thumb1FrameLowering::emitEpilogue(MachineFunction &MF,
                                   MachineBasicBlock &MBB) const {
  MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
  DebugLoc dl = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  const ThumbRegisterInfo *RegInfo =
      static_cast<const ThumbRegisterInfo *>(STI.getRegisterInfo());
  const Thumb1InstrInfo &TII =
      *static_cast<const Thumb1InstrInfo *>(STI.getInstrInfo());

  unsigned ArgRegsSaveSize = AFI->getArgRegsSaveSize();
  int NumBytes = (int)MFI.getStackSize();
  assert((unsigned)NumBytes >= ArgRegsSaveSize &&
         "ArgRegsSaveSize is included in NumBytes");
  const MCPhysReg *CSRegs = RegInfo->getCalleeSavedRegs(&MF);
  unsigned FramePtr = RegInfo->getFrameRegister(MF);

  if (!AFI->hasStackFrame()) {
    if (NumBytes - ArgRegsSaveSize != 0)
      emitSPUpdate(MBB, MBBI, TII, dl, *RegInfo, NumBytes - ArgRegsSaveSize);
  } else {
    // Unwind MBBI to point to first LDR / VLDRD.
    if (MBBI != MBB.begin()) {
      do
        --MBBI;
      while (MBBI != MBB.begin() && isCSRestore(*MBBI, CSRegs));
      if (!isCSRestore(*MBBI, CSRegs))
        ++MBBI;
    }

    // Move SP to start of FP callee save spill area.
    NumBytes -= (AFI->getGPRCalleeSavedArea1Size() +
                 AFI->getGPRCalleeSavedArea2Size() +
                 AFI->getDPRCalleeSavedAreaSize() +
                 ArgRegsSaveSize);

    if (AFI->shouldRestoreSPFromFP()) {
      NumBytes = AFI->getFramePtrSpillOffset() - NumBytes;
      // Reset SP based on frame pointer only if the stack frame extends beyond
      // frame pointer stack slot, the target is ELF and the function has FP, or
      // the target uses var sized objects.
      if (NumBytes) {
        assert(!MFI.getPristineRegs(MF).test(ARM::R4) &&
               "No scratch register to restore SP from FP!");
        emitThumbRegPlusImmediate(MBB, MBBI, dl, ARM::R4, FramePtr, -NumBytes,
                                  TII, *RegInfo);
        AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr),
                               ARM::SP)
          .addReg(ARM::R4));
      } else
        AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr),
                               ARM::SP)
          .addReg(FramePtr));
    } else {
      if (MBBI != MBB.end() && MBBI->getOpcode() == ARM::tBX_RET &&
          &MBB.front() != &*MBBI && std::prev(MBBI)->getOpcode() == ARM::tPOP) {
        MachineBasicBlock::iterator PMBBI = std::prev(MBBI);
        if (!tryFoldSPUpdateIntoPushPop(STI, MF, &*PMBBI, NumBytes))
          emitSPUpdate(MBB, PMBBI, TII, dl, *RegInfo, NumBytes);
      } else if (!tryFoldSPUpdateIntoPushPop(STI, MF, &*MBBI, NumBytes))
        emitSPUpdate(MBB, MBBI, TII, dl, *RegInfo, NumBytes);
    }
  }

  if (needPopSpecialFixUp(MF)) {
    bool Done = emitPopSpecialFixUp(MBB, /* DoIt */ true);
    (void)Done;
    assert(Done && "Emission of the special fixup failed!?");
  }
}

bool Thumb1FrameLowering::canUseAsEpilogue(const MachineBasicBlock &MBB) const {
  if (!needPopSpecialFixUp(*MBB.getParent()))
    return true;

  MachineBasicBlock *TmpMBB = const_cast<MachineBasicBlock *>(&MBB);
  return emitPopSpecialFixUp(*TmpMBB, /* DoIt */ false);
}

bool Thumb1FrameLowering::needPopSpecialFixUp(const MachineFunction &MF) const {
  ARMFunctionInfo *AFI =
      const_cast<MachineFunction *>(&MF)->getInfo<ARMFunctionInfo>();
  if (AFI->getArgRegsSaveSize())
    return true;

  // LR cannot be encoded with Thumb1, i.e., it requires a special fix-up.
  for (const CalleeSavedInfo &CSI : MF.getFrameInfo().getCalleeSavedInfo())
    if (CSI.getReg() == ARM::LR)
      return true;

  return false;
}

bool Thumb1FrameLowering::emitPopSpecialFixUp(MachineBasicBlock &MBB,
                                              bool DoIt) const {
  MachineFunction &MF = *MBB.getParent();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  unsigned ArgRegsSaveSize = AFI->getArgRegsSaveSize();
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  const ThumbRegisterInfo *RegInfo =
      static_cast<const ThumbRegisterInfo *>(STI.getRegisterInfo());

  // If MBBI is a return instruction, or is a tPOP followed by a return
  // instruction in the successor BB, we may be able to directly restore
  // LR in the PC.
  // This is only possible with v5T ops (v4T can't change the Thumb bit via
  // a POP PC instruction), and only if we do not need to emit any SP update.
  // Otherwise, we need a temporary register to pop the value
  // and copy that value into LR.
  auto MBBI = MBB.getFirstTerminator();
  bool CanRestoreDirectly = STI.hasV5TOps() && !ArgRegsSaveSize;
  if (CanRestoreDirectly) {
    if (MBBI != MBB.end() && MBBI->getOpcode() != ARM::tB)
      CanRestoreDirectly = (MBBI->getOpcode() == ARM::tBX_RET ||
                            MBBI->getOpcode() == ARM::tPOP_RET);
    else {
      auto MBBI_prev = MBBI;
      MBBI_prev--;
      assert(MBBI_prev->getOpcode() == ARM::tPOP);
      assert(MBB.succ_size() == 1);
      if ((*MBB.succ_begin())->begin()->getOpcode() == ARM::tBX_RET)
        MBBI = MBBI_prev; // Replace the final tPOP with a tPOP_RET.
      else
        CanRestoreDirectly = false;
    }
  }

  if (CanRestoreDirectly) {
    if (!DoIt || MBBI->getOpcode() == ARM::tPOP_RET)
      return true;
    MachineInstrBuilder MIB =
        AddDefaultPred(
            BuildMI(MBB, MBBI, MBBI->getDebugLoc(), TII.get(ARM::tPOP_RET)));
    // Copy implicit ops and popped registers, if any.
    for (auto MO: MBBI->operands())
      if (MO.isReg() && (MO.isImplicit() || MO.isDef()))
        MIB.addOperand(MO);
    MIB.addReg(ARM::PC, RegState::Define);
    // Erase the old instruction (tBX_RET or tPOP).
    MBB.erase(MBBI);
    return true;
  }

  // Look for a temporary register to use.
  // First, compute the liveness information.
  LivePhysRegs UsedRegs(STI.getRegisterInfo());
  UsedRegs.addLiveOuts(MBB);
  // The semantic of pristines changed recently and now,
  // the callee-saved registers that are touched in the function
  // are not part of the pristines set anymore.
  // Add those callee-saved now.
  const TargetRegisterInfo *TRI = STI.getRegisterInfo();
  const MCPhysReg *CSRegs = TRI->getCalleeSavedRegs(&MF);
  for (unsigned i = 0; CSRegs[i]; ++i)
    UsedRegs.addReg(CSRegs[i]);

  DebugLoc dl = DebugLoc();
  if (MBBI != MBB.end()) {
    dl = MBBI->getDebugLoc();
    auto InstUpToMBBI = MBB.end();
    while (InstUpToMBBI != MBBI)
      // The pre-decrement is on purpose here.
      // We want to have the liveness right before MBBI.
      UsedRegs.stepBackward(*--InstUpToMBBI);
  }

  // Look for a register that can be directly use in the POP.
  unsigned PopReg = 0;
  // And some temporary register, just in case.
  unsigned TemporaryReg = 0;
  BitVector PopFriendly =
      TRI->getAllocatableSet(MF, TRI->getRegClass(ARM::tGPRRegClassID));
  assert(PopFriendly.any() && "No allocatable pop-friendly register?!");
  // Rebuild the GPRs from the high registers because they are removed
  // form the GPR reg class for thumb1.
  BitVector GPRsNoLRSP =
      TRI->getAllocatableSet(MF, TRI->getRegClass(ARM::hGPRRegClassID));
  GPRsNoLRSP |= PopFriendly;
  GPRsNoLRSP.reset(ARM::LR);
  GPRsNoLRSP.reset(ARM::SP);
  GPRsNoLRSP.reset(ARM::PC);
  for (int Register = GPRsNoLRSP.find_first(); Register != -1;
       Register = GPRsNoLRSP.find_next(Register)) {
    if (!UsedRegs.contains(Register)) {
      // Remember the first pop-friendly register and exit.
      if (PopFriendly.test(Register)) {
        PopReg = Register;
        TemporaryReg = 0;
        break;
      }
      // Otherwise, remember that the register will be available to
      // save a pop-friendly register.
      TemporaryReg = Register;
    }
  }

  if (!DoIt && !PopReg && !TemporaryReg)
    return false;

  assert((PopReg || TemporaryReg) && "Cannot get LR");

  if (TemporaryReg) {
    assert(!PopReg && "Unnecessary MOV is about to be inserted");
    PopReg = PopFriendly.find_first();
    AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr))
                       .addReg(TemporaryReg, RegState::Define)
                       .addReg(PopReg, RegState::Kill));
  }

  if (MBBI != MBB.end() && MBBI->getOpcode() == ARM::tPOP_RET) {
    // We couldn't use the direct restoration above, so
    // perform the opposite conversion: tPOP_RET to tPOP.
    MachineInstrBuilder MIB =
        AddDefaultPred(
            BuildMI(MBB, MBBI, MBBI->getDebugLoc(), TII.get(ARM::tPOP)));
    bool Popped = false;
    for (auto MO: MBBI->operands())
      if (MO.isReg() && (MO.isImplicit() || MO.isDef()) &&
          MO.getReg() != ARM::PC) {
        MIB.addOperand(MO);
        if (!MO.isImplicit())
          Popped = true;
      }
    // Is there anything left to pop?
    if (!Popped)
      MBB.erase(MIB.getInstr());
    // Erase the old instruction.
    MBB.erase(MBBI);
    MBBI = AddDefaultPred(BuildMI(MBB, MBB.end(), dl, TII.get(ARM::tBX_RET)));
  }

  assert(PopReg && "Do not know how to get LR");
  AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tPOP)))
      .addReg(PopReg, RegState::Define);

  emitSPUpdate(MBB, MBBI, TII, dl, *RegInfo, ArgRegsSaveSize);

  AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr))
                     .addReg(ARM::LR, RegState::Define)
                     .addReg(PopReg, RegState::Kill));

  if (TemporaryReg)
    AddDefaultPred(BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr))
                       .addReg(PopReg, RegState::Define)
                       .addReg(TemporaryReg, RegState::Kill));

  return true;
}

// Return the first iteraror after CurrentReg which is present in EnabledRegs,
// or OrderEnd if no further registers are in that set. This does not advance
// the iterator fiorst, so returns CurrentReg if it is in EnabledRegs.
template <unsigned SetSize>
static const unsigned *
findNextOrderedReg(const unsigned *CurrentReg,
                   SmallSet<unsigned, SetSize> &EnabledRegs,
                   const unsigned *OrderEnd) {
  while (CurrentReg != OrderEnd && !EnabledRegs.count(*CurrentReg))
    ++CurrentReg;
  return CurrentReg;
}

bool Thumb1FrameLowering::
spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator MI,
                          const std::vector<CalleeSavedInfo> &CSI,
                          const TargetRegisterInfo *TRI) const {
  if (CSI.empty())
    return false;

  DebugLoc DL;
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  MachineFunction &MF = *MBB.getParent();
  const ARMBaseRegisterInfo *RegInfo = static_cast<const ARMBaseRegisterInfo *>(
      MF.getSubtarget().getRegisterInfo());

  SmallSet<unsigned, 9> LoRegsToSave; // r0-r7, lr
  SmallSet<unsigned, 4> HiRegsToSave; // r8-r11
  SmallSet<unsigned, 9> CopyRegs; // Registers which can be used after pushing
                           // LoRegs for saving HiRegs.

  for (unsigned i = CSI.size(); i != 0; --i) {
    unsigned Reg = CSI[i-1].getReg();

    if (ARM::tGPRRegClass.contains(Reg) || Reg == ARM::LR) {
      LoRegsToSave.insert(Reg);
    } else if (ARM::hGPRRegClass.contains(Reg) && Reg != ARM::LR) {
      HiRegsToSave.insert(Reg);
    } else {
      llvm_unreachable("callee-saved register of unexpected class");
    }

    if ((ARM::tGPRRegClass.contains(Reg) || Reg == ARM::LR) &&
        !MF.getRegInfo().isLiveIn(Reg) &&
        !(hasFP(MF) && Reg == RegInfo->getFrameRegister(MF)))
      CopyRegs.insert(Reg);
  }

  // Unused argument registers can be used for the high register saving.
  for (unsigned ArgReg : {ARM::R0, ARM::R1, ARM::R2, ARM::R3})
    if (!MF.getRegInfo().isLiveIn(ArgReg))
      CopyRegs.insert(ArgReg);

  // Push the low registers and lr
  if (!LoRegsToSave.empty()) {
    MachineInstrBuilder MIB = BuildMI(MBB, MI, DL, TII.get(ARM::tPUSH));
    AddDefaultPred(MIB);
    for (unsigned Reg : {ARM::R4, ARM::R5, ARM::R6, ARM::R7, ARM::LR}) {
      if (LoRegsToSave.count(Reg)) {
        bool isKill = !MF.getRegInfo().isLiveIn(Reg);
        if (isKill)
          MBB.addLiveIn(Reg);

        MIB.addReg(Reg, getKillRegState(isKill));
      }
    }
    MIB.setMIFlags(MachineInstr::FrameSetup);
  }

  // Push the high registers. There are no store instructions that can access
  // these registers directly, so we have to move them to low registers, and
  // push them. This might take multiple pushes, as it is possible for there to
  // be fewer low registers available than high registers which need saving.

  // These are in reverse order so that in the case where we need to use
  // multiple PUSH instructions, the order of the registers on the stack still
  // matches the unwind info. They need to be swicthed back to ascending order
  // before adding to the PUSH instruction.
  static const unsigned AllCopyRegs[] = {ARM::LR, ARM::R7, ARM::R6,
                                         ARM::R5, ARM::R4, ARM::R3,
                                         ARM::R2, ARM::R1, ARM::R0};
  static const unsigned AllHighRegs[] = {ARM::R11, ARM::R10, ARM::R9, ARM::R8};

  const unsigned *AllCopyRegsEnd = std::end(AllCopyRegs);
  const unsigned *AllHighRegsEnd = std::end(AllHighRegs);

  // Find the first register to save.
  const unsigned *HiRegToSave = findNextOrderedReg(
      std::begin(AllHighRegs), HiRegsToSave, AllHighRegsEnd);

  while (HiRegToSave != AllHighRegsEnd) {
    // Find the first low register to use.
    const unsigned *CopyReg =
        findNextOrderedReg(std::begin(AllCopyRegs), CopyRegs, AllCopyRegsEnd);

    // Create the PUSH, but don't insert it yet (the MOVs need to come first).
    MachineInstrBuilder PushMIB = BuildMI(MF, DL, TII.get(ARM::tPUSH));
    AddDefaultPred(PushMIB);

    SmallVector<unsigned, 4> RegsToPush;
    while (HiRegToSave != AllHighRegsEnd && CopyReg != AllCopyRegsEnd) {
      if (HiRegsToSave.count(*HiRegToSave)) {
        bool isKill = !MF.getRegInfo().isLiveIn(*HiRegToSave);
        if (isKill)
          MBB.addLiveIn(*HiRegToSave);

        // Emit a MOV from the high reg to the low reg.
        MachineInstrBuilder MIB =
            BuildMI(MBB, MI, DL, TII.get(ARM::tMOVr));
        MIB.addReg(*CopyReg, RegState::Define);
        MIB.addReg(*HiRegToSave, getKillRegState(isKill));
        AddDefaultPred(MIB);

        // Record the register that must be added to the PUSH.
        RegsToPush.push_back(*CopyReg);

        CopyReg = findNextOrderedReg(++CopyReg, CopyRegs, AllCopyRegsEnd);
        HiRegToSave =
            findNextOrderedReg(++HiRegToSave, HiRegsToSave, AllHighRegsEnd);
      }
    }

    // Add the low registers to the PUSH, in ascending order.
    for (unsigned Reg : reverse(RegsToPush))
      PushMIB.addReg(Reg, RegState::Kill);

    // Insert the PUSH instruction after the MOVs.
    MBB.insert(MI, PushMIB);
  }

  return true;
}

bool Thumb1FrameLowering::
restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator MI,
                            const std::vector<CalleeSavedInfo> &CSI,
                            const TargetRegisterInfo *TRI) const {
  if (CSI.empty())
    return false;

  MachineFunction &MF = *MBB.getParent();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  const ARMBaseRegisterInfo *RegInfo = static_cast<const ARMBaseRegisterInfo *>(
      MF.getSubtarget().getRegisterInfo());

  bool isVarArg = AFI->getArgRegsSaveSize() > 0;
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();

  SmallSet<unsigned, 9> LoRegsToRestore;
  SmallSet<unsigned, 4> HiRegsToRestore;
  // Low registers (r0-r7) which can be used to restore the high registers.
  SmallSet<unsigned, 9> CopyRegs;

  for (CalleeSavedInfo I : CSI) {
    unsigned Reg = I.getReg();

    if (ARM::tGPRRegClass.contains(Reg) || Reg == ARM::LR) {
      LoRegsToRestore.insert(Reg);
    } else if (ARM::hGPRRegClass.contains(Reg) && Reg != ARM::LR) {
      HiRegsToRestore.insert(Reg);
    } else {
      llvm_unreachable("callee-saved register of unexpected class");
    }

    // If this is a low register not used as the frame pointer, we may want to
    // use it for restoring the high registers.
    if ((ARM::tGPRRegClass.contains(Reg)) &&
        !(hasFP(MF) && Reg == RegInfo->getFrameRegister(MF)))
      CopyRegs.insert(Reg);
  }

  // If this is a return block, we may be able to use some unused return value
  // registers for restoring the high regs.
  auto Terminator = MBB.getFirstTerminator();
  if (Terminator != MBB.end() && Terminator->getOpcode() == ARM::tBX_RET) {
    CopyRegs.insert(ARM::R0);
    CopyRegs.insert(ARM::R1);
    CopyRegs.insert(ARM::R2);
    CopyRegs.insert(ARM::R3);
    for (auto Op : Terminator->implicit_operands()) {
      if (Op.isReg())
        CopyRegs.erase(Op.getReg());
    }
  }

  static const unsigned AllCopyRegs[] = {ARM::R0, ARM::R1, ARM::R2, ARM::R3,
                                         ARM::R4, ARM::R5, ARM::R6, ARM::R7};
  static const unsigned AllHighRegs[] = {ARM::R8, ARM::R9, ARM::R10, ARM::R11};

  const unsigned *AllCopyRegsEnd = std::end(AllCopyRegs);
  const unsigned *AllHighRegsEnd = std::end(AllHighRegs);

  // Find the first register to restore.
  auto HiRegToRestore = findNextOrderedReg(std::begin(AllHighRegs),
                                           HiRegsToRestore, AllHighRegsEnd);

  while (HiRegToRestore != AllHighRegsEnd) {
    assert(!CopyRegs.empty());
    // Find the first low register to use.
    auto CopyReg =
        findNextOrderedReg(std::begin(AllCopyRegs), CopyRegs, AllCopyRegsEnd);

    // Create the POP instruction.
    MachineInstrBuilder PopMIB = BuildMI(MBB, MI, DL, TII.get(ARM::tPOP));
    AddDefaultPred(PopMIB);

    while (HiRegToRestore != AllHighRegsEnd && CopyReg != AllCopyRegsEnd) {
      // Add the low register to the POP.
      PopMIB.addReg(*CopyReg, RegState::Define);

      // Create the MOV from low to high register.
      MachineInstrBuilder MIB =
          BuildMI(MBB, MI, DL, TII.get(ARM::tMOVr));
      MIB.addReg(*HiRegToRestore, RegState::Define);
      MIB.addReg(*CopyReg, RegState::Kill);
      AddDefaultPred(MIB);

      CopyReg = findNextOrderedReg(++CopyReg, CopyRegs, AllCopyRegsEnd);
      HiRegToRestore =
          findNextOrderedReg(++HiRegToRestore, HiRegsToRestore, AllHighRegsEnd);
    }
  }




  MachineInstrBuilder MIB = BuildMI(MF, DL, TII.get(ARM::tPOP));
  AddDefaultPred(MIB);

  bool NeedsPop = false;
  for (unsigned i = CSI.size(); i != 0; --i) {
    unsigned Reg = CSI[i-1].getReg();

    // High registers (excluding lr) have already been dealt with
    if (!(ARM::tGPRRegClass.contains(Reg) || Reg == ARM::LR))
      continue;

    if (Reg == ARM::LR) {
      if (MBB.succ_empty()) {
        // Special epilogue for vararg functions. See emitEpilogue
        if (isVarArg)
          continue;
        // ARMv4T requires BX, see emitEpilogue
        if (!STI.hasV5TOps())
          continue;
        Reg = ARM::PC;
        (*MIB).setDesc(TII.get(ARM::tPOP_RET));
        if (MI != MBB.end())
          MIB.copyImplicitOps(*MI);
        MI = MBB.erase(MI);
      } else
        // LR may only be popped into PC, as part of return sequence.
        // If this isn't the return sequence, we'll need emitPopSpecialFixUp
        // to restore LR the hard way.
        continue;
    }
    MIB.addReg(Reg, getDefRegState(true));
    NeedsPop = true;
  }

  // It's illegal to emit pop instruction without operands.
  if (NeedsPop)
    MBB.insert(MI, &*MIB);
  else
    MF.DeleteMachineInstr(MIB);

  return true;
}
