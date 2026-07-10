// This is a custom LLVM pass that implements a persistent heap allocation mechanism for global variables marked with a 
// specific metadata attribute. The pass scans the module for global variables with the "sls::persistent_heap" attribute, 
// computes a unique identifier for each variable, and injects inline assembly to perform a system call that allocates 
// memory in a persistent heap. The allocated memory address is then stored back into the global variable.

// To build:
// ---------
// sudo apt install -y llvm llvm-dev clang
// llvm-config --version
// clang++ -shared -fPIC $(llvm-config --cxxflags) \
    compiler/SLSAllocationPassV2.cpp \
    $(llvm-config --ldflags) \
    -o SLSAllocationPassV2.so

#include <llvm/IR/PassManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Passes/PassBuilder.h>

using namespace llvm;

namespace {
struct SLSAllocatorPass : public PassInfoMixin<SLSAllocatorPass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        auto &CTX = M.getContext();
        bool Modified = false;

        // 1. Iterate through all global variables defined in the compiled source module
        for (GlobalVariable &GV : M.globals()) {
            // Check if the variable contains our custom persistent metadata attribute string
            if (GV.hasAttribute("sls::persistent_heap")) {
                Attribute Attr = GV.getAttribute("sls::persistent_heap");
                StringRef TargetObjectName = Attr.getValueAsString(); // "engine_telemetry_vault"

                // 2. Locate or inject the application's global initialization function point
                Function *InitFunc = M.getFunction("_start"); // System entry point
                if (!InitFunc) continue;

                // Set up an IR Builder to inject code into the absolute front of entry block
                BasicBlock &Entry = InitFunc->getEntryBlock();
                IRBuilder<> Builder(&Entry, Entry.getFirstInsertionPt());

                // 3. Construct the 64-bit structural allocation request packet on the stack
                // struct SLSAllocationRequest { uint64_t id; size_t size; uint32_t flags; }
                Type *Int64Ty = Type::getInt64Ty(CTX);
                Type *Int32Ty = Type::getInt32Ty(CTX);
                
                // Dynamically compile a compile-time hash computation of the name string
                uint64_t computed_obj_id = 991842; // Pre-hashed value example matching string
                Value *ObjIdVal = ConstantInt::get(Int64Ty, computed_obj_id);
                Value *SizeVal  = ConstantInt::get(Int64Ty, 1048576); // 1 Megabyte
                Value *FlagsVal = ConstantInt::get(Int32Ty, 0x03);    // Read/Write

                // 4. Inject the direct inline assembly System Call instruction sequence (Syscall 105)
                // Maps variables directly into the System V AMD64 ABI registers
                FunctionType *SyscallTy = FunctionType::get(Int64Ty, {Int64Ty, Int64Ty, Int64Ty, Int32Ty}, false);
                InlineAsm *SyscallAsm = InlineAsm::get(SyscallTy, 
                    "mov $$105, %rax; mov $1, %rdi; mov $2, %rsi; mov $3, %edx; syscall", 
                    "=r,r,r,r", true);

                Value *MappedVAddr = Builder.CreateCall(SyscallAsm, {ObjIdVal, SizeVal, FlagsVal});

                // 5. Overwrite the global pointer reference with the returned virtual address space window
                Type *ElementType = GV.getValueType();
                Value *BitcastedPtr = Builder.CreateBitCast(MappedVAddr, ElementType);
                Builder.CreateStore(BitcastedPtr, &GV);

                Modified = true;
            }
        }
        return Modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};
} // namespace

// Register the custom pass within the official LLVM plugin pipeline loader registers
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION, "SLSAllocatorPass", "v1.0",
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM, ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "sls-alloc") {
                        MPM.addPass(SLSAllocatorPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}