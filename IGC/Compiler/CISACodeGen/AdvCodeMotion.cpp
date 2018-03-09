#include "common/LLVMWarningsPush.hpp"
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Local.h>
#include "common/LLVMWarningsPop.hpp"
#include "GenISAIntrinsics/GenIntrinsics.h"

#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/CISACodeGen/AdvCodeMotion.h"
#include "Compiler/CISACodeGen/WIAnalysis.hpp"

using namespace llvm;
using namespace llvm::PatternMatch;
using namespace IGC;
using namespace IGC::IGCMD;

namespace {

class WorkItemSetting {
public:
  struct Vec3 { Value *X, *Y, *Z; };

  Vec3 LocalId{nullptr, nullptr, nullptr};
  Vec3 GlobalSize{nullptr, nullptr, nullptr};
  Vec3 GlobalSize1{nullptr, nullptr, nullptr};
  Vec3 LocalSize{nullptr, nullptr, nullptr};
  Vec3 EnqueuedLocalSize{nullptr, nullptr, nullptr};
  Vec3 GroupId{nullptr, nullptr, nullptr};
  Vec3 GlobalOffset{nullptr, nullptr, nullptr};
  Vec3 GlobalId{nullptr, nullptr, nullptr};

  WorkItemSetting() {}

  void collect(Function *F);
  bool hasOneDim() const;
};

class AdvCodeMotion : public FunctionPass {
  unsigned ControlMask;

  DominatorTree *DT;
  LoopInfo *LI;
  PostDominatorTree *PDT;
  ScalarEvolution *SE;
  WIAnalysis *WI;

  WorkItemSetting WIS;

public:
  static char ID;

  AdvCodeMotion(unsigned C = 0) : FunctionPass(ID), ControlMask(C) {}

  bool runOnFunction(Function &F) override;

  StringRef getPassName() const override { return "Advanced Code Motion"; }

private:
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<CodeGenContextWrapper>();
    AU.addRequired<MetaDataUtilsWrapper>();
    AU.addRequired<WIAnalysis>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
  }

  bool hoistUniform(BasicBlock *Src, BasicBlock *Dst) const;
  bool hoistMost(bool InvPred, BasicBlock *IfBB,
                 BasicBlock *TBB, BasicBlock *FBB, BasicBlock *JBB) const;
  bool hoistMost2(bool InvPred, BasicBlock *IfBB,
                  BasicBlock *TBB, BasicBlock *FBB, BasicBlock *JBB) const;

  // Check whether any(Cond) is always true.
  bool isUniformlyAlwaysTaken(bool InvPred, Value *Cond) const;
  // Check whether most work-items will take on the given condition.
  bool isMostlyTaken(bool InvPred, Value *Cond) const;

  // Check of special cases.
  // [0, global_size(0) != global_id(0)
  bool isCase1(bool InvPred, Value *Cond) const;
};

char AdvCodeMotion::ID = 0;

} // End anonymous namespace

FunctionPass *IGC::createAdvCodeMotionPass(unsigned C) {
  return new AdvCodeMotion(C);
}

#define PASS_FLAG     "igc-advcodemotion"
#define PASS_DESC     "Advanced Code Motion"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
namespace IGC {
IGC_INITIALIZE_PASS_BEGIN(AdvCodeMotion, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(WIAnalysis)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass);
IGC_INITIALIZE_PASS_END(AdvCodeMotion, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY, PASS_ANALYSIS)
} // End namespace IGC

// Fill work-item setting from kernel function 'F'.
void WorkItemSetting::collect(Function *F) {
  // Extract individual elements from a vec3.
  auto getXYZ = [](Value *V3,
                   unsigned IdX, unsigned IdY, unsigned IdZ) -> Vec3 {
    if (!V3) return Vec3{nullptr, nullptr, nullptr};
    Value *X = nullptr;
    Value *Y = nullptr;
    Value *Z = nullptr;
    for (auto *U : V3->users()) {
      auto EEI = dyn_cast<ExtractElementInst>(U);
      if (!EEI || EEI->getVectorOperand() != V3)
        continue;
      auto Cst = dyn_cast<ConstantInt>(EEI->getIndexOperand());
      if (!Cst)
        continue;
      unsigned I = unsigned(Cst->getZExtValue());
      if (I == IdX)
        X = U;
      else if (I == IdY)
        Y = U;
      else if (I == IdZ)
        Z = U;
    }
    return Vec3{X, Y, Z};
  };

  // Find implicit arguments.
  for (auto AI = F->arg_begin(), AE = F->arg_end(); AI != AE; ++AI) {
    if (!AI->hasName())
      continue;
    auto Name = AI->getName();
    if (Name == "r0")
      GroupId = getXYZ(&*AI, 1, 6, 7);
    else if (Name == "payloadHeader")
      GlobalOffset = getXYZ(&*AI, 0, 1, 2);
    else if (Name == "globalSize")
      GlobalSize = getXYZ(&*AI, 0, 1, 2);
    else if (Name == "globalSize1")
      GlobalSize1 = getXYZ(&*AI, 0, 1, 2);
    else if (Name == "localSize")
      LocalSize = getXYZ(&*AI, 0, 1, 2);
    else if (Name == "enqueuedLocalSize")
      EnqueuedLocalSize = getXYZ(&*AI, 0, 1, 2);
    else if (Name == "localIdX" && !AI->use_empty())
      LocalId.X = &*AI;
    else if (Name == "localIdY" && !AI->use_empty())
      LocalId.Y = &*AI;
    else if (Name == "localIdZ" && !AI->use_empty())
      LocalId.Z = &*AI;
  }

  auto Entry = &F->getEntryBlock();
  for (auto BI = Entry->begin(), BE = Entry->end(); BI != BE; ++BI) {
    auto Inst = &*BI;
    // GlobalId.X
    if (match(Inst,
              m_Add(m_Add(m_ZExt(m_Specific(LocalId.X)),
                          m_Mul(m_Specific(EnqueuedLocalSize.X),
                                m_Specific(GroupId.X))),
                    m_Specific(GlobalOffset.X)))) {
      GlobalId.X = Inst;
    }
    // TODO: Add support of GlobalId.Y & GlobalId.Z.
  }
  // On some clients, global size calculation is different.
  if (GlobalSize.X == nullptr && GlobalSize1.X != nullptr) {
    for (auto BI = Entry->begin(), BE = Entry->end(); BI != BE; ++BI) {
      auto Inst = &*BI;
      // GlobalSize.X = (GlobalSize1.X == 0) ? X : GlobalSize1.X
      Value *X;
      ICmpInst::Predicate Pred;
      if (match(Inst,
                m_Select(m_ICmp(Pred, m_Specific(GlobalSize1.X), m_Zero()),
                         m_Value(X),
                         m_Specific(GlobalSize1.X))) &&
          Pred == ICmpInst::ICMP_EQ) {
        GlobalSize.X = Inst;
      }
      // TODO: Add support of GlobalSize.Y & GlobalSize.Z.
    }
  }
}

bool WorkItemSetting::hasOneDim() const {
  if (!LocalSize.X || LocalSize.Y || LocalSize.Z)
    return false;
  if (!EnqueuedLocalSize.X || EnqueuedLocalSize.Y || EnqueuedLocalSize.Z)
    return false;
  if (!GlobalSize.X || GlobalSize.Y || GlobalSize.Z)
    return false;
  if (!GroupId.X || GroupId.Y || GroupId.Z)
    return false;
  if (!GlobalOffset.X || GlobalOffset.Y || GlobalOffset.Z)
    return false;
  if (!LocalId.X || LocalId.Y || LocalId.Z)
    return false;
  if (!GlobalId.X || GlobalId.Y || GlobalId.Z)
    return false;
  return true;
}

bool AdvCodeMotion::hoistUniform(BasicBlock *Src, BasicBlock *Dst) const {
  bool Changed = false;
  auto Pos = Dst->getTerminator();
  for (auto BI = Src->begin(), BE = Src->end(); BI != BE; /*EMPTY*/) {
    Instruction *Inst = &*BI++;
    if (WI->whichDepend(Inst) != WIAnalysis::UNIFORM)
      break;
    Inst->moveBefore(Pos);
    Changed = true;
  }
  return Changed;
}

namespace {
class RegionSubgraph {
  BasicBlock *Exit;

public:
  RegionSubgraph(BasicBlock *E) : Exit(E) {}

