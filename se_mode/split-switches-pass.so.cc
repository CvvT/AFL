/*
 * Copyright 2016 laf-intel
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/ValueTracking.h"

#include <set>

using namespace llvm;

namespace {

  class SplitSwitchesTransform : public ModulePass {

    public:
      static char ID;
      SplitSwitchesTransform() : ModulePass(ID) {
      } 

      bool runOnModule(Module &M) override;

      // const char *getPassName() const override {
      //   return "splits switch constructs";
      // }

      struct CaseExpr {
        ConstantInt* Val;
        BasicBlock* BB;

        CaseExpr(ConstantInt *val = nullptr, BasicBlock *bb = nullptr) :
          Val(val), BB(bb) { }
      };

    typedef std::vector<CaseExpr> CaseVector;

    private:
      bool splitSwitches(Module &M);
      bool transformCmps(Module &M, const bool processStrcmp, const bool processMemcmp);
      BasicBlock* switchConvert(CaseVector Cases, std::vector<bool> bytesChecked,
                                BasicBlock* OrigBlock, BasicBlock* NewDefault,
                                Value* Val, unsigned level);
  };

}

char SplitSwitchesTransform::ID = 0;


/* switchConvert - Transform simple list of Cases into list of CaseRange's */
BasicBlock* SplitSwitchesTransform::switchConvert(CaseVector Cases, std::vector<bool> bytesChecked, 
                                            BasicBlock* OrigBlock, BasicBlock* NewDefault,
                                            Value* Val, unsigned level) {

  Function* F = OrigBlock->getParent();
  BasicBlock* NextNode = NewDefault;
  for (int i = Cases.size() - 1; i >= 0; --i) {
    auto &Case = Cases[i];
    BasicBlock* NewNode = BasicBlock::Create(Val->getContext(), "SwitchBlock", F);
    ICmpInst* Comp = new ICmpInst(ICmpInst::ICMP_EQ, Val, Case.Val, "SwitchEq");
    NewNode->getInstList().push_back(Comp);
    BranchInst::Create(Case.BB, NextNode, Comp, NewNode);
    NextNode = NewNode;

    // we have to update the phi nodes
    for (BasicBlock::iterator I = Case.BB->begin(); I != Case.BB->end(); ++I) {
      if (!isa<PHINode>(&*I))
        continue;
      PHINode *PN = cast<PHINode>(I);

      for (unsigned idx = 0; idx != PN->getNumIncomingValues(); ++idx) {
        if (PN->getIncomingBlock(idx) == OrigBlock) {
          PN->setIncomingBlock(idx, NewNode);
          break;
        }
      }
    }

  }
  return NextNode;
}

bool SplitSwitchesTransform::splitSwitches(Module &M) {

  std::vector<SwitchInst*> switches;

  /* iterate over all functions, bbs and instruction and add
   * all switches to switches vector for later processing */
  for (auto &F : M) {
    for (auto &BB : F) {
      SwitchInst* switchInst = nullptr;

      if ((switchInst = dyn_cast<SwitchInst>(BB.getTerminator()))) {
        if (switchInst->getNumCases() < 1)
            continue;
          switches.push_back(switchInst);
      }
    }
  }

  if (!switches.size())
    return false;
  errs() << "Rewriting " << switches.size() << " switch statements " << "\n";

  for (auto &SI: switches) {

    BasicBlock *CurBlock = SI->getParent();
    BasicBlock *OrigBlock = CurBlock;
    Function *F = CurBlock->getParent();
    /* this is the value we are switching on */
    Value *Val = SI->getCondition();
    BasicBlock* Default = SI->getDefaultDest();

    /* If there is only the default destination, don't bother with the code below. */
    if (!SI->getNumCases()) {
      continue;
    }

    /* Create a new, empty default block so that the new hierarchy of
     * if-then statements go to this and the PHI nodes are happy.
     * if the default block is set as an unreachable we avoid creating one
     * because will never be a valid target.*/
    BasicBlock *NewDefault = nullptr;
    NewDefault = BasicBlock::Create(SI->getContext(), "NewDefault");
    NewDefault->insertInto(F, Default);
    BranchInst::Create(Default, NewDefault);


    /* Prepare cases vector. */
    CaseVector Cases;
    for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end(); i != e; ++i)
      Cases.push_back(CaseExpr(i->getCaseValue(), i->getCaseSuccessor()));
    
    std::vector<bool> bytesChecked(Cases[0].Val->getBitWidth() / 8, false);
    BasicBlock* SwitchBlock = switchConvert(Cases, bytesChecked, OrigBlock, NewDefault, Val, 0);

    /* Branch to our shiny new if-then stuff... */
    BranchInst::Create(SwitchBlock, OrigBlock);

    /* We are now done with the switch instruction, delete it. */
    CurBlock->getInstList().erase(SI);


   /* we have to update the phi nodes! */
   for (BasicBlock::iterator I = Default->begin(); I != Default->end(); ++I) {
     if (!isa<PHINode>(&*I)) {
      continue;
     }
     PHINode *PN = cast<PHINode>(I);

     /* Only update the first occurence. */
     unsigned Idx = 0, E = PN->getNumIncomingValues();
     for (; Idx != E; ++Idx) {
       if (PN->getIncomingBlock(Idx) == OrigBlock) {
         PN->setIncomingBlock(Idx, NewDefault);
         break;
       }
     }
   }
 }

 verifyModule(M);
 return true;
}

bool SplitSwitchesTransform::runOnModule(Module &M) {

  llvm::errs() << "Running split-switches-pass by laf.intel@gmail.com\n"; 
  splitSwitches(M);
  verifyModule(M);

  return true;
}

static void registerSplitSwitchesTransPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  auto p = new SplitSwitchesTransform();
  PM.add(p);

}

static RegisterStandardPasses RegisterSplitSwitchesTransPass(
    PassManagerBuilder::EP_OptimizerLast, registerSplitSwitchesTransPass);

static RegisterStandardPasses RegisterSplitSwitchesTransPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerSplitSwitchesTransPass);

