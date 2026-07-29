#include "DebugInfoParser.hpp"
#include "Config.h"
#include "FileLogger.h"

#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/ObjTypeInference.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/TypedPointerType.h>
#include <llvm/Support/Casting.h>
#include <unordered_map>

using namespace llvm;

namespace liberator {
struct_padding_info_t::struct_padding_info_t(
    llvm::ArrayRef<byte_range_t> &ranges)
    : padding_byte_ranges_(ranges) {}

struct_padding_info_t::struct_padding_info_t(
    const llvm::DICompositeType *type) {
  auto size = type->getSizeInBits() / 8;

  struct field_info_t {
    unsigned offset;
    unsigned size;
  };

  SmallVector<field_info_t, 8> field_info;

  for (const Metadata *md : type->getElements()) {
    if (!md)
      continue;
    const auto *derived = dyn_cast<DIDerivedType>(md);
    if (!derived || derived->getTag() != dwarf::DW_TAG_member)
      continue;
    unsigned offset = derived->getOffsetInBits() / 8;
    unsigned size = derived->getSizeInBits() / 8;
    field_info.emplace_back(offset, size);
  }

  if (field_info.empty())
    return;

  sort(field_info, [](const field_info_t &f1, const field_info_t &f2) -> bool {
    return f1.offset < f2.offset;
  });

  unsigned curr_offset = 0;
  for (const auto &field : field_info) {
    if (field.offset > curr_offset)
      padding_byte_ranges_.emplace_back(curr_offset, field.offset);
    curr_offset = field.offset + field.size;
  }
  if (curr_offset < size)
    padding_byte_ranges_.emplace_back(curr_offset, size);
}

std::string struct_padding_info_t::to_string() const { return ""; }

std::unordered_map<llvm::StructType *, struct_padding_info_t>
debug_info_parser_t::get_struct_info_padding(const llvm::Module &M) {
  std::unordered_map<llvm::StructType *, struct_padding_info_t> res;
  SmallVector<DICompositeType *, 32> di_composite_type;

  DebugInfoFinder finder;

  finder.processModule(M);

  for (auto scope : finder.types()) {
    if (auto *composite_type = dyn_cast<DICompositeType>(scope)) {
      di_composite_type.push_back(composite_type);
    }
  }

  SmallDenseSet<DICompositeType *> visited;
  LLVMContext &context = M.getContext();
  const DataLayout &layout = M.getDataLayout();
  for (auto composite : di_composite_type) {
    insert_padding_info(composite, res, visited, context, layout);
  }

  return res;
}
void debug_info_parser_t::insert_padding_info(
    llvm::DICompositeType *type,
    std::unordered_map<llvm::StructType *, struct_padding_info_t>
        &padding_info_map,
    llvm::SmallDenseSet<llvm::DICompositeType *> &visited,
    llvm::LLVMContext &ctx, const llvm::DataLayout &dl,
    llvm::StructType *struct_type) {
  if (!visited.insert(type).second)
    return;

  if (!struct_type) {
    StringRef name = type->getName();
    if (!name.empty()) {
      std::string prefix;
      switch (type->getTag()) {
      case dwarf::DW_TAG_class_type:
      case dwarf::DW_TAG_structure_type:
      case dwarf::DW_TAG_union_type:
      case dwarf::DW_TAG_enumeration_type:
      default:
        llvm_unreachable("Not known DWARF type");
      }
      struct_type = StructType::getTypeByName(ctx, name);
    }
  }

  if (!struct_type)
    return;

  struct_padding_info_t struct_padding_info(type);
  padding_info_map.insert({struct_type, struct_padding_info});
  auto padding_ranges = struct_padding_info.get_padding_ranges();
  const StructLayout *struct_layout = dl.getStructLayout(struct_type);
  unsigned field_idx = 0;
  auto is_padding = [&padding_ranges,
                     &struct_layout](unsigned field_idx) -> bool {
    return std::ranges::any_of(
        padding_ranges,
        [&struct_layout,
         field_idx](const struct_padding_info_t::byte_range_t &range) -> bool {
          return struct_layout->getElementOffset(field_idx) == range.first;
        });
  };

  for (DINode *node : type->getElements()) {
    if (!node || (!isa<DICompositeType>(node) && !isa<DIDerivedType>(node)))
      continue;
    StructType *child_struct_type = nullptr;
    while (is_padding(field_idx))
      field_idx++;

    if (struct_type)
      child_struct_type =
          dyn_cast<StructType>(struct_type->getElementType(field_idx));

    if (auto *nested_di_composite_type = dyn_cast<DICompositeType>(node)) {
      insert_padding_info(nested_di_composite_type, padding_info_map, visited,
                          ctx, dl, child_struct_type);
    }
    if (auto *derived_di_type = dyn_cast<DIDerivedType>(node)) {
      if (auto *base_type = derived_di_type->getBaseType()) {
        if (auto *composite_di_type = dyn_cast<DICompositeType>(base_type))
          insert_padding_info(composite_di_type, padding_info_map, visited, ctx,
                              dl, child_struct_type);
      }
      field_idx++;
    }
  }
}

llvm::Type *infer_type_from_arg_attrs(const llvm::Argument *arg) {
  if (!arg->getType()->isPointerTy())
    return nullptr;
  // When the struct type is passed by value but is to big
  if (auto *T = arg->getParamByValType())
    return T;
  return nullptr;
}

// Accept the record's variable only if it is a *parameter* of `sp`
// (getArg() != 0), and not something pulled in from an inlined callee.
static llvm::DILocalVariable *param_of(llvm::DbgVariableRecord *DVR,
                                       const llvm::DISubprogram *sp) {
  llvm::DILocalVariable *var = DVR->getVariable();
  if (!var || var->getArg() == 0)
    return nullptr;
  if (llvm::getDISubprogram(var->getScope()) != sp)
    return nullptr;
  return var;
}

llvm::DIType *restore_param_di_type(const llvm::Value *v) {
  const auto *arg = dyn_cast<Argument>(v);
  if (!arg)
    return nullptr;
  auto sp = arg->getParent()->getSubprogram();
  if (!sp || !sp->getType())
    return nullptr;
  auto arr_types = sp->getType()->getTypeArray();
  if (!arr_types)
    return nullptr;

  DILocalVariable *found = nullptr;

  // find the store to the parameter because in -O0, parameters are stored on
  // the stack. And LLVM does not store types for arguments on this optimization
  // level. The only way to find the corresponding DILocalVariable of the
  // argument is through finding the alloca instruction where it is saved.
  // With the corresponding DbgVariableRecord we have access to the ArgNo in the
  // signature.
  for (auto u : arg->users()) {
    auto store_inst = dyn_cast<StoreInst>(u);
    if (!store_inst || store_inst->getValueOperand() != arg)
      continue;
    auto alloc_inst = dyn_cast<AllocaInst>(
        getUnderlyingObject(store_inst->getPointerOperand()));
    if (!alloc_inst)
      continue;
    for (auto dbg_record :
         findDVRDeclares(const_cast<AllocaInst *>(alloc_inst))) {
      if ((found = param_of(dbg_record, sp)))
        break;
    }
    if (found)
      break;
  }

  if (found && found->getArg() < arr_types.size()) {
    return arr_types[found->getArg()];
  }
  unsigned idx = arg->getArgNo() + 1;
  return idx < arr_types.size() ? arr_types[idx] : nullptr;
}

llvm::DIType *peel_di_qualifiers(llvm::DIType *t) {
  using namespace llvm::dwarf;
  if (!t) {
    TYPE_LOG("peel_di_qualifiers: null input\n");
    return nullptr;
  }
  /*std::string s;
  raw_string_ostream os(s);
  t->printTree(os);
  TYPE_LOG("{}", s);*/
  while (auto *d = llvm::dyn_cast_or_null<llvm::DIDerivedType>(t)) {
    auto tag = d->getTag();
    if (tag != DW_TAG_typedef && tag != DW_TAG_const_type &&
        tag != DW_TAG_volatile_type && tag != DW_TAG_restrict_type &&
        tag != DW_TAG_atomic_type)
      break;
    t = d->getBaseType();
  }
  return t;
}

// resolve function pointers
llvm::Type *resolve_di_type_to_llvm(llvm::DIType *di, llvm::Module &mod) {
  using namespace llvm::dwarf;
  auto &ctx = mod.getContext();

  if (!di)
    return llvm::Type::getVoidTy(ctx);

  /*std::string s;
  raw_string_ostream os(s);
  di->printTree(os);
  TYPE_LOG("{}", s);*/
  di = peel_di_qualifiers(di);
  if (!di)
    return llvm::Type::getVoidTy(ctx);

  // for function pointers
  if (auto *sr = llvm::dyn_cast<llvm::DISubroutineType>(di)) {
    TYPE_LOG("Found a function pointer\n");
    auto types = sr->getTypeArray();
    if (types.size() == 0)
      return nullptr;
    // resolve return type
    llvm::Type *ret = resolve_di_type_to_llvm(types[0], mod);
    if (!ret)
      ret = llvm::Type::getVoidTy(ctx);
    // resolve param types
    std::vector<llvm::Type *> params;
    for (unsigned i = 1; i < types.size(); ++i) {
      llvm::Type *p = resolve_di_type_to_llvm(types[i], mod);
      if (!p)
        return nullptr;
      params.push_back(p);
    }
    // create the function type signature
    return llvm::FunctionType::get(ret, params, false);
  }

  // TODO: maybe use something better then linear search for finding the correct
  // struct
  // for struct types and union and classes
  if (auto *comp = llvm::dyn_cast<llvm::DICompositeType>(di)) {
    auto tag = comp->getTag();
    if (tag == DW_TAG_structure_type || tag == DW_TAG_class_type ||
        tag == DW_TAG_union_type) {
      auto name = comp->getName().str();
      if (name.empty())
        return nullptr;
      // iterate each struct type defined in the module.
      for (auto *ST : mod.getIdentifiedStructTypes()) {
        if (ST->getName() == name || ST->getName() == "struct." + name ||
            ST->getName() == "union." + name ||
            ST->getName() == "class." + name)
          return ST;
      }
      return nullptr;
    }
  }

  if (auto *d = llvm::dyn_cast<llvm::DIDerivedType>(di)) {
    auto tag = d->getTag();
    if (tag == DW_TAG_pointer_type || tag == DW_TAG_reference_type ||
        tag == DW_TAG_rvalue_reference_type) {
      llvm::Type *pointee = resolve_di_type_to_llvm(d->getBaseType(), mod);
      if (!pointee || pointee->isVoidTy())
        pointee = llvm::Type::getInt8Ty(ctx);
      return llvm::TypedPointerType::get(pointee, 0);
    }
  }

  // get the correct basic type (int, float, short, double, long, char)
  if (auto *b = llvm::dyn_cast<llvm::DIBasicType>(di)) {
    auto bits = b->getSizeInBits();
    if (bits == 0)
      return llvm::Type::getVoidTy(ctx);
    switch (b->getEncoding()) {
    case DW_ATE_boolean:
      return llvm::Type::getInt1Ty(ctx);
    case DW_ATE_signed:
    case DW_ATE_unsigned:
    case DW_ATE_signed_char:
    case DW_ATE_unsigned_char:
    case DW_ATE_UTF:
      return llvm::IntegerType::get(ctx, bits);
    case DW_ATE_float:
      if (bits == 16)
        return llvm::Type::getHalfTy(ctx);
      if (bits == 32)
        return llvm::Type::getFloatTy(ctx);
      if (bits == 64)
        return llvm::Type::getDoubleTy(ctx);
      if (bits == 80)
        return llvm::Type::getX86_FP80Ty(ctx);
      if (bits == 128)
        return llvm::Type::getFP128Ty(ctx);
      return nullptr;
    default:
      return nullptr;
    }
  }

  return nullptr;
}

// Restore the llvm type for a param
const llvm::Type *restore_llvm_type(const llvm::Value *param) {
  auto *llvm_module_set = SVF::LLVMModuleSet::getLLVMModuleSet();

  const llvm::Type *seek_type = nullptr;
  if (auto *type_inference = llvm_module_set->getTypeInference())
    // will return i8 as default for heap objects and ptr for stack/global ->
    // then fallback
    seek_type = type_inference->inferObjType(param);

  bool inference_failed =
      !seek_type || seek_type->isPointerTy() || seek_type->isIntegerTy(8);
  if (!inference_failed) {
    TYPE_LOG("SVF type inference found type!\n");
    return seek_type;
  }

  const auto *arg = llvm::dyn_cast<llvm::Argument>(param);
  if (arg) {
    TYPE_LOG("Argument name: {}\n", arg->getName());
    if (auto *attr_type = infer_type_from_arg_attrs(arg)) {
      TYPE_LOG("Recovered pointee type from Argument attribute.\n");
      return attr_type;
    }

    TYPE_LOG("Couldn't deduce pointer type using SVF or attributes.\n "
             "Falling back to DWARF.\n");
    // Get the DWARF function representation
    if (llvm::DISubprogram *SP = arg->getParent()->getSubprogram()) {
      if (llvm::DISubroutineType *STy = SP->getType()) {
        // here we get [return_type, param1_type, param2_type, ...];
        llvm::DITypeRefArray type_array = STy->getTypeArray();

        // Get argument index + 1, because of return
        unsigned array_index = arg->getArgNo() + 1;
        if (array_index < type_array.size() && type_array[array_index]) {
          // Get type array index
          auto *param_di_type = type_array[array_index];
          auto *llvm_module = llvm_module_set->getMainLLVMModule();
          // if the function parameter is a function pointer itself
          if (auto *resolved =
                  resolve_di_type_to_llvm(param_di_type, *llvm_module)) {
            // resolve_di_type_to_llvm returns the parameter's own type; the
            // access trackers compare against GEP source element types, so
            // strip the outer pointer wrap down to the pointee.
            if (auto *tp = llvm::dyn_cast<llvm::TypedPointerType>(resolved)) {
              TYPE_LOG("Resolved DWARF source type.\n");
              return tp->getElementType();
            }
            if (!resolved->isVoidTy()) {
              TYPE_LOG("Resolved DWARF source type.\n");
              return resolved;
            }
          }
        }
      }
    }
  }

  if (auto *use_type = infer_type_from_forward_uses(param)) {
    TYPE_LOG("Recovered pointee type from forward use scan.\n");
    return use_type;
  }

  return seek_type;
}

llvm::DIType *decay_di_type(llvm::DIType *t) {
  t = peel_di_qualifiers(t);
  // dyn_cast_or_null because a void* has no pointee: its DW_TAG_pointer_type
  // entry carries a null base type.
  while (auto *tmp = llvm::dyn_cast_or_null<llvm::DIDerivedType>(t)) {
    auto tag = tmp->getTag();
    // Stop as soon as the type is none of the indirections: the condition has
    // to be a conjunction, a tag can never equal two of them at once.
    if (tag != llvm::dwarf::DW_TAG_pointer_type &&
        tag != llvm::dwarf::DW_TAG_reference_type &&
        tag != llvm::dwarf::DW_TAG_rvalue_reference_type)
      break;
    // Descend to the pointee, otherwise we would re-inspect the same node.
    t = peel_di_qualifiers(tmp->getBaseType());
  }

  return t;
}

llvm::StringRef canonical_name(const StructType *st) {
  auto name_ref = st->getName();
  // StructType can have no name
  if (name_ref.empty())
    return {};
  // name could be:
  // struct.[struct name]
  // class.[class name]
  // union.[class name]
  // remove the prefix:
  name_ref.consume_front("struct.");
  name_ref.consume_front("union.");
  name_ref.consume_front("class.");

  // Left could be something like [struct name].1.2.4 (multiple mergings with
  // different TUs) Because LLVM has to make sure that struct stays unique for
  // different TUs when linking. For example module1 contains struct definition
  // struct A but module2 also contains struct A, but for some reason the differ
  // in their definition, then when merging them into one, LLVM creates the
  // definition A.1 and A.2.
  // Because dots in C++ struct names are not allowed, we can just search for
  // the leftmost dot here.
  auto pos = name_ref.find('.');
  if (pos != StringRef::npos) {
    name_ref = name_ref.substr(0, pos);
  }

  return name_ref;
}

bool compare_types(llvm::DIType *di, const llvm::Type *type,
                   const llvm::DataLayout &dl) {
  llvm::DIType *tmp = decay_di_type(di);
  if (!tmp || !type)
    return false;

  if (auto st = dyn_cast<StructType>(type)) {
    auto llvm_name = canonical_name(st);
    // Has to be the decayed type: a `struct A *` parameter carries a
    // DW_TAG_pointer_type, and pointer entries have no name of their own.
    auto di_name = tmp->getName();
    if (!llvm_name.empty() && !di_name.empty())
      return llvm_name == di_name;
  }
  // there can also be anonymous composites like in struct A { struct {int x,
  // int y} f1;};. Then we can not compare the names but have to rely on size
  // comparisons.
  if (type->isSized() && tmp->getSizeInBits() != 0)
    return dl.getTypeAllocSizeInBits(const_cast<llvm::Type *>(type)) ==
           tmp->getSizeInBits();
  return false;
}

llvm::DIType *next_di_field(llvm::DIType *di, const llvm::Type *container,
                            uint64_t idx, const llvm::Type *result,
                            const llvm::DataLayout &dl) {
  llvm::DIType *res = decay_di_type(di);
  if (!res)
    return nullptr;

  if (auto *st = dyn_cast<StructType>(container)) {
    auto comp = dyn_cast<DICompositeType>(res);
    if (!comp)
      return nullptr;
    // check tag - conjunction again, the tag can only ever be one of them
    auto tag = comp->getTag();
    if (tag != llvm::dwarf::DW_TAG_union_type &&
        tag != llvm::dwarf::DW_TAG_structure_type &&
        tag != llvm::dwarf::DW_TAG_class_type)
      return nullptr;

    if (st->isOpaque() || idx >= st->getNumElements())
      return nullptr;

    uint64_t target_off_bits =
        dl.getStructLayout(const_cast<llvm::StructType *>(st))
            ->getElementOffsetInBits(idx);

    llvm::DIDerivedType *tmp = nullptr;
    // Code searches for best fitting DIType for the GEP offset in bits.
    // For example for anonymous structs: struct A { int f1; struct {int a, b;};
    // }; where b is to be accessed, the GEP would have an index of 2 but in
    // DWARF we only have index 0 and 1 because 0 is the int (f1) and 1 is the
    // anonymous struct -> this would fail if we search for offset 64 directly.
    // So instead we search for the offset that best fits our target offset. The
    // offset of b is at 64 bits in the GEP. In dwarf the first field (f1) has a
    // zero offset, then comes the anonymous struct as a member at offset 32.
    // Because 32 <= 64 we update type to the anonymous struct and finish.
    for (auto *el : comp->getElements()) {
      auto mem = llvm::dyn_cast<DIDerivedType>(el);
      if (!mem || mem->getTag() != dwarf::DW_TAG_member)
        continue;
      uint64_t off = mem->getOffsetInBits();
      if (off <= target_off_bits && (!tmp || off > tmp->getOffsetInBits()))
        tmp = mem;
    }
    return tmp ? tmp->getBaseType() : nullptr;
  }

  if (auto *comp = llvm::dyn_cast<DICompositeType>(res)) {
    if (comp->getTag() == llvm::dwarf::DW_TAG_array_type)
      return comp->getBaseType();
  }

  return nullptr;
}

llvm::Type *infer_type_from_forward_uses(const llvm::Value *param) {
  llvm::Type *best_struct = nullptr;
  llvm::Type *fallback = nullptr;
  for (const llvm::User *U : param->users()) {
    if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(U)) {
      if (gep->getPointerOperand() != param)
        continue;
      llvm::Type *src = gep->getSourceElementType();
      std::string str;
      raw_string_ostream os(str);
      src->print(os);
      TYPE_LOG("Found source element type: {}\n", str);
      if (src && src->isStructTy()) {
        if (!best_struct)
          best_struct = src;
      } else if (!fallback) {
        fallback = src;
      }
    } else if (auto *ld = llvm::dyn_cast<llvm::LoadInst>(U)) {
      if (ld->getPointerOperand() == param && !fallback)
        fallback = ld->getType();
    } else if (auto *st = llvm::dyn_cast<llvm::StoreInst>(U)) {
      if (st->getPointerOperand() == param && !fallback)
        fallback = st->getValueOperand()->getType();
    }
  }
  return best_struct ? best_struct : fallback;
}
} // namespace liberator
