/// \file
/// \brief This file implements the logic to translate a PTC instruction in to
///        LLVM IR.

// Standard includes
#include <cstdint>
#include <sstream>

// LLVM includes
#include "llvm/IR/CFG.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

// Local includes
#include "instructiontranslator.h"
#include "ir-helpers.h"
#include "ptcinterface.h"
#include "rai.h"
#include "range.h"
#include "transformadapter.h"
#include "variablemanager.h"

using namespace llvm;

namespace PTC {

  template<bool C>
  class InstructionImpl;

  enum ArgumentType {
    In,
    Out,
    Const
  };

  template<ArgumentType Type, bool IsCall>
  class InstructionArgumentsIterator :
    public RandomAccessIterator<uint64_t,
                                InstructionArgumentsIterator<Type, IsCall>,
                                false> {
  public:
    using base = RandomAccessIterator<uint64_t,
                                      InstructionArgumentsIterator,
                                      false>;

    InstructionArgumentsIterator&
    operator=(const InstructionArgumentsIterator& r) {
      base::operator=(r);
      TheInstruction = r.TheInstruction;
      return *this;
    }


    InstructionArgumentsIterator(const InstructionArgumentsIterator& r) :
      base(r),
      TheInstruction(r.TheInstruction) { }

    InstructionArgumentsIterator(const InstructionArgumentsIterator& r,
                                 unsigned Index) :
      base(Index),
      TheInstruction(r.TheInstruction) { }

    InstructionArgumentsIterator(PTCInstruction *TheInstruction,
                                 unsigned Index) :
      base(Index),
      TheInstruction(TheInstruction) { }

    bool isCompatible(const InstructionArgumentsIterator& r) const {
      return TheInstruction == r.TheInstruction;
    }

  public:
    uint64_t get(unsigned Index) const;

  private:
    PTCInstruction *TheInstruction;
  };

  template<>
  inline uint64_t
  InstructionArgumentsIterator<In, true>::get(unsigned Index) const {
    return ptc_call_instruction_in_arg(&ptc, TheInstruction, Index);
  }

  template<>
  inline uint64_t
  InstructionArgumentsIterator<Const, true>::get(unsigned Index) const {
    return ptc_call_instruction_const_arg(&ptc, TheInstruction, Index);
  }

  template<>
  inline uint64_t
  InstructionArgumentsIterator<Out, true>::get(unsigned Index) const {
    return ptc_call_instruction_out_arg(&ptc, TheInstruction, Index);
  }

  template<>
  inline uint64_t
  InstructionArgumentsIterator<In, false>::get(unsigned Index) const {
    return ptc_instruction_in_arg(&ptc, TheInstruction, Index);
  }

  template<>
  inline uint64_t
  InstructionArgumentsIterator<Const, false>::get(unsigned Index) const {
    return ptc_instruction_const_arg(&ptc, TheInstruction, Index);
  }

  template<>
  inline uint64_t
  InstructionArgumentsIterator<Out, false>::get(unsigned Index) const {
    return ptc_instruction_out_arg(&ptc, TheInstruction, Index);
  }

  template<bool IsCall>
  class InstructionImpl {
  private:
    template<ArgumentType Type>
    using arguments = InstructionArgumentsIterator<Type, IsCall>;
  public:
    InstructionImpl(PTCInstruction *TheInstruction) :
      TheInstruction(TheInstruction),
      InArguments(arguments<In>(TheInstruction, 0),
                  arguments<In>(TheInstruction, inArgCount())),
      ConstArguments(arguments<Const>(TheInstruction, 0),
                     arguments<Const>(TheInstruction, constArgCount())),
      OutArguments(arguments<Out>(TheInstruction, 0),
                   arguments<Out>(TheInstruction, outArgCount()))
    { }

    PTCOpcode opcode() const {
      return TheInstruction->opc;
    }

    std::string helperName() const {
      assert(IsCall);
      PTCHelperDef *Helper = ptc_find_helper(&ptc, ConstArguments[0]);
      assert(Helper != nullptr && Helper->name != nullptr);
      return std::string(Helper->name);
    }

  private:
    PTCInstruction* TheInstruction;

  public:
    const Range<InstructionArgumentsIterator<In, IsCall>> InArguments;
    const Range<InstructionArgumentsIterator<Const, IsCall>> ConstArguments;
    const Range<InstructionArgumentsIterator<Out, IsCall>> OutArguments;

  private:
    unsigned inArgCount() const;
    unsigned constArgCount() const;
    unsigned outArgCount() const;
  };

  using Instruction = InstructionImpl<false>;
  using CallInstruction = InstructionImpl<true>;

  template<>
  inline unsigned CallInstruction::inArgCount() const {
    return ptc_call_instruction_in_arg_count(&ptc, TheInstruction);
  }

  template<>
  inline unsigned Instruction::inArgCount() const {
    return ptc_instruction_in_arg_count(&ptc, TheInstruction);
  }

  template<>
  inline unsigned CallInstruction::constArgCount() const {
    return ptc_call_instruction_const_arg_count(&ptc, TheInstruction);
  }

  template<>
  inline unsigned Instruction::constArgCount() const {
    return ptc_instruction_const_arg_count(&ptc, TheInstruction);
  }

  template<>
  inline unsigned CallInstruction::outArgCount() const {
    return ptc_call_instruction_out_arg_count(&ptc, TheInstruction);
  }

  template<>
  inline unsigned Instruction::outArgCount() const {
    return ptc_instruction_out_arg_count(&ptc, TheInstruction);
  }

}

/// Converts a PTC condition into an LLVM predicate
///
/// \param Condition the input PTC condition.
///
/// \return the corresponding LLVM predicate.
static CmpInst::Predicate conditionToPredicate(PTCCondition Condition) {
  switch (Condition) {
  case PTC_COND_NEVER:
    // TODO: this is probably wrong
    return CmpInst::FCMP_FALSE;
  case PTC_COND_ALWAYS:
    // TODO: this is probably wrong
    return CmpInst::FCMP_TRUE;
  case PTC_COND_EQ:
    return CmpInst::ICMP_EQ;
  case PTC_COND_NE:
    return CmpInst::ICMP_NE;
  case PTC_COND_LT:
    return CmpInst::ICMP_SLT;
  case PTC_COND_GE:
    return CmpInst::ICMP_SGE;
  case PTC_COND_LE:
    return CmpInst::ICMP_SLE;
  case PTC_COND_GT:
    return CmpInst::ICMP_SGT;
  case PTC_COND_LTU:
    return CmpInst::ICMP_ULT;
  case PTC_COND_GEU:
    return CmpInst::ICMP_UGE;
  case PTC_COND_LEU:
    return CmpInst::ICMP_ULE;
  case PTC_COND_GTU:
    return CmpInst::ICMP_UGT;
  default:
    llvm_unreachable("Unknown comparison operator");
  }
}