  bool preVisit(Optional<BasicBlock *> From, BasicBlock *To) {
    if (To == Exit)
      return false;
    return Visited.insert(To).second;
  }

  SmallPtrSet<BasicBlock *, 32> Visited;
};
} // End anonymous namespace

namespace llvm {
template<>
class po_iterator_storage<RegionSubgraph, true> {
  RegionSubgraph &RSG;

public:
  po_iterator_storage(RegionSubgraph &G) : RSG(G) {}

  bool insertEdge(Optional<BasicBlock *> From, BasicBlock *To) {
    return RSG.preVisit(From, To);
  }
  void finishPostorder(BasicBlock *) {}
};
} // End llvm namespace

static bool hasMemoryWrite(BasicBlock *BB) {
  for (auto II = BB->begin(), IE = BB->end(); II != IE; ++II)
    if (II->mayWriteToMemory())
      return true;
  return false;
}

static bool isDummyBasicBlock(BasicBlock *BB) {
  if (BB->size() != 1)
    return false;
  if (std::next(pred_begin(BB)) != pred_end(BB))
    return false;
  if (std::next(succ_begin(BB)) != succ_end(BB))
    return false;
  return true;
}

static BasicBlock *getJointBasicBlock(PostDominatorTree *PDT, BasicBlock *BB,
                                      BasicBlock *IfBB) {
  if (isDummyBasicBlock(BB))
    BB = *succ_begin(BB);
  if (PDT->dominates(BB, IfBB))
    return BB;
  return nullptr;
}

static std::tuple<bool, BasicBlock *, BasicBlock *, BasicBlock *>
getIfStatementBlock(PostDominatorTree *PDT, BasicBlock *IfBB) {
  auto SI = succ_begin(IfBB), SE = succ_end(IfBB);
  BasicBlock *TBB = (SI != SE) ? *SI++ : nullptr;
  BasicBlock *FBB = (SI != SE) ? *SI++ : nullptr;
  if (!TBB || !FBB || SI != SE)
    return std::make_tuple(false, nullptr, nullptr, nullptr);

  bool InvPred = false;
  BasicBlock *JBB = getJointBasicBlock(PDT, FBB, IfBB);
  if (!JBB) {
    std::swap(TBB, FBB);
    InvPred = true;
    JBB = getJointBasicBlock(PDT, FBB, IfBB);
  }
  if (!JBB)
    return std::make_tuple(false, nullptr, nullptr, nullptr);

  return std::make_tuple(InvPred, TBB, FBB, JBB);
}

bool AdvCodeMotion::hoistMost(bool InvPred, BasicBlock *IfBB,
                              BasicBlock *TBB, BasicBlock *FBB,
                              BasicBlock *JBB) const {
  // Hoist most code from TBB into IfBB, where IfBB, TBB and JBB are
  // if-statement blocks.
  SmallVector<BasicBlock *, 2> Preds(pred_begin(JBB), pred_end(JBB));
  if (Preds.size() != 2)
    return false;
  BasicBlock *Exit = Preds[0];
  if (!DT->dominates(TBB, Exit))
    Exit = Preds[1];
  if (!DT->dominates(TBB, Exit))
    return false;

  bool Changed = false;
  Instruction *Pos = IfBB->getTerminator();
  auto Cond = cast<BranchInst>(Pos)->getCondition();
  RegionSubgraph RSG(JBB);
  // Check if there's any memory write.
  for (auto SI = po_ext_begin(TBB, RSG),
            SE = po_ext_end(TBB, RSG); SI != SE; ++SI)
    if (hasMemoryWrite(*SI))
      return false;
  // Check whether Cond is used in that region.
  SmallPtrSet<BasicBlock *, 8> UserBlocks;
  for (auto *User : Cond->users()) {
    Instruction *I = dyn_cast<Instruction>(User);
    if (!I)
      continue;
    BasicBlock *BB = I->getParent();
    if (UserBlocks.count(BB))
      continue;
    if (RSG.Visited.count(BB))
      return false;
  }
  // Split IfBB and merge TBB into upper part and Exit into lower part.
  BasicBlock *Lower = IfBB->splitBasicBlock(Pos);
  BasicBlock *Upper = *pred_begin(Lower);
  // Merge entry block into upper part.
  Pos = Upper->getTerminator();
  for (auto BI = TBB->begin(), BE = TBB->end(); BI != BE; /*EMPTY*/) {
    Instruction *Inst = &*BI++;
    Inst->moveBefore(Pos);
  }
  Pos->eraseFromParent();
  // Merge exit block into lower part.
  Pos = &Lower->front();
  for (auto BI = Exit->begin(), BE = Exit->end(); BI != BE; /*EMPTY*/) {
    Instruction *Inst = &*BI++;
    if (isa<TerminatorInst>(Inst))
      break;
    Inst->moveBefore(Pos);
  }
  Lower->moveBefore(JBB);
  // Rebuild CFG.
  Exit->replaceAllUsesWith(Lower);
  TBB->replaceAllUsesWith(Exit);
  TBB->eraseFromParent();
  // Update PHI nodes.
  for (auto BI = JBB->begin(), BE = JBB->end(); BI != BE; ++BI) {
    PHINode *PN = dyn_cast<PHINode>(&*BI);
    if (!PN)
      break;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      if (PN->getIncomingBlock(i) == Lower)
        PN->setIncomingBlock(i, Exit);
    }
  }
  Changed = true;
  return Changed;
}

bool AdvCodeMotion::hoistMost2(bool InvPred, BasicBlock *IfBB,
                               BasicBlock *TBB, BasicBlock *FBB,
                               BasicBlock *JBB) const {
  bool InvPred2;
  BasicBlock *TBB2, *FBB2, *JBB2;
  std::tie(InvPred2, TBB2, FBB2, JBB2) =
      getIfStatementBlock(PDT, TBB);
  if (!JBB2)
    return false;
  // Check whether it's safe to hoist TBB.
  if (hasMemoryWrite(TBB))
    return false;
  Instruction *Pos = IfBB->getTerminator();
  auto Cond = cast<BranchInst>(Pos)->getCondition();
  for (auto *User : Cond->users()) {
    Instruction *I = dyn_cast<Instruction>(User);
    if (!I)
      continue;
    if (I->getParent() == TBB)
      return false;
  }

  //    IfBB                 IfBB
  //   /    \                TBB
  //  |     TBB             /    \
  //  |    /    \         FBB2  TBB2
  //  |  FBB2  TBB2  =>    |    ...
  // FBB  |    ...          \    /
  //  |    \    /            JBB2
  //  |     JBB2             JBB
  //   \     /
  //     JBB
  // Hoist TBB only and simplify the CFG.

  // Merge TBB into IfBB.
  if (InvPred) {
    IRBuilder<> IRB(Pos);
    Cond = IRB.CreateNot(Cond);
  }
  for (auto BI = TBB->begin(), BE = TBB->end(); BI != BE; /*EMPTY*/) {
    Instruction *Inst = &*BI++;
    Inst->moveBefore(Pos);
  }
  Pos->eraseFromParent(); // Remove original terminator in IfBB
  Pos = IfBB->getTerminator(); // Fetch the new terminator (the one in TBB).
  IRBuilder<> IRB(Pos);

  User *NewUser = nullptr;
  auto Cond2 = cast<BranchInst>(Pos)->getCondition();
  auto C2 = Cond2;
  if (InvPred2) {
    C2 = IRB.CreateNot(C2);
    NewUser = cast<User>(C2);
  }
  Value *NewCond = IRB.CreateAnd(Cond, C2);
  if (!NewUser) NewUser = cast<User>(NewCond);
  if (InvPred2) NewCond = IRB.CreateNot(NewCond);
  for (auto UI = Cond2->use_begin(),
            UE = Cond2->use_end(); UI != UE; /*EMPTY*/) {
    auto &U = *UI++;
    if (U.getUser() == NewUser)
      continue;
    U.set(NewCond);
  }

  // Merge JBB into JBB2
  Pos = JBB2->getTerminator();
  for (auto BI = JBB->begin(), BE = JBB->end(); BI != BE; /*EMPTY*/) {
    Instruction *Inst = &*BI++;
    if (PHINode *PN = dyn_cast<PHINode>(Inst)) {
      Value *NewVal = PN->getIncomingValueForBlock(JBB2);
      PN->replaceAllUsesWith(NewVal);
      PN->eraseFromParent();
      continue;
    }
    Inst->moveBefore(Pos);
  }
  Pos->eraseFromParent();
  // Update PHI nodes.
  for (auto SI = succ_begin(JBB2), SE = succ_end(JBB2); SI != SE; ++SI) {
    BasicBlock *BB = *SI;
    for (auto BI = BB->begin(), BE = BB->end(); BI != BE; ++BI) {
      PHINode *PN = dyn_cast<PHINode>(&*BI);
      if (!PN)
        break;
      for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
        if (PN->getIncomingBlock(i) == JBB)
          PN->setIncomingBlock(i, JBB2);
    }
  }
  TBB->eraseFromParent();
  FBB->eraseFromParent();
  JBB->eraseFromParent();

  // Further simplify a specific pattern in JBB2.
  for (auto BI = JBB2->begin(), BE = JBB2->end(); BI != BE; /*EMPTY*/) {
    Instruction *Inst = &*BI++;
    Value *LHS, *RHS;
    if (!match(Inst, m_Or(m_Value(LHS), m_Value(RHS))))
      continue;
    PHINode *PN = dyn_cast<PHINode>(RHS);
    if (!PN || PN->getNumIncomingValues() != 2)
      continue;
    Constant *One = dyn_cast<Constant>(PN->getIncomingValue(0));
    if (!One || !One->isOneValue())
      continue;
    Constant *Zero = dyn_cast<Constant>(PN->getIncomingValue(1));
    if (!Zero || !Zero->isNullValue())
      continue;
    PN->setIncomingValue(1, LHS);
    Inst->replaceAllUsesWith(PN);
    Inst->eraseFromParent();
  }

  return true;
}

