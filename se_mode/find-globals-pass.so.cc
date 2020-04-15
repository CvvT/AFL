/*
 * @author Weiteng Chen
 */

#define AFL_FIND_GLOBALS_PASS
#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {
    class GlobalVariablePass : public ModulePass {
    public:
        static char ID;
        GlobalVariablePass() : ModulePass(ID) {}
        bool runOnModule(Module &M) override;
    };
}

char GlobalVariablePass::ID = 0;

bool GlobalVariablePass::runOnModule(Module &M) {
    printf("Passing %s\n", M.getName().str().c_str());
    for (GlobalVariable &G : M.globals()) {
        // printf("Get global variables: %s\n", G.getName().str().c_str());
        if (!G.isConstant())
            continue;

        // errs() << G << "\n";
        if (G.getInitializer() && !G.getInitializer()->isNullValue() && !G.hasPrivateLinkage()) {
            // errs() << "Linkage: " << G.getLinkage() << "\n";
            Type *GVTy = G.getValueType();
            if (ArrayType *ArrayTy = dyn_cast<ArrayType>(GVTy)) {
                const DataLayout &DL = G.getParent()->getDataLayout();
                uint64_t SizeInBytes = DL.getTypeStoreSize(GVTy);
                printf("Get global variables: %s\n", G.getName().str().c_str());
                printf("Find: %lu\n", SizeInBytes);
            }
        }
    }
    return false;
}

static void registerGlobalPass(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
    PM.add(new GlobalVariablePass());
}

static RegisterStandardPasses RegisterGlobalPass(PassManagerBuilder::EP_ModuleOptimizerEarly, registerGlobalPass);

static RegisterStandardPasses RegisterGlobalPass0(PassManagerBuilder::EP_EnabledOnOptLevel0, registerGlobalPass);