/// Obtains the LLVM binary operation corresponding to the specified PTC opcode.
///
/// \param Opcode the PTC opcode.
///
/// \return the LLVM binary operation matching opcode.
static Instruction::BinaryOps opcodeToBinaryOp(PTCOpcode Opcode) {
  switch (Opcode) {
  case PTC_INSTRUCTION_op_add_i32:
  case PTC_INSTRUCTION_op_add_i64:
  case PTC_INSTRUCTION_op_add2_i32:
  case PTC_INSTRUCTION_op_add2_i64:
    return Instruction::Add;
  case PTC_INSTRUCTION_op_sub_i32:
  case PTC_INSTRUCTION_op_sub_i64:
  case PTC_INSTRUCTION_op_sub2_i32:
  case PTC_INSTRUCTION_op_sub2_i64:
    return Instruction::Sub;
  case PTC_INSTRUCTION_op_mul_i32:
  case PTC_INSTRUCTION_op_mul_i64:
    return Instruction::Mul;
  case PTC_INSTRUCTION_op_div_i32:
  case PTC_INSTRUCTION_op_div_i64:
    return Instruction::SDiv;
  case PTC_INSTRUCTION_op_divu_i32:
  case PTC_INSTRUCTION_op_divu_i64:
    return Instruction::UDiv;
  case PTC_INSTRUCTION_op_rem_i32:
  case PTC_INSTRUCTION_op_rem_i64:
    return Instruction::SRem;
  case PTC_INSTRUCTION_op_remu_i32:
  case PTC_INSTRUCTION_op_remu_i64:
    return Instruction::URem;
  case PTC_INSTRUCTION_op_and_i32:
  case PTC_INSTRUCTION_op_and_i64:
    return Instruction::And;
  case PTC_INSTRUCTION_op_or_i32:
  case PTC_INSTRUCTION_op_or_i64:
    return Instruction::Or;
  case PTC_INSTRUCTION_op_xor_i32:
  case PTC_INSTRUCTION_op_xor_i64:
    return Instruction::Xor;
  case PTC_INSTRUCTION_op_shl_i32:
  case PTC_INSTRUCTION_op_shl_i64:
    return Instruction::Shl;
  case PTC_INSTRUCTION_op_shr_i32:
  case PTC_INSTRUCTION_op_shr_i64:
    return Instruction::LShr;
  case PTC_INSTRUCTION_op_sar_i32:
  case PTC_INSTRUCTION_op_sar_i64:
    return Instruction::AShr;
  default:
    llvm_unreachable("PTC opcode is not a binary operator");
  }
}

/// Returns the maximum value which can be represented with the specified number
/// of bits.
static uint64_t getMaxValue(unsigned Bits) {
  if (Bits == 32)
    return 0xffffffff;
  else if (Bits == 64)
    return 0xffffffffffffffff;
  else
    llvm_unreachable("Not the number of bits in a integer type");
}

