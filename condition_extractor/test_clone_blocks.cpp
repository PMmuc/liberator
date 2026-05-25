#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <iostream>

using namespace llvm;

int main() {
    LLVMContext Ctx;
    Module *OldMod = new Module("Old", Ctx);
    Module *NewMod = new Module("New", Ctx);

    FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
    Function *OldFunc = Function::Create(FT, Function::ExternalLinkage, "old_func", OldMod);
    
    // Add a basic block and a return instruction to OldFunc
    BasicBlock *BB = BasicBlock::Create(Ctx, "entry", OldFunc);
    IRBuilder<> Builder(BB);
    Builder.CreateRetVoid();

    // Create new func with nullptr
    Function *NewFunc = Function::Create(FT, Function::ExternalLinkage, "new_func", nullptr);
    std::cout << "NewFunc parent is: " << NewFunc->getParent() << std::endl;

    llvm::ValueToValueMapTy VMap;
    SmallVector<ReturnInst *, 8> Returns;
    
    std::cout << "Calling CloneFunctionInto..." << std::endl;
    // this mimics SVF's call
    CloneFunctionInto(NewFunc, OldFunc, VMap, llvm::CloneFunctionChangeType::LocalChangesOnly, Returns, "", nullptr);

    std::cout << "Success!" << std::endl;
    return 0;
}
