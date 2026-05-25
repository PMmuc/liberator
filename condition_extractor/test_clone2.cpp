#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <iostream>

using namespace llvm;

int main() {
    LLVMContext Ctx;
    Module *OldMod = new Module("Old", Ctx);
    Module *NewMod = new Module("New", Ctx);

    FunctionType *FT = FunctionType::get(Type::getVoidTy(Ctx), false);
    Function *OldFunc = Function::Create(FT, Function::ExternalLinkage, "old_func", OldMod);
    
    // Create new func with NewMod!
    Function *NewFunc = Function::Create(FT, Function::ExternalLinkage, "new_func", NewMod);
    std::cout << "NewFunc parent is: " << NewFunc->getParent() << std::endl;

    llvm::ValueToValueMapTy VMap;
    SmallVector<ReturnInst *, 8> Returns;
    
    std::cout << "Calling CloneFunctionInto..." << std::endl;
    // this mimics SVF's call but with parent already set
    CloneFunctionInto(NewFunc, OldFunc, VMap, llvm::CloneFunctionChangeType::LocalChangesOnly, Returns, "", nullptr);

    std::cout << "Success!" << std::endl;
    return 0;
}