/// Maps an opcode the corresponding input and output register size.
///
/// \return the size, in bits, of the registers used by the opcode.
static unsigned getRegisterSize(unsigned Opcode) {
  switch (Opcode) {
  case PTC_INSTRUCTION_op_add2_i32:
  case PTC_INSTRUCTION_op_add_i32:
  case PTC_INSTRUCTION_op_andc_i32:
  case PTC_INSTRUCTION_op_and_i32:
  case PTC_INSTRUCTION_op_brcond2_i32:
  case PTC_INSTRUCTION_op_brcond_i32:
  case PTC_INSTRUCTION_op_bswap16_i32:
  case PTC_INSTRUCTION_op_bswap32_i32:
  case PTC_INSTRUCTION_op_deposit_i32:
  case PTC_INSTRUCTION_op_div2_i32:
  case PTC_INSTRUCTION_op_div_i32:
  case PTC_INSTRUCTION_op_divu2_i32:
  case PTC_INSTRUCTION_op_divu_i32:
  case PTC_INSTRUCTION_op_eqv_i32:
  case PTC_INSTRUCTION_op_ext16s_i32:
  case PTC_INSTRUCTION_op_ext16u_i32:
  case PTC_INSTRUCTION_op_ext8s_i32:
  case PTC_INSTRUCTION_op_ext8u_i32:
  case PTC_INSTRUCTION_op_ld16s_i32:
  case PTC_INSTRUCTION_op_ld16u_i32:
  case PTC_INSTRUCTION_op_ld8s_i32:
  case PTC_INSTRUCTION_op_ld8u_i32:
  case PTC_INSTRUCTION_op_ld_i32:
  case PTC_INSTRUCTION_op_movcond_i32:
  case PTC_INSTRUCTION_op_mov_i32:
  case PTC_INSTRUCTION_op_movi_i32:
  case PTC_INSTRUCTION_op_mul_i32:
  case PTC_INSTRUCTION_op_muls2_i32:
  case PTC_INSTRUCTION_op_mulsh_i32:
  case PTC_INSTRUCTION_op_mulu2_i32:
  case PTC_INSTRUCTION_op_muluh_i32:
  case PTC_INSTRUCTION_op_nand_i32:
  case PTC_INSTRUCTION_op_neg_i32:
  case PTC_INSTRUCTION_op_nor_i32:
  case PTC_INSTRUCTION_op_not_i32:
  case PTC_INSTRUCTION_op_orc_i32:
  case PTC_INSTRUCTION_op_or_i32:
  case PTC_INSTRUCTION_op_qemu_ld_i32:
  case PTC_INSTRUCTION_op_qemu_st_i32:
  case PTC_INSTRUCTION_op_rem_i32:
  case PTC_INSTRUCTION_op_remu_i32:
  case PTC_INSTRUCTION_op_rotl_i32:
  case PTC_INSTRUCTION_op_rotr_i32:
  case PTC_INSTRUCTION_op_sar_i32:
  case PTC_INSTRUCTION_op_setcond2_i32:
  case PTC_INSTRUCTION_op_setcond_i32:
  case PTC_INSTRUCTION_op_shl_i32:
  case PTC_INSTRUCTION_op_shr_i32:
  case PTC_INSTRUCTION_op_st16_i32:
  case PTC_INSTRUCTION_op_st8_i32:
  case PTC_INSTRUCTION_op_st_i32:
  case PTC_INSTRUCTION_op_sub2_i32:
  case PTC_INSTRUCTION_op_sub_i32:
  case PTC_INSTRUCTION_op_trunc_shr_i32:
  case PTC_INSTRUCTION_op_xor_i32:
    return 32;
  case PTC_INSTRUCTION_op_add2_i64:
  case PTC_INSTRUCTION_op_add_i64:
  case PTC_INSTRUCTION_op_andc_i64:
  case PTC_INSTRUCTION_op_and_i64:
  case PTC_INSTRUCTION_op_brcond_i64:
  case PTC_INSTRUCTION_op_bswap16_i64:
  case PTC_INSTRUCTION_op_bswap32_i64:
  case PTC_INSTRUCTION_op_bswap64_i64:
  case PTC_INSTRUCTION_op_deposit_i64:
  case PTC_INSTRUCTION_op_div2_i64:
  case PTC_INSTRUCTION_op_div_i64:
  case PTC_INSTRUCTION_op_divu2_i64:
  case PTC_INSTRUCTION_op_divu_i64:
  case PTC_INSTRUCTION_op_eqv_i64:
  case PTC_INSTRUCTION_op_ext16s_i64:
  case PTC_INSTRUCTION_op_ext16u_i64:
  case PTC_INSTRUCTION_op_ext32s_i64:
  case PTC_INSTRUCTION_op_ext32u_i64:
  case PTC_INSTRUCTION_op_ext8s_i64:
  case PTC_INSTRUCTION_op_ext8u_i64:
  case PTC_INSTRUCTION_op_ld16s_i64:
  case PTC_INSTRUCTION_op_ld16u_i64:
  case PTC_INSTRUCTION_op_ld32s_i64:
  case PTC_INSTRUCTION_op_ld32u_i64:
  case PTC_INSTRUCTION_op_ld8s_i64:
  case PTC_INSTRUCTION_op_ld8u_i64:
  case PTC_INSTRUCTION_op_ld_i64:
  case PTC_INSTRUCTION_op_movcond_i64:
  case PTC_INSTRUCTION_op_mov_i64:
  case PTC_INSTRUCTION_op_movi_i64:
  case PTC_INSTRUCTION_op_mul_i64:
  case PTC_INSTRUCTION_op_muls2_i64:
  case PTC_INSTRUCTION_op_mulsh_i64:
  case PTC_INSTRUCTION_op_mulu2_i64:
  case PTC_INSTRUCTION_op_muluh_i64:
  case PTC_INSTRUCTION_op_nand_i64:
  case PTC_INSTRUCTION_op_neg_i64:
  case PTC_INSTRUCTION_op_nor_i64:
  case PTC_INSTRUCTION_op_not_i64:
  case PTC_INSTRUCTION_op_orc_i64:
  case PTC_INSTRUCTION_op_or_i64:
  case PTC_INSTRUCTION_op_qemu_ld_i64:
  case PTC_INSTRUCTION_op_qemu_st_i64:
  case PTC_INSTRUCTION_op_rem_i64:
  case PTC_INSTRUCTION_op_remu_i64:
  case PTC_INSTRUCTION_op_rotl_i64:
  case PTC_INSTRUCTION_op_rotr_i64:
  case PTC_INSTRUCTION_op_sar_i64:
  case PTC_INSTRUCTION_op_setcond_i64:
  case PTC_INSTRUCTION_op_shl_i64:
  case PTC_INSTRUCTION_op_shr_i64:
  case PTC_INSTRUCTION_op_st16_i64:
  case PTC_INSTRUCTION_op_st32_i64:
  case PTC_INSTRUCTION_op_st8_i64:
  case PTC_INSTRUCTION_op_st_i64:
  case PTC_INSTRUCTION_op_sub2_i64:
  case PTC_INSTRUCTION_op_sub_i64:
  case PTC_INSTRUCTION_op_xor_i64:
    return 64;
  case PTC_INSTRUCTION_op_br:
  case PTC_INSTRUCTION_op_call:
  case PTC_INSTRUCTION_op_debug_insn_start:
  case PTC_INSTRUCTION_op_discard:
  case PTC_INSTRUCTION_op_exit_tb:
  case PTC_INSTRUCTION_op_goto_tb:
  case PTC_INSTRUCTION_op_set_label:
    return 0;
  default:
    llvm_unreachable("Unexpected opcode");
    break;
  }
}

/// Create a compare instruction given a comparison operator and the operands
///
/// \param Builder the builder to use to create the instruction.
/// \param RawCondition the PTC condition.
/// \param FirstOperand the first operand of the comparison.
/// \param SecondOperand the second operand of the comparison.
///
/// \return a compare instruction.
template<typename T>
static Value *CreateICmp(T& Builder,
                         uint64_t RawCondition,
                         Value *FirstOperand,
                         Value *SecondOperand) {
  PTCCondition Condition = static_cast<PTCCondition>(RawCondition);
  return Builder.CreateICmp(conditionToPredicate(Condition),
                            FirstOperand,
                            SecondOperand);
}

using LBM = InstructionTranslator::LabeledBlocksMap;
InstructionTranslator::InstructionTranslator(IRBuilder<>& Builder,
                                             VariableManager& Variables,
                                             JumpTargetManager& JumpTargets,
                                             LBM& LabeledBasicBlocks,
                                             std::vector<BasicBlock *> Blocks,
                                             Module& TheModule,
                                             Function *TheFunction,
                                             Architecture& SourceArchitecture,
                                             Architecture& TargetArchitecture) :
  Builder(Builder),
  Variables(Variables),
  JumpTargets(JumpTargets),
  LabeledBasicBlocks(LabeledBasicBlocks),
  Blocks(Blocks),
  TheModule(TheModule),
  TheFunction(TheFunction),
  SourceArchitecture(SourceArchitecture),
  TargetArchitecture(TargetArchitecture),
  NewPCMarker(nullptr),
  LastMarker(nullptr) {

  auto &Context = TheModule.getContext();
  NewPCMarker = Function::Create(FunctionType::get(Type::getVoidTy(Context),
                                                   {
                                                     Type::getInt64Ty(Context),
                                                     Type::getInt64Ty(Context)
                                                   },
                                                   false),
                                 GlobalValue::ExternalLinkage,
                                 "newpc",
                                 &TheModule);
  }

void InstructionTranslator::removeNewPCMarkers() {

  std::vector<Instruction *> ToDelete;

  for (User *Call : NewPCMarker->users())
    if (cast<Instruction>(Call)->getParent() != nullptr)
      ToDelete.push_back(cast<Instruction>(Call));

  for (Instruction *TheInstruction : ToDelete)
    TheInstruction->eraseFromParent();

  NewPCMarker->eraseFromParent();
}