bool AdvCodeMotion::runOnFunction(Function &F) {
  // Skip non-kernel function.
  MetaDataUtils *MDU = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
  auto FII = MDU->findFunctionsInfoItem(&F);
  if (FII == MDU->end_FunctionsInfo())
    return false;

  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  PDT = &getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  WI = &getAnalysis<WIAnalysis>();

  WIS.collect(&F);
  if (!WIS.hasOneDim())
    return false;

  assert(WIS.LocalSize.X && WIS.EnqueuedLocalSize.X && WIS.GlobalSize.X &&
         WIS.GroupId.X && WIS.GlobalOffset.X && WIS.LocalId.X &&
         WIS.GlobalId.X && "Missing necessary work-item settings");

  bool Changed = false;

  SmallVector<std::tuple<bool, BasicBlock *, BasicBlock *, BasicBlock *,
                         BasicBlock *>, 8> Candidates;
  for (auto DFI = df_begin(DT->getRootNode()),
            DFE = df_end(DT->getRootNode()); DFI != DFE; ++DFI) {
    // Record if-endif structure.
    auto *IfBB = (*DFI)->getBlock();
    bool InvPred;
    BasicBlock *TBB, *FBB, *JBB;
    std::tie(InvPred, TBB, FBB, JBB) = getIfStatementBlock(PDT, IfBB);
    if (!JBB)
      continue;
    Candidates.push_back(std::make_tuple(InvPred, IfBB, TBB, FBB, JBB));
  }

  while (!Candidates.empty()) {
    bool InvPred;
    BasicBlock *IfBB, *TBB, *FBB, *JBB;
    std::tie(InvPred, IfBB, TBB, FBB, JBB) = Candidates.pop_back_val();
    auto Cond = cast<BranchInst>(IfBB->getTerminator())->getCondition();
    if (!isUniformlyAlwaysTaken(InvPred, Cond))
      continue;
    bool LocalChanged = false;
    if ((ControlMask & 1) != 0 && isMostlyTaken(InvPred, Cond))
      LocalChanged |= hoistMost2(InvPred, IfBB, TBB, FBB, JBB);
    // Hoist uniform instructions from TBB into IfBB.
    // TODO: Hoist uniform loads only.
    if (!LocalChanged)
      LocalChanged |= hoistUniform(TBB, IfBB);
    Changed |= LocalChanged;
  }

  return Changed;
}

