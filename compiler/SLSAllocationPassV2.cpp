#include 
    "llvm/IR/PassManager.h"
    #include "llvm/IR/IRBuilder.h"
    #include "llvm/Passes/PassPlugin.h"
    #include "llvm/Passes/PassBuilder.h"
    #include "llvm/ADT/Triple.h"
    using namespace llvm;
    namespace {
        struct SLSAllocationPassV2 : public PassInfoMixin<SLSAllocationPassV2> {
            PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
                return PreservedAnalyses::all();
            }
        };
    }
    extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
        return { LLVM_PLUGIN_API_VERSION, "SLSAllocationPassV2", "v2.0", [](PassBuilder &) {} };
    }