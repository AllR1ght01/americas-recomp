#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"

#include <algorithm>
#include <vector>

namespace ShaderRecompiler::Detail {

bool IsRawRead(const IrResourcePlan& program, const IrValue& inst) {
    return IsRawRead(program, inst.Opcode(), inst.Flags<MemoryFlags>().index);
}

bool IsRawRead(const IrResourcePlan& program, IrOpcode op, std::uint32_t index) {
    if (op != IrOpcode::LoadAddressU32 && op != IrOpcode::ReadConstBuffer) {
        return false;
    }
    if (index >= program.memoryInfo.size()) {
        return false;
    }
    const auto kind = program.memoryInfo[index].kind;
    return (op == IrOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) || (op == IrOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

bool IsMemoryRead(IrOpcode opcode) {
    return opcode == IrOpcode::ReadConst || opcode == IrOpcode::LoadAddressU32 || opcode == IrOpcode::ReadConstBuffer;
}

bool LoadedFromMemory(const IrValue& handle) {
    std::vector<const IrValue*> pending;
    std::vector<const IrValue*> visited;
    for (std::size_t index = 0; index < handle.ArgumentCount(); ++index) pending.push_back(handle.Argument(index));
    while (!pending.empty()) {
        const IrValue* value = pending.back();
        pending.pop_back();
        if (value == nullptr) continue;
        value = value->Resolve();
        if (value->HasImmediate() || std::find(visited.begin(), visited.end(), value) != visited.end()) continue;
        visited.push_back(value);
        if (IsMemoryRead(value->Opcode())) return true;
        for (std::size_t index = 0; index < value->ArgumentCount(); ++index) pending.push_back(value->Argument(index));
    }
    return false;
}

bool IsDescriptorHandle(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::GetBufferResource:
        case IrOpcode::GetAddressResource:
        case IrOpcode::GetImageResource:
        case IrOpcode::GetSamplerResource: return true;
        default: return false;
    }
}

bool IsRuntimeSelect(IrOpcode opcode) {
    return opcode == IrOpcode::SelectU1 || opcode == IrOpcode::SelectU32 || opcode == IrOpcode::SelectF32;
}

namespace {

bool extractBitTest(const IrValue* value, const IrValue*& word, const IrValue*& shift) {
    if (value == nullptr) return false;
    value = value->Resolve();
    if (value->Opcode() != IrOpcode::INotEqual32 || value->ArgumentCount() != 2u) return false;
    const IrValue* a = value->Argument(0)->Resolve();
    const IrValue* b = value->Argument(1)->Resolve();
    const IrValue* masked = nullptr;
    if (b->HasImmediate() && b->Type() == IrType::U32 && b->ImmediateU32() == 0u) masked = a;
    else if (a->HasImmediate() && a->Type() == IrType::U32 && a->ImmediateU32() == 0u) masked = b;
    else return false;
    if (masked->Opcode() != IrOpcode::BitwiseAnd32 || masked->ArgumentCount() != 2u) return false;
    const IrValue* m0 = masked->Argument(0)->Resolve();
    const IrValue* m1 = masked->Argument(1)->Resolve();
    const IrValue* shifted = nullptr;
    if (m1->HasImmediate() && m1->Type() == IrType::U32 && m1->ImmediateU32() == 1u) shifted = m0;
    else if (m0->HasImmediate() && m0->Type() == IrType::U32 && m0->ImmediateU32() == 1u) shifted = m1;
    else return false;
    if (shifted->Opcode() != IrOpcode::ShiftRightLogical32 || shifted->ArgumentCount() != 2u) return false;
    word = shifted->Argument(0)->Resolve();
    shift = shifted->Argument(1)->Resolve();
    return true;
}

bool isLaneLowBitShift(const IrValue* shift) {
    if (shift == nullptr) return false;
    shift = shift->Resolve();
    if (shift->Opcode() == IrOpcode::LaneId) return true;
    if (shift->Opcode() != IrOpcode::BitwiseAnd32 || shift->ArgumentCount() != 2u) return false;
    const IrValue* a = shift->Argument(0)->Resolve();
    const IrValue* b = shift->Argument(1)->Resolve();
    return (a->Opcode() == IrOpcode::LaneId && b->HasImmediate() && b->Type() == IrType::U32 && b->ImmediateU32() == 31u) ||
           (b->Opcode() == IrOpcode::LaneId && a->HasImmediate() && a->Type() == IrType::U32 && a->ImmediateU32() == 31u);
}

bool extractBallotWord(const IrValue* word, const IrValue*& pred) {
    if (word == nullptr) return false;
    word = word->Resolve();
    if (word->Opcode() != IrOpcode::CompositeExtractU32x4 || word->ArgumentCount() != 2u) return false;
    const IrValue* idx = word->Argument(1)->Resolve();
    if (!idx->HasImmediate() || idx->Type() != IrType::U32 || idx->ImmediateU32() > 1u) return false;
    const IrValue* ballot = word->Argument(0)->Resolve();
    if (ballot->Opcode() != IrOpcode::Ballot || ballot->ArgumentCount() != 1u) return false;
    pred = ballot->Argument(0)->Resolve();
    return true;
}

bool activeMaskImplies(const IrResourcePlan& program, const IrValue* mask, const IrValue* condition, std::vector<const IrValue*>& visiting, std::uint32_t depth);

bool bitMaskImplies(const IrResourcePlan& program, const IrValue* maskWord, const IrValue* condWord, const IrValue* condTest, const IrValue* shift, std::vector<const IrValue*>& visiting, std::uint32_t depth) {
    if (maskWord == nullptr || condWord == nullptr) return false;
    maskWord = maskWord->Resolve();
    condWord = condWord->Resolve();
    if (maskWord == condWord || EquivalentValue(program, maskWord, condWord)) return true;
    if (depth == 0u) return false;
    if (maskWord->Opcode() == IrOpcode::BitwiseAnd32 && maskWord->ArgumentCount() == 2u) {
        return bitMaskImplies(program, maskWord->Argument(0), condWord, condTest, shift, visiting, depth - 1u) ||
               bitMaskImplies(program, maskWord->Argument(1), condWord, condTest, shift, visiting, depth - 1u);
    }
    if (condWord->Opcode() == IrOpcode::BitwiseAnd32 && condWord->ArgumentCount() == 2u) {
        return bitMaskImplies(program, maskWord, condWord->Argument(0), condTest, shift, visiting, depth - 1u) &&
               bitMaskImplies(program, maskWord, condWord->Argument(1), condTest, shift, visiting, depth - 1u);
    }
    if (maskWord->IsPhi() && maskWord->ArgumentCount() != 0u) {
        if (std::find(visiting.begin(), visiting.end(), maskWord) != visiting.end()) return true;
        visiting.push_back(maskWord);
        bool all = true;
        for (std::size_t index = 0; all && index < maskWord->ArgumentCount(); ++index) {
            all = bitMaskImplies(program, maskWord->Argument(index), condWord, condTest, shift, visiting, depth - 1u);
        }
        visiting.pop_back();
        return all;
    }
    const IrValue* ballotPred = nullptr;
    if (condTest != nullptr && isLaneLowBitShift(shift) && extractBallotWord(maskWord, ballotPred)) {
        return activeMaskImplies(program, ballotPred, condTest, visiting, depth - 1u);
    }
    return false;
}

bool activeMaskImplies(const IrResourcePlan& program, const IrValue* mask, const IrValue* condition, std::vector<const IrValue*>& visiting, std::uint32_t depth) {
    if (mask == nullptr || condition == nullptr) return false;
    mask = mask->Resolve();
    condition = condition->Resolve();
    if (mask == condition) return true;
    if (EquivalentValue(program, mask, condition)) return true;
    if (depth == 0u) return false;
    if (mask->Opcode() == IrOpcode::LogicalAnd && mask->ArgumentCount() == 2) {
        return activeMaskImplies(program, mask->Argument(0), condition, visiting, depth - 1u) || activeMaskImplies(program, mask->Argument(1), condition, visiting, depth - 1u);
    }
    if (mask->Opcode() == IrOpcode::LogicalXor && mask->ArgumentCount() == 2) {
        if (std::find(visiting.begin(), visiting.end(), mask) != visiting.end()) return true;
        visiting.push_back(mask);
        const bool both = activeMaskImplies(program, mask->Argument(0), condition, visiting, depth - 1u) &&
                          activeMaskImplies(program, mask->Argument(1), condition, visiting, depth - 1u);
        visiting.pop_back();
        return both;
    }
    if (mask->IsPhi() && mask->ArgumentCount() != 0u) {
        if (std::find(visiting.begin(), visiting.end(), mask) != visiting.end()) return true;
        visiting.push_back(mask);
        bool all = true;
        for (std::size_t index = 0; all && index < mask->ArgumentCount(); ++index) {
            all = activeMaskImplies(program, mask->Argument(index), condition, visiting, depth - 1u);
        }
        visiting.pop_back();
        return all;
    }
    const IrValue* maskWord = nullptr;
    const IrValue* maskShift = nullptr;
    const IrValue* condWord = nullptr;
    const IrValue* condShift = nullptr;
    if (extractBitTest(mask, maskWord, maskShift) && extractBitTest(condition, condWord, condShift) &&
        EquivalentValue(program, maskShift, condShift)) {
        return bitMaskImplies(program, maskWord, condWord, condition, maskShift, visiting, depth - 1u);
    }
    return false;
}

bool activeMaskDisjoint(const IrResourcePlan& program, const IrValue* mask, const IrValue* condition, std::vector<std::pair<const IrValue*, const IrValue*>>& visiting, std::uint32_t depth);

bool bitMaskWordDisjoint(const IrResourcePlan& program, const IrValue* otherBool, const IrValue* word, std::vector<std::pair<const IrValue*, const IrValue*>>& visiting, std::uint32_t depth) {
    if (otherBool == nullptr || word == nullptr || depth == 0u) return false;
    word = word->Resolve();
    if (word->Opcode() == IrOpcode::BitwiseAnd32 && word->ArgumentCount() == 2u) {
        return bitMaskWordDisjoint(program, otherBool, word->Argument(0), visiting, depth - 1u) ||
               bitMaskWordDisjoint(program, otherBool, word->Argument(1), visiting, depth - 1u);
    }
    if (word->IsPhi() && word->ArgumentCount() != 0u) {
        const auto pair = std::make_pair(otherBool, word);
        if (std::find(visiting.begin(), visiting.end(), pair) != visiting.end()) return true;
        visiting.push_back(pair);
        bool all = true;
        for (std::size_t index = 0; all && index < word->ArgumentCount(); ++index) {
            all = bitMaskWordDisjoint(program, otherBool, word->Argument(index), visiting, depth - 1u);
        }
        visiting.pop_back();
        return all;
    }
    const IrValue* ballotPred = nullptr;
    if (extractBallotWord(word, ballotPred)) {
        return activeMaskDisjoint(program, otherBool, ballotPred, visiting, depth - 1u);
    }
    return false;
}

bool activeMaskDisjoint(const IrResourcePlan& program, const IrValue* mask, const IrValue* condition, std::vector<std::pair<const IrValue*, const IrValue*>>& visiting, std::uint32_t depth) {
    if (mask == nullptr || condition == nullptr || depth == 0u) return false;
    mask = mask->Resolve();
    condition = condition->Resolve();
    if (condition->Opcode() == IrOpcode::LogicalNot && condition->ArgumentCount() == 1u) {
        std::vector<const IrValue*> impVisiting;
        return activeMaskImplies(program, mask, condition->Argument(0), impVisiting, 48u);
    }
    if (mask->Opcode() == IrOpcode::LogicalNot && mask->ArgumentCount() == 1u) {
        std::vector<const IrValue*> impVisiting;
        return activeMaskImplies(program, condition, mask->Argument(0), impVisiting, 48u);
    }
    if (condition->Opcode() == IrOpcode::LogicalAnd && condition->ArgumentCount() == 2u) {
        return activeMaskDisjoint(program, mask, condition->Argument(0), visiting, depth - 1u) ||
               activeMaskDisjoint(program, mask, condition->Argument(1), visiting, depth - 1u);
    }
    if (mask->Opcode() == IrOpcode::LogicalAnd && mask->ArgumentCount() == 2u) {
        return activeMaskDisjoint(program, mask->Argument(0), condition, visiting, depth - 1u) ||
               activeMaskDisjoint(program, mask->Argument(1), condition, visiting, depth - 1u);
    }
    if (condition->Opcode() == IrOpcode::LogicalXor && condition->ArgumentCount() == 2u) {
        const auto pair = std::make_pair(mask, condition);
        if (std::find(visiting.begin(), visiting.end(), pair) != visiting.end()) return true;
        visiting.push_back(pair);
        const bool both = activeMaskDisjoint(program, mask, condition->Argument(0), visiting, depth - 1u) &&
                          activeMaskDisjoint(program, mask, condition->Argument(1), visiting, depth - 1u);
        visiting.pop_back();
        return both;
    }
    if (mask->Opcode() == IrOpcode::LogicalXor && mask->ArgumentCount() == 2u) {
        const auto pair = std::make_pair(mask, condition);
        if (std::find(visiting.begin(), visiting.end(), pair) != visiting.end()) return true;
        visiting.push_back(pair);
        const bool both = activeMaskDisjoint(program, mask->Argument(0), condition, visiting, depth - 1u) &&
                          activeMaskDisjoint(program, mask->Argument(1), condition, visiting, depth - 1u);
        visiting.pop_back();
        return both;
    }
    if (condition->IsPhi() && condition->ArgumentCount() != 0u) {
        const auto pair = std::make_pair(mask, condition);
        if (std::find(visiting.begin(), visiting.end(), pair) != visiting.end()) return true;
        visiting.push_back(pair);
        bool all = true;
        for (std::size_t index = 0; all && index < condition->ArgumentCount(); ++index) {
            all = activeMaskDisjoint(program, mask, condition->Argument(index), visiting, depth - 1u);
        }
        visiting.pop_back();
        return all;
    }
    if (mask->IsPhi() && mask->ArgumentCount() != 0u) {
        const auto pair = std::make_pair(mask, condition);
        if (std::find(visiting.begin(), visiting.end(), pair) != visiting.end()) return true;
        visiting.push_back(pair);
        bool all = true;
        for (std::size_t index = 0; all && index < mask->ArgumentCount(); ++index) {
            all = activeMaskDisjoint(program, mask->Argument(index), condition, visiting, depth - 1u);
        }
        visiting.pop_back();
        return all;
    }
    const IrValue* word = nullptr;
    const IrValue* shift = nullptr;
    if (extractBitTest(condition, word, shift) && isLaneLowBitShift(shift)) {
        if (bitMaskWordDisjoint(program, mask, word, visiting, depth - 1u)) return true;
    }
    if (extractBitTest(mask, word, shift) && isLaneLowBitShift(shift)) {
        if (bitMaskWordDisjoint(program, condition, word, visiting, depth - 1u)) return true;
    }
    return false;
}

}

bool ActiveMaskImplies(const IrResourcePlan& program, const IrValue* mask, const IrValue* condition) {
    std::vector<const IrValue*> visiting;
    return activeMaskImplies(program, mask, condition, visiting, 48u);
}

bool ActiveMaskDisjoint(const IrResourcePlan& program, const IrValue* mask, const IrValue* condition) {
    std::vector<std::pair<const IrValue*, const IrValue*>> visiting;
    return activeMaskDisjoint(program, mask, condition, visiting, 48u);
}

bool IsRuntimeUniformOp(IrOpcode opcode) {
    switch (opcode) {
        case IrOpcode::BitCastU32F32:
        case IrOpcode::BitCastF32U32:
        case IrOpcode::ConvertU32F32:
        case IrOpcode::ConvertF32U32:
        case IrOpcode::CompositeConstructU64:
        case IrOpcode::CompositeExtractU64:
        case IrOpcode::CompositeConstructU32x2:
        case IrOpcode::CompositeExtractU32x2:
        case IrOpcode::BitFieldInsert:
        case IrOpcode::BitFieldUExtract:
        case IrOpcode::BitFieldSExtract:
        case IrOpcode::IAdd32:
        case IrOpcode::IAdd64:
        case IrOpcode::IAddCarry32:
        case IrOpcode::ISub32:
        case IrOpcode::ISub64:
        case IrOpcode::IMul32:
        case IrOpcode::IMul64:
        case IrOpcode::SMin32:
        case IrOpcode::UMin32:
        case IrOpcode::ShiftLeftLogical32:
        case IrOpcode::ShiftLeftLogical64:
        case IrOpcode::ShiftRightLogical32:
        case IrOpcode::ShiftRightLogical64:
        case IrOpcode::ShiftRightArithmetic32:
        case IrOpcode::ShiftRightArithmetic64:
        case IrOpcode::BitwiseAnd32:
        case IrOpcode::BitwiseAnd64:
        case IrOpcode::BitwiseOr32:
        case IrOpcode::BitwiseXor32:
        case IrOpcode::BitwiseNot32:
        case IrOpcode::BitCount32:
        case IrOpcode::FindILsb32:
        case IrOpcode::FindUMsb32:
        case IrOpcode::SelectU1:
        case IrOpcode::SelectU32:
        case IrOpcode::SelectF32:
        case IrOpcode::SLessThan32:
        case IrOpcode::ULessThan32:
        case IrOpcode::IEqual32:
        case IrOpcode::UGreaterThan32:
        case IrOpcode::UGreaterThanEqual32:
        case IrOpcode::INotEqual32:
        case IrOpcode::LogicalOr:
        case IrOpcode::LogicalAnd:
        case IrOpcode::LogicalXor:
        case IrOpcode::LogicalNot:
        case IrOpcode::FPOrdLessThanEqual32:
        case IrOpcode::FPOrdGreaterThanEqual32:
        case IrOpcode::FPIsNan32:
        case IrOpcode::FPMul32:
        case IrOpcode::NanPropagateF32:
        case IrOpcode::FPTrunc32: return true;
        default: return false;
    }
}

namespace {

bool holdsFoldableValue(IrValue* value, std::uint32_t depth, std::vector<const IrValue*>& seen) {
    value = value->Resolve();
    if (value->HasImmediate()) {
        return false;
    }
    switch (value->Opcode()) {
        case IrOpcode::GetShaderBase:
        case IrOpcode::GetExec:
        case IrOpcode::GetExecLo:
        case IrOpcode::GetExecHi:
        case IrOpcode::Ballot:
        case IrOpcode::LaneId:
        case IrOpcode::ImageRead:
        case IrOpcode::ImageSampleExplicitLod:
        case IrOpcode::ImageSampleImplicitLod:
        case IrOpcode::ImageSampleRaw: return true;
        default: break;
    }
    if (depth == 0u) {
        return false;
    }
    if (value->IsPhi()) {
        if (std::ranges::find(seen, value) != seen.end()) {
            return false;
        }
        seen.push_back(value);
    } else if (!IsRuntimeUniformOp(value->Opcode()) && value->Opcode() != IrOpcode::CompositeExtractU32x4 && value->Opcode() != IrOpcode::WqmU64) {
        return false;
    }
    for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
        if (holdsFoldableValue(value->Argument(index), depth - 1u, seen)) {
            return true;
        }
    }
    return false;
}

}

bool HoldsFoldableValue(IrValue* value, std::uint32_t depth) {
    std::vector<const IrValue*> seen;
    return holdsFoldableValue(value, depth, seen);
}

}