bool AdvCodeMotion::isCase1(bool InvPred, Value *Cond) const {
  // Case 1: v != gid and v is recurrent value ranging from 0 to
  // global_size(0).
  ICmpInst::Predicate Pred = InvPred ? ICmpInst::ICMP_EQ : ICmpInst::ICMP_NE;
  ICmpInst *ICmp = dyn_cast<ICmpInst>(Cond);
  if (!ICmp || ICmp->getPredicate() != Pred)
    return false;
  Value *LHS = ICmp->getOperand(0);
  Value *RHS = ICmp->getOperand(1);
  if (LHS != WIS.GlobalId.X) std::swap(LHS, RHS);
  if (LHS != WIS.GlobalId.X)
    return false;
  auto Inst = dyn_cast<Instruction>(RHS);
  if (!Inst)
    return false;

  const SCEVAddRecExpr *Exp = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(Inst));
  // Skip non-affine expression.
  if (!Exp || !Exp->isAffine())
    return false;
  const SCEV *InnerStep = Exp->getStepRecurrence(*SE);
  if (!InnerStep->isOne())
    return false;
  Loop *InL = const_cast<Loop *>(Exp->getLoop());
  BasicBlock *InnerExit = InL->getExitingBlock();
  if (!InnerExit)
    return false;
  const SCEVUnknown *InnerCount =
      dyn_cast<SCEVUnknown>(SE->getAddExpr(SE->getExitCount(InL, InnerExit),
                                           InnerStep));
  if (!InnerCount || InnerCount->getValue() != WIS.LocalSize.X)
    return false;
  // Inner loop ranges from Start to Start + LocalSizeX with step of 1.
  const SCEVAddRecExpr *InnerInit = dyn_cast<SCEVAddRecExpr>(Exp->getStart());
  if (!InnerInit || !InnerInit->getStart()->isZero())
    return false;
  const SCEVUnknown *OuterStep =
      dyn_cast<SCEVUnknown>(InnerInit->getStepRecurrence(*SE));
  if (!OuterStep || OuterStep->getValue() != WIS.LocalSize.X)
    return false;
  Loop *OutL = const_cast<Loop *>(InnerInit->getLoop());
  BasicBlock *OuterExit = OutL->getExitingBlock();
  if (!OuterExit)
    return false;
  auto Cond2 = cast<BranchInst>(OuterExit->getTerminator())->getCondition();
  auto OuterCmp = dyn_cast<ICmpInst>(Cond2);
  if (!OuterCmp || OuterCmp->getPredicate() != CmpInst::ICMP_ULT)
    return false;
  auto LHS2 = dyn_cast<AddOperator>(OuterCmp->getOperand(0));
  if (!LHS2 || SE->getSCEV(LHS2) != InnerInit->getPostIncExpr(*SE))
    return false;
  if (LHS2->getOperand(1) != WIS.LocalSize.X)
    return false;
  auto RHS2 = OuterCmp->getOperand(1);
  if (RHS2 != WIS.GlobalSize.X)
    return false;
  // Outer loop (i) ranges from 0 to GlobalSizeX with step of LocalSizeX.
  // Since OCL spec states that
  // """
  // If local_work_size is specified, the values specified in
  // global_work_size[0],... global_work_size[work_dim - 1] must be evenly
  // divisable by the corresponding values specified in local_work_size[0],...
  // local_work_size[work_dim - 1]
  // """
  // Plus, inner loop (j) ranges from j to j + LocalSizeX with step of 1. Thus,
  // (i+j) ranges from 0 to GlobalSizeX with step of 1. It could be confirmed
  // that 'Inst' has a range from inclusive 0 to exclusive GlobalSizeX. In
  // other word, GlobalIdX is in range of 'Inst' and any((i + j) != GlobalIdX)
  // is always true if the minimal subgroup size is greater than 1.
  return true;
}

bool AdvCodeMotion::isUniformlyAlwaysTaken(bool InvPred, Value *Cond) const {
  if (isCase1(InvPred, Cond))
    return true;
  return false;
}

bool AdvCodeMotion::isMostlyTaken(bool InvPred, Value *Cond) const {
  // For condition (i + j) != GlobalIdX, except the work-item with that global
  // ID, all other work-items evaluate that condition as true. For each SIMD
  // dispatch, maximally only one SIMD lane will be disabled.
  if (isCase1(InvPred, Cond))
    return true;
  return false;
}