void InstructionTranslator::closeLastInstruction(uint64_t PC) {
  assert(LastMarker != nullptr);

  auto *Operand = cast<ConstantInt>(LastMarker->getArgOperand(0));
  uint64_t StartPC = Operand->getLimitedValue();

  assert(PC > StartPC);
  LastMarker->setArgOperand(1, Builder.getInt64(PC - StartPC));

  LastMarker = nullptr;
}

std::tuple<bool, MDNode *, uint64_t>
InstructionTranslator::newInstruction(PTCInstruction *Instr,
                                      bool IsFirst) {
  using r = std::tuple<bool, MDNode *, uint64_t>;

  const PTC::Instruction TheInstruction(Instr);
  // A new original instruction, let's create a new metadata node
  // referencing it for all the next instructions to come
  uint64_t PC = TheInstruction.ConstArguments[0];

  // TODO: replace using a field in Architecture
  if (TheInstruction.ConstArguments.size() > 1)
    PC |= TheInstruction.ConstArguments[1] << 32;

  std::stringstream OriginalStringStream;
  disassembleOriginal(OriginalStringStream, PC);
  std::string OriginalString = OriginalStringStream.str();
  LLVMContext& Context = TheModule.getContext();
  MDString *MDOriginalString = MDString::get(Context, OriginalString);
  MDNode *MDOriginalInstr = MDNode::getDistinct(Context, MDOriginalString);

  if (!IsFirst) {
    // Check if this PC already has a block and use it
    bool ShouldContinue;
    BasicBlock *DivergeTo = JumpTargets.newPC(PC, ShouldContinue);
    if (DivergeTo != nullptr) {
      Builder.CreateBr(DivergeTo);

      if (ShouldContinue) {
        // The block is empty, let's fill it
        Blocks.push_back(DivergeTo);
        Builder.SetInsertPoint(DivergeTo);
        Variables.newBasicBlock();
      } else {
        // The block contains already translated code, early exit
        return r { true, MDOriginalInstr, PC };
      }
    }
  }

  if (LastMarker != nullptr)
    closeLastInstruction(PC);
  LastMarker = Builder.CreateCall(NewPCMarker,
                                  { Builder.getInt64(PC), Builder.getInt64(0) });

  if (!IsFirst) {
    // Inform the JumpTargetManager about the new PC we met
    BasicBlock::iterator CurrentIt = Builder.GetInsertPoint();
    if (CurrentIt == Builder.GetInsertBlock()->begin())
      JumpTargets.registerBlock(PC, Builder.GetInsertBlock());
    else
      JumpTargets.registerInstruction(PC, LastMarker);
  }

  return r { false, MDOriginalInstr, PC };
}

void InstructionTranslator::translateCall(PTCInstruction *Instr) {
  const PTC::CallInstruction TheCall(Instr);

  auto LoadArgs = [this] (uint64_t TemporaryId) -> Value * {
    auto *Load = Builder.CreateLoad(Variables.getOrCreate(TemporaryId));
    Variables.setAliasScope(Load);
    return Load;
  };

  auto GetValueType = [] (Value *Argument) { return Argument->getType(); };

  std::vector<Value *> InArgs = (TheCall.InArguments | LoadArgs).toVector();
  std::vector<Type *> InArgsType = (InArgs | GetValueType).toVector();

  // TODO: handle multiple return arguments
  assert(TheCall.OutArguments.size() <= 1);

  Value *ResultDestination = nullptr;
  Type *ResultType = nullptr;

  if (TheCall.OutArguments.size() != 0) {
    ResultDestination = Variables.getOrCreate(TheCall.OutArguments[0]);
    ResultType = ResultDestination->getType()->getPointerElementType();
  } else {
    ResultType = Builder.getVoidTy();
  }

  auto *CalleeType = FunctionType::get(ResultType,
                                       ArrayRef<Type *>(InArgsType),
                                       false);

  std::string HelperName = "helper_" + TheCall.helperName();
  Constant *FunctionDeclaration = TheModule.getOrInsertFunction(HelperName,
                                                                CalleeType);
  Value *Result = Builder.CreateCall(FunctionDeclaration, InArgs);

  if (TheCall.OutArguments.size() != 0) {
    auto *Store = Builder.CreateStore(Result, ResultDestination);
    Variables.setAliasScope(Store);
  }
}

bool InstructionTranslator::translate(PTCInstruction *Instr, uint64_t PC) {
  const PTC::Instruction TheInstruction(Instr);

  auto LoadArgs = [this] (uint64_t TemporaryId) -> Value * {
    auto *Load = Builder.CreateLoad(Variables.getOrCreate(TemporaryId));
    Variables.setAliasScope(Load);
    return Load;
  };

  auto ConstArgs = TheInstruction.ConstArguments;
  auto InArgs = TheInstruction.InArguments | LoadArgs;

  auto Result = translateOpcode(TheInstruction.opcode(),
                                ConstArgs.toVector(),
                                InArgs.toVector());

  // Check if there was an error while translating the instruction
  if (!Result) {
    Builder.CreateCall(TheModule.getFunction("abort"));
    Builder.CreateUnreachable();
    return true;
  }

  assert(Result->size() == (size_t) TheInstruction.OutArguments.size());
  // TODO: use ZipIterator here
  for (unsigned I = 0; I < Result->size(); I++) {
    auto *Destination = Variables.getOrCreate(TheInstruction.OutArguments[I]);
    auto *Value = Result.get()[I];
    auto *Store = Builder.CreateStore(Value, Destination);
    Variables.setAliasScope(Store);

    // If we're writing the PC with an immediate, register it for exploration
    // immediately
    if (JumpTargets.isPCReg(Destination)) {
      auto *Constant = dyn_cast<ConstantInt>(Value);
      if (Constant != nullptr) {
        uint64_t Address = Constant->getLimitedValue();
        if (PC != Address)
          JumpTargets.getBlockAt(Address);
      }
    }
  }

  return false;
}

ErrorOr<std::vector<Value *>>
InstructionTranslator::translateOpcode(PTCOpcode Opcode,
                                       std::vector<uint64_t> ConstArguments,
                                       std::vector<Value *> InArguments) {
  LLVMContext& Context = TheModule.getContext();
  unsigned RegisterSize = getRegisterSize(Opcode);
  Type *RegisterType = nullptr;
  if (RegisterSize == 32)
    RegisterType = Builder.getInt32Ty();
  else if (RegisterSize == 64)
    RegisterType = Builder.getInt64Ty();
  else if (RegisterSize != 0)
    llvm_unreachable("Unexpected register size");

  using v = std::vector<Value *>;
  switch (Opcode) {
  case PTC_INSTRUCTION_op_movi_i32:
  case PTC_INSTRUCTION_op_movi_i64:
    return v { ConstantInt::get(RegisterType, ConstArguments[0]) };
  case PTC_INSTRUCTION_op_discard:
    // Let's overwrite the discarded temporary with a 0
    return v { ConstantInt::get(RegisterType, 0) };
  case PTC_INSTRUCTION_op_mov_i32:
  case PTC_INSTRUCTION_op_mov_i64:
    return v { Builder.CreateTrunc(InArguments[0], RegisterType) };
  case PTC_INSTRUCTION_op_setcond_i32:
  case PTC_INSTRUCTION_op_setcond_i64:
    {
      Value *Compare = CreateICmp(Builder,
                                  ConstArguments[0],
                                  InArguments[0],
                                  InArguments[1]);
      // TODO: convert single-bit registers to i1
      return v { Builder.CreateZExt(Compare, RegisterType) };
    }
  case PTC_INSTRUCTION_op_movcond_i32: // Resist the fallthrough temptation
  case PTC_INSTRUCTION_op_movcond_i64:
    {
      Value *Compare = CreateICmp(Builder,
                                  ConstArguments[0],
                                  InArguments[0],
                                  InArguments[1]);
      Value *Select = Builder.CreateSelect(Compare,
                                           InArguments[2],
                                           InArguments[3]);
      return v { Select };
    }
  case PTC_INSTRUCTION_op_qemu_ld_i32:
  case PTC_INSTRUCTION_op_qemu_ld_i64:
  case PTC_INSTRUCTION_op_qemu_st_i32:
  case PTC_INSTRUCTION_op_qemu_st_i64:
    {
      PTCLoadStoreArg MemoryAccess;
      MemoryAccess = ptc.parse_load_store_arg(ConstArguments[0]);

      // What are we supposed to do in this case?
      assert(MemoryAccess.access_type != PTC_MEMORY_ACCESS_UNKNOWN);

      unsigned Alignment = 0;
      if (MemoryAccess.access_type == PTC_MEMORY_ACCESS_UNALIGNED)
        Alignment = 1;
      else
        Alignment = SourceArchitecture.defaultAlignment();

      // Load size
      IntegerType *MemoryType = nullptr;
      switch (ptc_get_memory_access_size(MemoryAccess.type)) {
      case PTC_MO_8:
        MemoryType = Builder.getInt8Ty();
        break;
      case PTC_MO_16:
        MemoryType = Builder.getInt16Ty();
        break;
      case PTC_MO_32:
        MemoryType = Builder.getInt32Ty();
        break;
      case PTC_MO_64:
        MemoryType = Builder.getInt64Ty();
        break;
      default:
        llvm_unreachable("Unexpected load size");
      }

      // If necessary, handle endianess mismatch
      // TODO: it might be a bit overkill, but it be nice to make this function
      //       template-parametric w.r.t. endianess mismatch
      Function *BSwapFunction = nullptr;
      if (MemoryType != Builder.getInt8Ty() &&
          SourceArchitecture.endianess() != TargetArchitecture.endianess())
        BSwapFunction = Intrinsic::getDeclaration(&TheModule,
                                                  Intrinsic::bswap,
                                                  { MemoryType });

      bool SignExtend = ptc_is_sign_extended_load(MemoryAccess.type);

      Value *Pointer = nullptr;
      if (Opcode == PTC_INSTRUCTION_op_qemu_ld_i32 ||
          Opcode == PTC_INSTRUCTION_op_qemu_ld_i64) {

        Pointer = Builder.CreateIntToPtr(InArguments[0],
                                         MemoryType->getPointerTo());
        auto *Load = Builder.CreateAlignedLoad(Pointer, Alignment);
        Variables.setNoAlias(Load);
        Value *Loaded = Load;

        if (BSwapFunction != nullptr)
          Loaded = Builder.CreateCall(BSwapFunction, Load);

        if (SignExtend)
          return v { Builder.CreateSExt(Loaded, RegisterType) };
        else
          return v { Builder.CreateZExt(Loaded, RegisterType) };

      } else if (Opcode == PTC_INSTRUCTION_op_qemu_st_i32 ||
                 Opcode == PTC_INSTRUCTION_op_qemu_st_i64) {

        Pointer = Builder.CreateIntToPtr(InArguments[1],
                                         MemoryType->getPointerTo());
        Value *Value = Builder.CreateTrunc(InArguments[0], MemoryType);

        if (BSwapFunction != nullptr)
          Value = Builder.CreateCall(BSwapFunction, Value);

        auto *Store = Builder.CreateAlignedStore(Value, Pointer, Alignment);
        Variables.setNoAlias(Store);

        return v { };
      } else
        llvm_unreachable("Unknown load type");
    }
  case PTC_INSTRUCTION_op_ld8u_i32:
  case PTC_INSTRUCTION_op_ld8s_i32:
  case PTC_INSTRUCTION_op_ld16u_i32:
  case PTC_INSTRUCTION_op_ld16s_i32:
  case PTC_INSTRUCTION_op_ld_i32:
  case PTC_INSTRUCTION_op_ld8u_i64:
  case PTC_INSTRUCTION_op_ld8s_i64:
  case PTC_INSTRUCTION_op_ld16u_i64:
  case PTC_INSTRUCTION_op_ld16s_i64:
  case PTC_INSTRUCTION_op_ld32u_i64:
  case PTC_INSTRUCTION_op_ld32s_i64:
  case PTC_INSTRUCTION_op_ld_i64:
    {
      Value *Base = dyn_cast<LoadInst>(InArguments[0])->getPointerOperand();
      if (Base == nullptr || !Variables.isEnv(Base)) {
        // TODO: emit warning
        return std::errc::invalid_argument;
      }

      Value *Target = Variables.getByEnvOffset(ConstArguments[0]);

      auto *LoadEnvField = Builder.CreateLoad(Target);
      Variables.setAliasScope(LoadEnvField);
      Value *Fitted = Builder.CreateZExtOrTrunc(LoadEnvField, RegisterType);

      return v { Fitted };
    }
  case PTC_INSTRUCTION_op_st8_i32:
  case PTC_INSTRUCTION_op_st16_i32:
  case PTC_INSTRUCTION_op_st_i32:
  case PTC_INSTRUCTION_op_st8_i64:
  case PTC_INSTRUCTION_op_st16_i64:
  case PTC_INSTRUCTION_op_st32_i64:
  case PTC_INSTRUCTION_op_st_i64:
    {
      Value *Base = dyn_cast<LoadInst>(InArguments[1])->getPointerOperand();
      if (Base == nullptr || !Variables.isEnv(Base)) {
        // TODO: emit warning
        return std::errc::invalid_argument;
      }

      Value *Target = Variables.getByEnvOffset(ConstArguments[0]);
      Type *TargetPointer = Target->getType()->getPointerElementType();
      Value *ToStore = Builder.CreateZExt(InArguments[0], TargetPointer);
      auto *Store = Builder.CreateStore(ToStore, Target);
      Variables.setAliasScope(Store);
      return v { };
    }
  case PTC_INSTRUCTION_op_add_i32:
  case PTC_INSTRUCTION_op_sub_i32:
  case PTC_INSTRUCTION_op_mul_i32:
  case PTC_INSTRUCTION_op_div_i32:
  case PTC_INSTRUCTION_op_divu_i32:
  case PTC_INSTRUCTION_op_rem_i32:
  case PTC_INSTRUCTION_op_remu_i32:
  case PTC_INSTRUCTION_op_and_i32:
  case PTC_INSTRUCTION_op_or_i32:
  case PTC_INSTRUCTION_op_xor_i32:
  case PTC_INSTRUCTION_op_shl_i32:
  case PTC_INSTRUCTION_op_shr_i32:
  case PTC_INSTRUCTION_op_sar_i32:
  case PTC_INSTRUCTION_op_add_i64:
  case PTC_INSTRUCTION_op_sub_i64:
  case PTC_INSTRUCTION_op_mul_i64:
  case PTC_INSTRUCTION_op_div_i64:
  case PTC_INSTRUCTION_op_divu_i64:
  case PTC_INSTRUCTION_op_rem_i64:
  case PTC_INSTRUCTION_op_remu_i64:
  case PTC_INSTRUCTION_op_and_i64:
  case PTC_INSTRUCTION_op_or_i64:
  case PTC_INSTRUCTION_op_xor_i64:
  case PTC_INSTRUCTION_op_shl_i64:
  case PTC_INSTRUCTION_op_shr_i64:
  case PTC_INSTRUCTION_op_sar_i64:
    {
      // TODO: assert on sizes?
      Instruction::BinaryOps BinaryOp = opcodeToBinaryOp(Opcode);
      Value *Operation = Builder.CreateBinOp(BinaryOp,
                                             InArguments[0],
                                             InArguments[1]);
      return v { Operation };
    }
  case PTC_INSTRUCTION_op_div2_i32:
  case PTC_INSTRUCTION_op_divu2_i32:
  case PTC_INSTRUCTION_op_div2_i64:
  case PTC_INSTRUCTION_op_divu2_i64:
    {
      Instruction::BinaryOps DivisionOp, RemainderOp;

      if (Opcode == PTC_INSTRUCTION_op_div2_i32 ||
          Opcode == PTC_INSTRUCTION_op_div2_i64) {
        DivisionOp = Instruction::SDiv;
        RemainderOp = Instruction::SRem;
      } else if (Opcode == PTC_INSTRUCTION_op_divu2_i32 ||
                 Opcode == PTC_INSTRUCTION_op_divu2_i64) {
        DivisionOp = Instruction::UDiv;
        RemainderOp = Instruction::URem;
      } else
        llvm_unreachable("Unknown operation type");

      // TODO: we're ignoring InArguments[1], which is the MSB
      // TODO: assert on sizes?
      Value *Division = Builder.CreateBinOp(DivisionOp,
                                            InArguments[0],
                                            InArguments[2]);
      Value *Remainder = Builder.CreateBinOp(RemainderOp,
                                             InArguments[0],
                                             InArguments[2]);
      return v { Division, Remainder };
    }
  case PTC_INSTRUCTION_op_rotr_i32:
  case PTC_INSTRUCTION_op_rotr_i64:
  case PTC_INSTRUCTION_op_rotl_i32:
  case PTC_INSTRUCTION_op_rotl_i64:
    {
      Value *Bits = ConstantInt::get(RegisterType, RegisterSize);

      Instruction::BinaryOps FirstShiftOp, SecondShiftOp;
      if (Opcode == PTC_INSTRUCTION_op_rotl_i32 ||
          Opcode == PTC_INSTRUCTION_op_rotl_i64) {
        FirstShiftOp = Instruction::LShr;
        SecondShiftOp = Instruction::Shl;
      } else if (Opcode == PTC_INSTRUCTION_op_rotr_i32 ||
                 Opcode == PTC_INSTRUCTION_op_rotr_i64) {
        FirstShiftOp = Instruction::Shl;
        SecondShiftOp = Instruction::LShr;
      } else
        llvm_unreachable("Unexpected opcode");

      Value *FirstShift = Builder.CreateBinOp(FirstShiftOp,
                                              InArguments[0],
                                              InArguments[1]);
      Value *SecondShiftAmount = Builder.CreateSub(Bits,
                                                   InArguments[1]);
      Value *SecondShift = Builder.CreateBinOp(SecondShiftOp,
                                               InArguments[0],
                                               SecondShiftAmount);

      return v { Builder.CreateOr(FirstShift, SecondShift) };
    }
  case PTC_INSTRUCTION_op_deposit_i32:
  case PTC_INSTRUCTION_op_deposit_i64:
    {
      unsigned Position = ConstArguments[0];
      if (Position == RegisterSize)
        return v { InArguments[0] };

      unsigned Length = ConstArguments[1];
      uint64_t Bits = 0;

      // Thou shall not << 32
      if (Length == RegisterSize)
        Bits = getMaxValue(RegisterSize);
      else
        Bits = (1 << Length) - 1;

      // result = (t1 & ~(bits << position)) | ((t2 & bits) << position)
      uint64_t BaseMask = ~(Bits << Position);
      Value *MaskedBase = Builder.CreateAnd(InArguments[0], BaseMask);
      Value *Deposit = Builder.CreateAnd(InArguments[1], Bits);
      Value *ShiftedDeposit = Builder.CreateShl(Deposit, Position);
      Value *Result = Builder.CreateOr(MaskedBase, ShiftedDeposit);

      return v { Result };
    }
  case PTC_INSTRUCTION_op_ext8s_i32:
  case PTC_INSTRUCTION_op_ext16s_i32:
  case PTC_INSTRUCTION_op_ext8u_i32:
  case PTC_INSTRUCTION_op_ext16u_i32:
  case PTC_INSTRUCTION_op_ext8s_i64:
  case PTC_INSTRUCTION_op_ext16s_i64:
  case PTC_INSTRUCTION_op_ext32s_i64:
  case PTC_INSTRUCTION_op_ext8u_i64:
  case PTC_INSTRUCTION_op_ext16u_i64:
  case PTC_INSTRUCTION_op_ext32u_i64:
    {
      Type *SourceType = nullptr;
      switch (Opcode) {
      case PTC_INSTRUCTION_op_ext8s_i32:
      case PTC_INSTRUCTION_op_ext8u_i32:
      case PTC_INSTRUCTION_op_ext8s_i64:
      case PTC_INSTRUCTION_op_ext8u_i64:
        SourceType = Builder.getInt8Ty();
        break;
      case PTC_INSTRUCTION_op_ext16s_i32:
      case PTC_INSTRUCTION_op_ext16u_i32:
      case PTC_INSTRUCTION_op_ext16s_i64:
      case PTC_INSTRUCTION_op_ext16u_i64:
        SourceType = Builder.getInt16Ty();
        break;
      case PTC_INSTRUCTION_op_ext32s_i64:
      case PTC_INSTRUCTION_op_ext32u_i64:
        SourceType = Builder.getInt32Ty();
        break;
      default:
        llvm_unreachable("Unexpected opcode");
      }

      Value *Truncated = Builder.CreateTrunc(InArguments[0], SourceType);

      switch (Opcode) {
      case PTC_INSTRUCTION_op_ext8s_i32:
      case PTC_INSTRUCTION_op_ext8s_i64:
      case PTC_INSTRUCTION_op_ext16s_i32:
      case PTC_INSTRUCTION_op_ext16s_i64:
      case PTC_INSTRUCTION_op_ext32s_i64:
        return v { Builder.CreateSExt(Truncated, RegisterType) };
      case PTC_INSTRUCTION_op_ext8u_i32:
      case PTC_INSTRUCTION_op_ext8u_i64:
      case PTC_INSTRUCTION_op_ext16u_i32:
      case PTC_INSTRUCTION_op_ext16u_i64:
      case PTC_INSTRUCTION_op_ext32u_i64:
        return v { Builder.CreateZExt(Truncated, RegisterType) };
      default:
        llvm_unreachable("Unexpected opcode");
      }
    }
  case PTC_INSTRUCTION_op_not_i32:
  case PTC_INSTRUCTION_op_not_i64:
    return v { Builder.CreateXor(InArguments[0], getMaxValue(RegisterSize)) };
  case PTC_INSTRUCTION_op_neg_i32:
  case PTC_INSTRUCTION_op_neg_i64:
    {
      auto *InitialValue = ConstantInt::get(RegisterType, 0);
      return v { Builder.CreateSub(InitialValue, InArguments[0]) };
    }
  case PTC_INSTRUCTION_op_andc_i32:
  case PTC_INSTRUCTION_op_andc_i64:
  case PTC_INSTRUCTION_op_orc_i32:
  case PTC_INSTRUCTION_op_orc_i64:
  case PTC_INSTRUCTION_op_eqv_i32:
  case PTC_INSTRUCTION_op_eqv_i64:
    {
      Instruction::BinaryOps ExternalOp;
      switch (Opcode) {
      case PTC_INSTRUCTION_op_andc_i32:
      case PTC_INSTRUCTION_op_andc_i64:
        ExternalOp = Instruction::And;
        break;
      case PTC_INSTRUCTION_op_orc_i32:
      case PTC_INSTRUCTION_op_orc_i64:
        ExternalOp = Instruction::Or;
        break;
      case PTC_INSTRUCTION_op_eqv_i32:
      case PTC_INSTRUCTION_op_eqv_i64:
        ExternalOp = Instruction::Xor;
        break;
      default:
        llvm_unreachable("Unexpected opcode");
      }

      Value *Negate = Builder.CreateXor(InArguments[1],
                                        getMaxValue(RegisterSize));
      Value *Result = Builder.CreateBinOp(ExternalOp, InArguments[0], Negate);
      return v { Result };
    }
  case PTC_INSTRUCTION_op_nand_i32:
  case PTC_INSTRUCTION_op_nand_i64:
    {
      Value *AndValue = Builder.CreateAnd(InArguments[0], InArguments[1]);
      Value *Result = Builder.CreateXor(AndValue, getMaxValue(RegisterSize));
      return v { Result };
    }
  case PTC_INSTRUCTION_op_nor_i32:
  case PTC_INSTRUCTION_op_nor_i64:
    {
      Value *OrValue = Builder.CreateOr(InArguments[0], InArguments[1]);
      Value *Result = Builder.CreateXor(OrValue, getMaxValue(RegisterSize));
      return v { Result };
    }
  case PTC_INSTRUCTION_op_bswap16_i32:
  case PTC_INSTRUCTION_op_bswap32_i32:
  case PTC_INSTRUCTION_op_bswap16_i64:
  case PTC_INSTRUCTION_op_bswap32_i64:
  case PTC_INSTRUCTION_op_bswap64_i64:
    {
      Type *SwapType = nullptr;
      switch (Opcode) {
      case PTC_INSTRUCTION_op_bswap16_i32:
      case PTC_INSTRUCTION_op_bswap16_i64:
        SwapType = Builder.getInt16Ty();
      case PTC_INSTRUCTION_op_bswap32_i32:
      case PTC_INSTRUCTION_op_bswap32_i64:
        SwapType = Builder.getInt32Ty();
      case PTC_INSTRUCTION_op_bswap64_i64:
        SwapType = Builder.getInt64Ty();
      default:
        llvm_unreachable("Unexpected opcode");
      }

      Value *Truncated = Builder.CreateTrunc(InArguments[0], SwapType);

      Function *BSwapFunction = Intrinsic::getDeclaration(&TheModule,
                                                          Intrinsic::bswap,
                                                          { RegisterType });
      Value *Swapped = Builder.CreateCall(BSwapFunction, Truncated);

      return v { Builder.CreateZExt(Swapped, RegisterType) };
    }
  case PTC_INSTRUCTION_op_set_label:
    {
      unsigned LabelId = ptc.get_arg_label_id(ConstArguments[0]);
      std::string Label = "L" + std::to_string(LabelId);

      BasicBlock *Fallthrough = nullptr;
      auto ExistingBasicBlock = LabeledBasicBlocks.find(Label);

      if (ExistingBasicBlock == LabeledBasicBlocks.end()) {
        Fallthrough = BasicBlock::Create(Context, Label, TheFunction);
        LabeledBasicBlocks[Label] = Fallthrough;
      } else {
        // A basic block with that label already exist
        Fallthrough = LabeledBasicBlocks[Label];

        // Ensure it's empty
        assert(Fallthrough->begin() == Fallthrough->end());

        // Move it to the bottom
        Fallthrough->removeFromParent();
        TheFunction->getBasicBlockList().push_back(Fallthrough);
      }

      Builder.CreateBr(Fallthrough);

      Blocks.push_back(Fallthrough);
      Builder.SetInsertPoint(Fallthrough);
      Variables.newBasicBlock();

      return v { };
    }
  case PTC_INSTRUCTION_op_br:
  case PTC_INSTRUCTION_op_brcond_i32:
  case PTC_INSTRUCTION_op_brcond2_i32:
  case PTC_INSTRUCTION_op_brcond_i64:
    {
      // We take the last constant arguments, which is the LabelId both in
      // conditional and unconditional jumps
      unsigned LabelId = ptc.get_arg_label_id(ConstArguments.back());
      std::string Label = "L" + std::to_string(LabelId);

      BasicBlock *Fallthrough = BasicBlock::Create(Context, "", TheFunction);

      // Look for a matching label
      BasicBlock *Target = nullptr;
      auto ExistingBasicBlock = LabeledBasicBlocks.find(Label);

      // No matching label, create a temporary block
      if (ExistingBasicBlock == LabeledBasicBlocks.end()) {
        Target = BasicBlock::Create(Context, Label, TheFunction);
        LabeledBasicBlocks[Label] = Target;
      } else
        Target = LabeledBasicBlocks[Label];

      if (Opcode == PTC_INSTRUCTION_op_br) {
        // Unconditional jump
        Builder.CreateBr(Target);
      } else if (Opcode == PTC_INSTRUCTION_op_brcond_i32
                 || Opcode == PTC_INSTRUCTION_op_brcond_i64) {
        // Conditional jump
        Value *Compare = CreateICmp(Builder,
                                    ConstArguments[0],
                                    InArguments[0],
                                    InArguments[1]);
        Builder.CreateCondBr(Compare, Target, Fallthrough);
      } else
        llvm_unreachable("Unhandled opcode");

      Blocks.push_back(Fallthrough);
      Builder.SetInsertPoint(Fallthrough);
      Variables.newBasicBlock();

      return v { };
    }
  case PTC_INSTRUCTION_op_exit_tb:
    {
      Builder.CreateCall(JumpTargets.exitTB(), { });
      Builder.CreateUnreachable();

      auto *NextBB = BasicBlock::Create(Context, "", TheFunction);
      Blocks.push_back(NextBB);
      Builder.SetInsertPoint(NextBB);
      Variables.newBasicBlock();

      return v { };
    }
  case PTC_INSTRUCTION_op_goto_tb:
    // Nothing to do here
    return v { };
  case PTC_INSTRUCTION_op_add2_i32:
  case PTC_INSTRUCTION_op_sub2_i32:
  case PTC_INSTRUCTION_op_add2_i64:
  case PTC_INSTRUCTION_op_sub2_i64:
    {
      Value *FirstOperandLow = nullptr;
      Value *FirstOperandHigh = nullptr;
      Value *SecondOperandLow = nullptr;
      Value *SecondOperandHigh = nullptr;

      IntegerType *DestinationType = Builder.getIntNTy(RegisterSize * 2);

      FirstOperandLow = Builder.CreateSExt(InArguments[0], DestinationType);
      FirstOperandHigh = Builder.CreateSExt(InArguments[1], DestinationType);
      SecondOperandLow = Builder.CreateSExt(InArguments[2], DestinationType);
      SecondOperandHigh = Builder.CreateSExt(InArguments[3], DestinationType);

      FirstOperandHigh = Builder.CreateShl(FirstOperandHigh, RegisterSize);
      SecondOperandHigh = Builder.CreateShl(SecondOperandHigh, RegisterSize);

      Value *FirstOperand = Builder.CreateOr(FirstOperandHigh, FirstOperandLow);
      Value *SecondOperand = Builder.CreateOr(SecondOperandHigh,
                                              SecondOperandLow);

      Instruction::BinaryOps BinaryOp = opcodeToBinaryOp(Opcode);

      Value *Result = Builder.CreateBinOp(BinaryOp, FirstOperand, SecondOperand);

      Value *ResultLow = Builder.CreateTrunc(Result, RegisterType);
      Value *ShiftedResult = Builder.CreateLShr(Result, RegisterSize);
      Value *ResultHigh = Builder.CreateTrunc(ShiftedResult, RegisterType);

      return v { ResultLow, ResultHigh };
    }
  case PTC_INSTRUCTION_op_mulu2_i32:
  case PTC_INSTRUCTION_op_mulu2_i64:
  case PTC_INSTRUCTION_op_muls2_i32:
  case PTC_INSTRUCTION_op_muls2_i64:
    {
      IntegerType *DestinationType = Builder.getIntNTy(RegisterSize * 2);

      Value *FirstOperand = nullptr;
      Value *SecondOperand = nullptr;

      if (Opcode == PTC_INSTRUCTION_op_mulu2_i32
          || Opcode == PTC_INSTRUCTION_op_mulu2_i64) {
        FirstOperand = Builder.CreateZExt(InArguments[0], DestinationType);
        SecondOperand = Builder.CreateZExt(InArguments[1], DestinationType);
      } else if (Opcode == PTC_INSTRUCTION_op_muls2_i32
                 || Opcode == PTC_INSTRUCTION_op_muls2_i64) {
        FirstOperand = Builder.CreateSExt(InArguments[0], DestinationType);
        SecondOperand = Builder.CreateSExt(InArguments[1], DestinationType);
      } else
        llvm_unreachable("Unexpected opcode");

      Value *Result = Builder.CreateMul(FirstOperand, SecondOperand);

      Value *ResultLow = Builder.CreateTrunc(Result, RegisterType);
      Value *ShiftedResult = Builder.CreateLShr(Result, RegisterSize);
      Value *ResultHigh = Builder.CreateTrunc(ShiftedResult, RegisterType);

      return v { ResultLow, ResultHigh };
    }
  case PTC_INSTRUCTION_op_muluh_i32:
  case PTC_INSTRUCTION_op_mulsh_i32:
  case PTC_INSTRUCTION_op_muluh_i64:
  case PTC_INSTRUCTION_op_mulsh_i64:

  case PTC_INSTRUCTION_op_setcond2_i32:

  case PTC_INSTRUCTION_op_trunc_shr_i32:
    llvm_unreachable("Instruction not implemented");
  default:
    llvm_unreachable("Unknown opcode");
  }
}