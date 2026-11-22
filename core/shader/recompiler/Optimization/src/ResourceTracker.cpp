#include "Optimization/ResourceTracker.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "Optimization/SrtWalker.hpp"
#include "Optimization/ResourceMaterializer.hpp"
#include "SpirvBackend/SpirvBufferFormat.hpp"
#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
namespace {

constexpr std::uint32_t samplerBorderClampMask = (1u << 2u) | (1u << 5u) | (1u << 8u);
constexpr std::uint32_t samplerDword3ReservedMask = 0x3ffff000u;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

// Debug aid: APS5_TRACE_BDA=1 names the accesses that make a program address-based (see Collect).
bool bdaTraceEnabled() {
    static const bool enabled = std::getenv("APS5_TRACE_BDA") != nullptr;
    return enabled;
}
constexpr unsigned bdaTraceLimit = 8;

std::string formatHex32(std::uint32_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex(8u, '0');
    for (std::uint32_t index = 0; index < 8u; index++) {
        hex[7u - index] = digits[(value >> (index * 4u)) & 0xfu];
    }
    return hex;
}

std::string describeValueChain(const IrValue* value, std::uint32_t depth) {
    value = value->Resolve();
    if (value->HasImmediate()) {
        return "Immediate";
    }
    std::string text = std::string(IrOpcodeName(value->Opcode()));
    if (value->Opcode() == IrOpcode::LoadAddressU32 || value->Opcode() == IrOpcode::ReadConstBuffer) {
        text += "@pc=0x" + formatHex32(value->Flags<MemoryFlags>().pc);
    }
    if (depth == 0u || value->ArgumentCount() == 0u) {
        return text;
    }
    text += "(";
    for (std::uint32_t index = 0; index < value->ArgumentCount(); index++) {
        if (index != 0u) {
            text += ", ";
        }
        text += describeValueChain(value->Argument(index), depth - 1u);
    }
    text += ")";
    return text;
}

std::uint32_t possibleU32BitsDepth(const IrValue* value, std::uint32_t depth) {
    value = value->Resolve();
    if (value->HasImmediate()) {
        return value->Type() == IrType::U32 ? value->ImmediateU32() : std::numeric_limits<std::uint32_t>::max();
    }
    if (depth == 0u) return std::numeric_limits<std::uint32_t>::max();
    switch (value->Opcode()) {
        case IrOpcode::BitwiseAnd32:
            return possibleU32BitsDepth(value->Argument(0), depth - 1u) & possibleU32BitsDepth(value->Argument(1), depth - 1u);
        case IrOpcode::BitwiseOr32:
            return possibleU32BitsDepth(value->Argument(0), depth - 1u) | possibleU32BitsDepth(value->Argument(1), depth - 1u);
        case IrOpcode::ShiftLeftLogical32: {
            const IrValue* shift = value->Argument(1)->Resolve();
            return shift->HasImmediate() && shift->Type() == IrType::U32 ? possibleU32BitsDepth(value->Argument(0), depth - 1u) << (shift->ImmediateU32() & 31u) : std::numeric_limits<std::uint32_t>::max();
        }
        case IrOpcode::ReadFirstLane:
            return value->ArgumentCount() >= 1u ? possibleU32BitsDepth(value->Argument(0), depth - 1u) : std::numeric_limits<std::uint32_t>::max();
        case IrOpcode::SelectU32:
            return value->ArgumentCount() == 3u ? (possibleU32BitsDepth(value->Argument(1), depth - 1u) | possibleU32BitsDepth(value->Argument(2), depth - 1u)) : std::numeric_limits<std::uint32_t>::max();
        default:
            return std::numeric_limits<std::uint32_t>::max();
    }
}

std::uint32_t possibleU32Bits(const IrValue* value) {
    return possibleU32BitsDepth(value, 8u);
}

std::uint32_t possibleU32UpperBoundDepth(const IrValue* value, std::uint32_t depth) {
    value = value->Resolve();
    if (value->HasImmediate()) {
        return value->Type() == IrType::U32 ? value->ImmediateU32() : std::numeric_limits<std::uint32_t>::max();
    }
    if (depth == 0u) return std::numeric_limits<std::uint32_t>::max();
    const auto add = [](std::uint32_t left, std::uint32_t right) {
        return left > std::numeric_limits<std::uint32_t>::max() - right ? std::numeric_limits<std::uint32_t>::max() : left + right;
    };
    switch (value->Opcode()) {
        case IrOpcode::BitwiseAnd32:
            return std::min(possibleU32UpperBoundDepth(value->Argument(0), depth - 1u), possibleU32UpperBoundDepth(value->Argument(1), depth - 1u));
        case IrOpcode::BitwiseOr32:
            return add(possibleU32UpperBoundDepth(value->Argument(0), depth - 1u), possibleU32UpperBoundDepth(value->Argument(1), depth - 1u));
        case IrOpcode::ShiftLeftLogical32: {
            const IrValue* shift = value->Argument(1)->Resolve();
            if (!shift->HasImmediate() || shift->Type() != IrType::U32) return std::numeric_limits<std::uint32_t>::max();
            const auto bits = shift->ImmediateU32() & 31u;
            const auto upper = possibleU32UpperBoundDepth(value->Argument(0), depth - 1u);
            return bits != 0u && upper > (std::numeric_limits<std::uint32_t>::max() >> bits) ? std::numeric_limits<std::uint32_t>::max() : upper << bits;
        }
        case IrOpcode::ReadFirstLane:
            return value->ArgumentCount() >= 1u ? possibleU32UpperBoundDepth(value->Argument(0), depth - 1u) : std::numeric_limits<std::uint32_t>::max();
        case IrOpcode::SelectU32:
            return value->ArgumentCount() == 3u ? (possibleU32UpperBoundDepth(value->Argument(1), depth - 1u) | possibleU32UpperBoundDepth(value->Argument(2), depth - 1u)) : std::numeric_limits<std::uint32_t>::max();
        case IrOpcode::UMin32:
            return value->ArgumentCount() == 2u ? std::min(possibleU32UpperBoundDepth(value->Argument(0), depth - 1u), possibleU32UpperBoundDepth(value->Argument(1), depth - 1u)) : std::numeric_limits<std::uint32_t>::max();
        case IrOpcode::UMax32:
            return value->ArgumentCount() == 2u ? std::max(possibleU32UpperBoundDepth(value->Argument(0), depth - 1u), possibleU32UpperBoundDepth(value->Argument(1), depth - 1u)) : std::numeric_limits<std::uint32_t>::max();
        case IrOpcode::IAdd32:
            return value->ArgumentCount() == 2u ? add(possibleU32UpperBoundDepth(value->Argument(0), depth - 1u), possibleU32UpperBoundDepth(value->Argument(1), depth - 1u)) : std::numeric_limits<std::uint32_t>::max();
        case IrOpcode::Phi: {
            std::uint32_t upper = 0;
            for (std::uint32_t index = 0; index < value->ArgumentCount(); ++index) upper |= possibleU32UpperBoundDepth(value->Argument(index), depth - 1u);
            return upper;
        }
        case IrOpcode::FindILsb32:
        case IrOpcode::FindUMsb32:
            return 31u;
        default:
            return std::numeric_limits<std::uint32_t>::max();
    }
}

std::uint32_t possibleU32UpperBound(const IrValue* value) {
    return possibleU32UpperBoundDepth(value, 8u);
}

std::uint32_t byteExtent(const MemoryInfo& memory) {
    const auto bytes = std::max((memory.dataBits + 7u) / 8u, 1u);
    const auto count = std::max(memory.dataDwords, 1u);
    const auto end = static_cast<std::uint64_t>(memory.offset) + static_cast<std::uint64_t>(bytes) * count;
    return end > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(end);
}

class Tracker {
public:
    explicit Tracker(IrProgram& program) : m_program(program), m_info(program.Resources().info), m_builder(program) {
        m_info.buffers.clear();
        m_info.images.clear();
        m_info.samplers.clear();
        m_info.sampledPairs.clear();
        m_info.usesDma = false;
        m_info.bdaWrites = false;
    }

    void Run() {
        if (m_program.Resources().resourceTrackingComplete) {
            fail("resources already tracked");
        }
        if (!m_program.Resources().srtPlanComplete) {
            fail("SRT plan is not ready");
        }
        SplitDescriptorPhis();
        if (m_foldedWeb) {
            DeadCodeEliminator{}.Eliminate(m_program);
        }
        PlanTableColumns();
        std::unordered_map<const IrBlock*, std::uint32_t> blockIndices;
        for (const IrBlock* block : m_program.BlockOrder()) {
            blockIndices.emplace(block, static_cast<std::uint32_t>(blockIndices.size()));
        }
        m_blockSources.assign(blockIndices.size(), {});
        m_blockImages.assign(blockIndices.size(), {});
        for (auto& block : m_program.Blocks()) {
            const auto found = blockIndices.find(block.get());
            m_currentBlock = found == blockIndices.end() ? std::numeric_limits<std::uint32_t>::max() : found->second;
            for (IrValue* inst : block->Instructions()) {
                Collect(*inst);
            }
        }
        m_currentBlock = std::numeric_limits<std::uint32_t>::max();
        PlanResourceControlFlow();
        if (m_bdaTraces > bdaTraceLimit) std::fprintf(stderr, "[bda] %u more address accesses in this program not shown\n", m_bdaTraces - bdaTraceLimit);
        CheckTableStores();
        AssignTables();
        LinkImageAliases();
        for (const auto& patch : m_handlePatches) {
            patch.handle->SetFlags<std::uint32_t>(patch.resource);
        }
        for (const auto& patch : m_memoryPatches) {
            auto& memory = m_program.Resources().memoryInfo[patch.index];
            memory.resource = patch.resource;
            if (patch.hasSampler) {
                memory.sampler = patch.sampler;
            }
        }
        std::vector<const IrValue*> referenced;
        for (const auto& plan : m_tableColumns) {
            const auto& column = *m_sources[plan.source].tableColumn;
            IrValue* offset = column.selected ? &m_builder.Constant(column.addend) : plan.offset;
            for (std::uint32_t argument = 0; argument < plan.handle->ArgumentCount(); argument++) {
                const auto value = plan.parentPointer != nullptr && (argument == 1u || argument == 2u) ? plan.parentPointer->Argument(argument - 1u) : offset;
                plan.handle->ReplaceArgument(argument, value);
            }
            KeepSourceAlive(*plan.handle, m_sources[column.heapSource], referenced);
            if (column.selected) {
                KeepSourceAlive(*plan.handle, m_sources[column.selection], referenced);
            }
            if (plan.keyDomain.has_value()) {
                KeepSourceAlive(*plan.handle, m_sources[plan.keyDomain->source], referenced);
            }
        }
        for (const auto index : m_tablePlanningMemory) {
            m_program.Resources().memoryInfo[index].planningOnly = true;
        }
        std::erase_if(m_program.Metadata().dynamicReads, [&](IrValue* value) {
            const IrValue* inst = value->Resolve();
            return std::ranges::find(m_tablePlanningReads, inst) != m_tablePlanningReads.end();
        });
        m_program.Resources().descriptorSources = std::move(m_sources);
        m_program.Resources().info = std::move(m_info);
        m_program.Resources().resourceTrackingComplete = true;
    }

private:
    struct HandlePatch {
        IrValue* handle = nullptr;
        std::uint32_t resource = 0;
    };

    struct MemoryPatch {
        std::uint32_t index = 0;
        std::uint32_t resource = 0;
        std::uint32_t sampler = 0;
        bool hasSampler = false;
    };

    struct TableColumnPlan {
        IrValue* handle = nullptr;
        std::uint32_t source = 0;
        IrValue* offset = nullptr;
        IrValue* parentPointer = nullptr;
        std::vector<std::uint32_t> memory;
        std::vector<const IrValue*> reads;
        std::vector<std::uint32_t> hopMemory;
        std::vector<const IrValue*> hopReads;
        std::optional<KeyDomain> keyDomain;
    };

    void MakeSource(const IrValue& handle, std::uint32_t width, bool sampler, bool sampleAdjust, DescriptorSource& descriptor) {
        if (handle.ArgumentCount() != width) {
            fail(std::string(IrOpcodeName(handle.Opcode())) + " has " + std::to_string(handle.ArgumentCount()) + " descriptor dwords, expected " + std::to_string(width));
        }
        descriptor.dwordCount = width;
        for (std::uint32_t i = 0; i < width; i++) {
            descriptor.dwords[i] = handle.Argument(i)->Resolve();
        }
        if (sampleAdjust) {
            descriptor.dwords[3] = canonicalizeSampleAdjustDword3(descriptor.dwords[3]);
        }
        const IrValue* dword0 = descriptor.dwords[0]->Resolve();
        if (sampler && dword0->HasImmediate() && dword0->Type() == IrType::U32 && (dword0->ImmediateU32() & samplerBorderClampMask) == 0u) {
            descriptor.dwords[3] = &m_builder.Constant(0u);
        }
    }

    IrValue* canonicalizeSampleAdjustDword3(IrValue* value) {
        for (;;) {
            value = value->Resolve();
            if ((possibleU32Bits(value) & ~samplerDword3ReservedMask) == 0u) {
                return &m_builder.Constant(0u);
            }
            if (value->Opcode() != IrOpcode::BitwiseOr32) {
                return value;
            }
            IrValue* left = value->Argument(0)->Resolve();
            IrValue* right = value->Argument(1)->Resolve();
            const bool leftReserved = (possibleU32Bits(left) & ~samplerDword3ReservedMask) == 0u;
            const bool rightReserved = (possibleU32Bits(right) & ~samplerDword3ReservedMask) == 0u;
            if (leftReserved) {
                value = right;
            } else if (rightReserved) {
                value = left;
            } else {
                return value;
            }
        }
    }

    bool ValidateSource(const DescriptorSource& descriptor, std::uint32_t& badDword) const {
        constexpr SrtWalker walker;
        for (std::uint32_t i = 0; i < descriptor.dwordCount; i++) {
            badDword = i;
            if (descriptor.dwords[i]->Resolve()->Type() != IrType::U32) {
                return false;
            }
            if (!walker.ValidateRuntimeValue(m_program.Resources(), descriptor.dwords[i])) {
                return false;
            }
        }
        return true;
    }

    std::uint32_t InternSource(DescriptorSource descriptor) {
        if (!m_readOrigins.empty()) {
            std::vector<const IrValue*> pending(descriptor.dwords.begin(), descriptor.dwords.begin() + descriptor.dwordCount);
            std::unordered_set<const IrValue*> visited;
            const auto& order = m_program.BlockOrder();
            while (!pending.empty()) {
                const IrValue* value = pending.back()->Resolve();
                pending.pop_back();
                if (!visited.insert(value).second) continue;
                if (const auto origin = m_readOrigins.find(value); origin != m_readOrigins.end()) {
                    const auto block = std::ranges::find(order, origin->second);
                    if (block == order.end()) fail("copied descriptor read has no resource control-flow block");
                    descriptor.requiredBlocks.push_back(static_cast<std::uint32_t>(block - order.begin()));
                }
                for (const IrValue* argument : value->Arguments()) pending.push_back(argument);
            }
            std::ranges::sort(descriptor.requiredBlocks);
            descriptor.requiredBlocks.erase(std::unique(descriptor.requiredBlocks.begin(), descriptor.requiredBlocks.end()), descriptor.requiredBlocks.end());
        }
        for (std::uint32_t candidate = 0; candidate < m_sources.size(); candidate++) {
            const auto& current = m_sources[candidate];
            if (current.dwordCount != descriptor.dwordCount || current.tableColumn != descriptor.tableColumn || current.requiredBlocks != descriptor.requiredBlocks) {
                continue;
            }
            bool same = true;
            for (std::uint32_t i = 0; i < descriptor.dwordCount; i++) {
                same = same && EquivalentValue(m_program.Resources(), current.dwords[i], descriptor.dwords[i]);
            }
            if (same) {
                NoteBlockSource(candidate);
                return candidate;
            }
        }
        m_sources.push_back(descriptor);
        NoteBlockSource(static_cast<std::uint32_t>(m_sources.size() - 1));
        return static_cast<std::uint32_t>(m_sources.size() - 1);
    }

    void NoteBlockImage(std::uint32_t image) {
        if (m_currentBlock >= m_blockImages.size()) {
            return;
        }
        auto& images = m_blockImages[m_currentBlock];
        if (std::ranges::find(images, image) == images.end()) {
            images.push_back(image);
        }
    }

    void NoteBlockSource(std::uint32_t source) {
        if (m_currentBlock >= m_blockSources.size()) {
            return;
        }
        auto& sources = m_blockSources[m_currentBlock];
        if (std::ranges::find(sources, source) == sources.end()) {
            sources.push_back(source);
        }
    }

    void PlanResourceControlFlow() {
        const auto& order = m_program.BlockOrder();
        const auto& info = m_program.Metadata().blockInfo;
        if (order.empty() || order.size() != info.size()) {
            if (std::ranges::any_of(m_sources, [](const DescriptorSource& source) { return !source.requiredBlocks.empty(); })) fail("copied descriptor reads require resource control flow");
            return;
        }
        std::unordered_map<const IrBlock*, std::uint32_t> indexOf;
        std::unordered_map<std::uint32_t, std::uint32_t> idToIndex;
        for (std::uint32_t index = 0; index < order.size(); index++) {
            indexOf.emplace(order[index], index);
            idToIndex.emplace(info[index].id, index);
        }
        std::vector<ResourceBlock> controlFlow(order.size());
        for (std::uint32_t index = 0; index < order.size(); index++) {
            auto& block = controlFlow[index];
            const auto& terminator = info[index].terminator;
            const auto target = [&](std::uint32_t id) { return idToIndex.find(id); };
            const auto trueEdge = target(terminator.trueBlock);
            const auto falseEdge = target(terminator.falseBlock);
            if (terminator.kind == TerminatorKind::ConditionalBranch && !terminator.loopHeader && info[index].condition != nullptr && trueEdge != idToIndex.end() && falseEdge != idToIndex.end()) {
                block.condition = info[index].condition;
                block.successors = {trueEdge->second, falseEdge->second};
            } else if (terminator.kind != TerminatorKind::Return) {
                for (const IrBlock* successor : order[index]->Successors()) {
                    const auto found = indexOf.find(successor);
                    if (found == indexOf.end()) {
                        return;
                    }
                    block.successors.push_back(found->second);
                }
            }
            block.sources = m_blockSources[index];
            block.images = m_blockImages[index];
            for (const IrValue* inst : order[index]->Instructions()) {
                if (inst->Opcode() != IrOpcode::ReadConst || inst->ArgumentCount() != 2u || inst->Argument(0)->Resolve()->Opcode() != IrOpcode::GetSrtResource) continue;
                const IrValue* slot = inst->Argument(1)->Resolve();
                if (!slot->HasImmediate() || slot->Type() != IrType::U32 || slot->ImmediateU32() >= m_program.Resources().srtReads.size()) fail("resource control flow has an invalid flat SRT slot");
                if (std::ranges::find(block.flatSlots, slot->ImmediateU32()) == block.flatSlots.end()) block.flatSlots.push_back(slot->ImmediateU32());
            }
        }
        std::vector<std::uint8_t> unconditional(order.size());
        for (std::uint32_t index = 0; index < order.size() && unconditional[index] == 0u;) {
            unconditional[index] = 1u;
            if (controlFlow[index].successors.size() != 1u) {
                break;
            }
            index = controlFlow[index].successors.front();
        }
        std::vector<std::uint8_t> reached(m_sources.size());
        std::vector<std::uint8_t> reachedImages(m_info.images.size());
        std::vector<std::uint8_t> reachedFlat(m_program.Resources().srtReads.size());
        for (std::uint32_t index = 0; index < order.size(); index++) {
            if (unconditional[index] == 0u) {
                continue;
            }
            for (const auto source : controlFlow[index].sources) {
                reached[source] = 1u;
            }
            for (const auto image : controlFlow[index].images) {
                reachedImages[image] = 1u;
            }
            for (const auto slot : controlFlow[index].flatSlots) {
                reachedFlat[slot] = 1u;
            }
        }
        bool guarded = std::ranges::any_of(m_sources, [](const DescriptorSource& source) { return !source.requiredBlocks.empty(); });
        for (const auto& block : controlFlow) {
            for (const auto source : block.sources) {
                guarded = guarded || reached[source] == 0u;
            }
            for (const auto image : block.images) {
                guarded = guarded || reachedImages[image] == 0u;
            }
            for (const auto slot : block.flatSlots) {
                guarded = guarded || reachedFlat[slot] == 0u;
            }
        }
        if (guarded) {
            m_program.Resources().controlFlow = std::move(controlFlow);
        }
    }

    static bool immediateU32(IrValue* value, std::uint32_t& result) {
        value = value->Resolve();
        if (!value->HasImmediate() || value->Type() != IrType::U32) {
            return false;
        }
        result = value->ImmediateU32();
        return true;
    }

    const MemoryInfo* ScalarReadMemory(const IrValue& read, std::uint32_t& index) const {
        if (read.Opcode() != IrOpcode::ReadConstBuffer || read.ArgumentCount() != 2u) {
            return nullptr;
        }
        index = read.Flags<MemoryFlags>().index;
        if (index >= m_program.Resources().memoryInfo.size()) {
            return nullptr;
        }
        const auto& memory = m_program.Resources().memoryInfo[index];
        return memory.kind == ResourceKind::ScalarBuffer && memory.dataBits == 32u && memory.dataDwords == 1u ? &memory : nullptr;
    }

    const MemoryInfo* ScalarAddressMemory(const IrValue& read, std::uint32_t& index) const {
        if (read.Opcode() != IrOpcode::LoadAddressU32 || read.ArgumentCount() != 4u) {
            return nullptr;
        }
        index = read.Flags<MemoryFlags>().index;
        if (index >= m_program.Resources().memoryInfo.size()) {
            return nullptr;
        }
        const auto& memory = m_program.Resources().memoryInfo[index];
        std::uint32_t high = 0;
        const IrValue* active = read.Argument(3)->Resolve();
        const bool always = active->HasImmediate() && active->Type() == IrType::U1 && active->ImmediateBool();
        return memory.kind == ResourceKind::ScalarAddress && !memory.addressIsFull && memory.dataBits == 32u && memory.dataDwords == 1u && immediateU32(read.Argument(2), high) && high == 0u && always ? &memory : nullptr;
    }

    static bool AccessesMemory(IrOpcode op) {
        return BufferAccessOf(op) != BufferAccess::None || AddressOpcodeInfoOf(op).access != AddressAccess::None || ImageOpcodeInfoOf(op).access != ImageAccess::None;
    }

    void IndexMemoryUsers() {
        for (const auto& block : m_program.Blocks()) {
            for (const IrValue* inst : block->Instructions()) {
                if (AccessesMemory(inst->Opcode())) {
                    m_memoryUsers[inst->Flags<MemoryFlags>().index].push_back(inst);
                }
            }
        }
        m_memoryUsersIndexed = true;
    }

    bool MemoryIndexBelongsTo(std::uint32_t index, const IrValue& owner) const {
        if (m_memoryUsersIndexed) {
            const auto users = m_memoryUsers.find(index);
            return users == m_memoryUsers.end() || std::ranges::all_of(users->second, [&](const IrValue* user) { return user == &owner; });
        }
        for (const auto& block : m_program.Blocks()) {
            for (const IrValue* inst : block->Instructions()) {
                if (!AccessesMemory(inst->Opcode()) || inst == &owner) {
                    continue;
                }
                if (inst->Flags<MemoryFlags>().index == index) {
                    return false;
                }
            }
        }
        return true;
    }

    bool MakeRuntimeSource(const IrValue& handle, IrOpcode expected, std::uint32_t width, std::uint32_t& source, DescriptorSource& descriptor) {
        if (handle.Opcode() != expected) {
            return false;
        }
        MakeSource(handle, width, false, false, descriptor);
        std::uint32_t badDword = 0;
        if (!ValidateSource(descriptor, badDword)) {
            return false;
        }
        source = InternSource(descriptor);
        return true;
    }

    static bool MatchScale(IrValue* value, IrValue*& key, std::uint32_t& stride) {
        value = value->Resolve();
        if (value->ArgumentCount() != 2u) {
            return false;
        }
        std::uint32_t immediate = 0;
        if (value->Opcode() == IrOpcode::ShiftLeftLogical32) {
            if (!immediateU32(value->Argument(1), immediate) || immediate >= 32u) {
                return false;
            }
            key = value->Argument(0)->Resolve();
            stride = 1u << immediate;
            return true;
        }
        if (value->Opcode() != IrOpcode::IMul32) {
            return false;
        }
        if (immediateU32(value->Argument(1), immediate)) {
            key = value->Argument(0)->Resolve();
        } else if (immediateU32(value->Argument(0), immediate)) {
            key = value->Argument(1)->Resolve();
        } else {
            return false;
        }
        stride = immediate;
        return true;
    }

    // XBOX-FORK (assumption, not a proof; disable with XBOX_NO_LANE_ASSUME=1): a waterfall loop reads
    // v_readfirstlane(v) while exec is narrowed by v_cmpx. The SSA form of v is "lane was active at the write ? new : old",
    // i.e. Select(((execMask >> (lane & 31)) & 1) != 0, new, old). Proving the loop mask is a subset of that write-time mask needs
    // exec-flow analysis across different exec snapshots; instead take the "lane was active" arm, the value the shader intends.
    static bool AssumeLaneActive(const IrValue* condition) {
        static const bool disabled = std::getenv("XBOX_NO_LANE_ASSUME") != nullptr;
        if (disabled) return false;
        condition = condition->Resolve();
        if (condition->Opcode() != IrOpcode::INotEqual32 || condition->ArgumentCount() != 2u) return false;
        const IrValue* test = condition->Argument(0)->Resolve();
        if (test->Opcode() != IrOpcode::BitwiseAnd32 || test->ArgumentCount() != 2u) return false;
        const IrValue* shifted = test->Argument(0)->Resolve();
        if (shifted->Opcode() != IrOpcode::ShiftRightLogical32 || shifted->ArgumentCount() != 2u) return false;
        const IrValue* amount = shifted->Argument(1)->Resolve();
        if (amount->Opcode() != IrOpcode::BitwiseAnd32 || amount->ArgumentCount() != 2u) return false;
        return amount->Argument(0)->Resolve()->Opcode() == IrOpcode::LaneId || amount->Argument(1)->Resolve()->Opcode() == IrOpcode::LaneId;
    }

    bool MatchAffineOffset(IrValue* value, IrValue*& key, std::uint32_t& stride, std::uint32_t& addend) const {
        value = value->Resolve();
        if (value->Opcode() == IrOpcode::ReadFirstLane && value->ArgumentCount() == 2u) {
            const auto* activeMask = value->Argument(1)->Resolve();
            value = value->Argument(0)->Resolve();
            while (value->Opcode() == IrOpcode::SelectU32 && value->ArgumentCount() == 3u && (Detail::ActiveMaskImplies(m_program.Resources(), activeMask, value->Argument(0)) || AssumeLaneActive(value->Argument(0)))) {
                value = value->Argument(1)->Resolve();
            }
        }
        addend = 0;
        bool matched = MatchScale(value, key, stride);
        if (!matched && value->Opcode() == IrOpcode::IAdd32 && value->ArgumentCount() == 2u) {
            if (immediateU32(value->Argument(0), addend)) {
                matched = MatchScale(value->Argument(1), key, stride);
            } else if (immediateU32(value->Argument(1), addend)) {
                matched = MatchScale(value->Argument(0), key, stride);
            }
        }
        return matched && stride >= 4u && stride % 4u == 0u && key->Type() == IrType::U32;
    }

    static bool MatchMaskedOffset(IrValue* value, IrValue*& key, std::uint32_t& stride) {
        value = value->Resolve();
        if (value->Opcode() != IrOpcode::BitwiseAnd32 || value->ArgumentCount() != 2u) {
            return false;
        }
        std::uint32_t mask = 0;
        IrValue* scaled = nullptr;
        if (immediateU32(value->Argument(1), mask)) {
            scaled = value->Argument(0);
        } else if (immediateU32(value->Argument(0), mask)) {
            scaled = value->Argument(1);
        } else {
            return false;
        }
        return MatchScale(scaled, key, stride) && std::has_single_bit(stride) && stride >= 4u && (mask & ~(stride - 1u)) != 0u && key->Type() == IrType::U32;
    }

    IrValue* ActiveLaneValue(IrValue* value, const IrValue* mask) const {
        value = value->Resolve();
        while (mask != nullptr && Detail::IsRuntimeSelect(value->Opcode()) && value->ArgumentCount() == 3u && Detail::ActiveMaskImplies(m_program.Resources(), mask, value->Argument(0))) {
            value = value->Argument(1)->Resolve();
        }
        return value;
    }

    std::uint32_t ScaledBase(IrValue* value, const IrValue* mask, IrValue*& base) const {
        value = ActiveLaneValue(value, mask);
        std::uint32_t immediate = 0;
        std::uint32_t scale = 1u;
        while (value->ArgumentCount() == 2u && immediateU32(value->Argument(1), immediate) && ((value->Opcode() == IrOpcode::ShiftLeftLogical32 && immediate < 32u) || value->Opcode() == IrOpcode::IMul32)) {
            const std::uint64_t next = static_cast<std::uint64_t>(scale) * (value->Opcode() == IrOpcode::IMul32 ? immediate : 1u << immediate);
            if (next > std::numeric_limits<std::uint32_t>::max()) {
                break;
            }
            scale = static_cast<std::uint32_t>(next);
            value = ActiveLaneValue(value->Argument(0), mask);
        }
        base = value;
        return scale;
    }

    struct MultipleSearch {
        std::vector<const IrValue*> visiting;
        std::unordered_map<const IrValue*, std::uint32_t> known;
        const IrValue* mask = nullptr;
        bool cycle = false;
    };

    std::uint32_t KnownMultiple(IrValue* value, MultipleSearch& search, std::uint32_t depth) const {
        value = ActiveLaneValue(value, search.mask);
        std::uint32_t immediate = 0;
        if (immediateU32(value, immediate)) {
            return immediate;
        }
        if (std::ranges::find(search.visiting, value) != search.visiting.end()) {
            search.cycle = true;
            return 0u;
        }
        if (const auto found = search.known.find(value); found != search.known.end()) {
            return found->second;
        }
        if (depth == 0u || value->Type() != IrType::U32) {
            return 1u;
        }
        const bool outerCycle = search.cycle;
        search.cycle = false;
        auto& visiting = search.visiting;
        visiting.push_back(value);
        const auto product = [](std::uint32_t multiple, std::uint32_t scale) {
            const std::uint64_t result = static_cast<std::uint64_t>(multiple) * scale;
            return result > std::numeric_limits<std::uint32_t>::max() ? std::uint32_t{1u} << std::countr_zero(scale) : static_cast<std::uint32_t>(result);
        };
        std::uint32_t multiple = 1u;
        const auto op = value->Opcode();
        if (value->IsPhi()) {
            multiple = 0u;
            for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
                multiple = std::gcd(multiple, KnownMultiple(value->Argument(index), search, depth - 1u));
            }
        } else if ((op == IrOpcode::ShiftLeftLogical32 || op == IrOpcode::IMul32) && value->ArgumentCount() == 2u && immediateU32(value->Argument(1), immediate)) {
            IrValue* base = nullptr;
            const auto scale = ScaledBase(value, search.mask, base);
            multiple = product(KnownMultiple(base, search, depth - 1u), scale);
        } else if (op == IrOpcode::IAdd32 && value->ArgumentCount() == 2u) {
            IrValue* left = nullptr;
            IrValue* right = nullptr;
            const auto leftScale = ScaledBase(value->Argument(0), search.mask, left);
            const auto rightScale = ScaledBase(value->Argument(1), search.mask, right);
            const std::uint64_t sum = static_cast<std::uint64_t>(leftScale) + rightScale;
            if (EquivalentValue(m_program.Resources(), left, right) && sum <= std::numeric_limits<std::uint32_t>::max()) {
                multiple = product(KnownMultiple(left, search, depth - 1u), static_cast<std::uint32_t>(sum));
            } else {
                multiple = std::gcd(KnownMultiple(value->Argument(0), search, depth - 1u), KnownMultiple(value->Argument(1), search, depth - 1u));
            }
        } else if (Detail::IsRuntimeSelect(op) && value->ArgumentCount() == 3u) {
            multiple = std::gcd(KnownMultiple(value->Argument(1), search, depth - 1u), KnownMultiple(value->Argument(2), search, depth - 1u));
        } else if (op == IrOpcode::ReadFirstLane && value->ArgumentCount() == 2u && search.mask == nullptr) {
            search.mask = value->Argument(1)->Resolve();
            multiple = KnownMultiple(value->Argument(0), search, depth - 1u);
            search.mask = nullptr;
        }
        visiting.pop_back();
        if (!search.cycle) {
            search.known.emplace(value, multiple);
        }
        search.cycle = search.cycle || outerCycle;
        return multiple;
    }

    bool MatchKnownMultipleOffset(IrValue* value, IrValue*& key, std::uint32_t& stride) const {
        value = value->Resolve();
        if (value->HasImmediate() || value->Type() != IrType::U32) {
            return false;
        }
        if (value->Opcode() == IrOpcode::ReadFirstLane && value->ArgumentCount() != 2u) {
            return false;
        }
        MultipleSearch search;
        const auto multiple = KnownMultiple(value, search, firstLaneMultipleDepth);
        if (multiple < 4u || multiple % 4u != 0u) {
            return false;
        }
        key = value;
        stride = multiple;
        return true;
    }

    static void DiscardPendingPhis(std::vector<std::pair<IrBlock*, IrValue*>>& pendingPhis) {
        for (const auto& [block, phi] : pendingPhis) {
            for (std::size_t index = 0; index < phi->ArgumentCount(); index++) {
                phi->ReplaceArgument(index, nullptr);
            }
        }
        for (const auto& [block, phi] : pendingPhis) {
            phi->Invalidate();
        }
        pendingPhis.clear();
    }

    static const IrValue* UnwrapGuardNot(const IrValue* value, bool& expected) {
        value = value->Resolve();
        while (value->Opcode() == IrOpcode::LogicalNot && value->ArgumentCount() == 1u) {
            expected = !expected;
            value = value->Argument(0)->Resolve();
        }
        return value;
    }

    static bool GuardProvablyFalse(const IrValue* value, bool expected) {
        value = UnwrapGuardNot(value, expected);
        if (value->HasImmediate() && value->Type() == IrType::Bool) {
            return value->ImmediateBool() != expected;
        }
        if (expected && value->Opcode() == IrOpcode::LogicalAnd && value->ArgumentCount() == 2u) {
            return GuardProvablyFalse(value->Argument(0), true) || GuardProvablyFalse(value->Argument(1), true);
        }
        if (!expected && value->Opcode() == IrOpcode::LogicalOr && value->ArgumentCount() == 2u) {
            return GuardProvablyFalse(value->Argument(0), false) || GuardProvablyFalse(value->Argument(1), false);
        }
        return false;
    }

    struct GuardPredicate {
        const IrValue* phi = nullptr;
        bool isBool = true;
        bool expectedBool = false;
        std::uint32_t expectedU32 = 0u;
        bool negate = false;

        bool operator==(const GuardPredicate& other) const = default;
        bool operator<(const GuardPredicate& other) const {
            return std::tie(phi, isBool, expectedBool, expectedU32, negate) < std::tie(other.phi, other.isBool, other.expectedBool, other.expectedU32, other.negate);
        }
    };

    static bool GuardImmediateU32(const IrValue* value, std::uint32_t& result) {
        value = value->Resolve();
        if (!value->HasImmediate() || value->Type() != IrType::U32) {
            return false;
        }
        result = value->ImmediateU32();
        return true;
    }

    static bool ParseBranchGuard(const IrValue* condition, bool branchValue, GuardPredicate& out) {
        bool flipped = false;
        condition = UnwrapGuardNot(condition, flipped);
        const bool want = branchValue != flipped;
        if (condition->IsPhi() && condition->Type() == IrType::Bool && condition->Parent() != nullptr) {
            out = GuardPredicate{condition, true, want, 0u, false};
            return true;
        }
        if ((condition->Opcode() == IrOpcode::IEqual32 || condition->Opcode() == IrOpcode::INotEqual32) && condition->ArgumentCount() == 2u) {
            const IrValue* left = condition->Argument(0)->Resolve();
            const IrValue* right = condition->Argument(1)->Resolve();
            std::uint32_t imm = 0u;
            const IrValue* phi = nullptr;
            if (left->IsPhi() && left->Type() == IrType::U32 && left->Parent() != nullptr && GuardImmediateU32(right, imm)) {
                phi = left;
            } else if (right->IsPhi() && right->Type() == IrType::U32 && right->Parent() != nullptr && GuardImmediateU32(left, imm)) {
                phi = right;
            }
            if (phi != nullptr) {
                const bool isEqual = (condition->Opcode() == IrOpcode::IEqual32) == want;
                out = GuardPredicate{phi, false, false, imm, !isEqual};
                return true;
            }
        }
        return false;
    }

    static bool PredicateContradictsValue(const IrValue* value, const GuardPredicate& pred) {
        if (pred.isBool) {
            return GuardProvablyFalse(value, pred.expectedBool);
        }
        std::uint32_t imm = 0u;
        if (GuardImmediateU32(value, imm)) {
            return pred.negate ? (imm == pred.expectedU32) : (imm != pred.expectedU32);
        }
        return false;
    }

    static bool StepGuardPredicate(const IrValue* incoming, const GuardPredicate& pred, GuardPredicate& next) {
        if (pred.isBool) {
            bool edgeExpected = pred.expectedBool;
            const IrValue* unwrapped = UnwrapGuardNot(incoming, edgeExpected);
            if (unwrapped->IsPhi() && unwrapped->Type() == IrType::Bool && unwrapped->Parent() != nullptr) {
                next = GuardPredicate{unwrapped, true, edgeExpected, 0u, false};
                return true;
            }
            return false;
        }
        incoming = incoming->Resolve();
        if (incoming->IsPhi() && incoming->Type() == IrType::U32 && incoming->Parent() != nullptr) {
            next = pred;
            next.phi = incoming;
            return true;
        }
        return false;
    }

    struct BranchGuard {
        std::size_t branchIndex = 0;
        std::size_t edgeIndex = 0;
        GuardPredicate predicate;
    };

    bool TryMergePhiTableReads(IrValue& handle, std::uint32_t words, IrValue*& heapHandle, IrValue*& offset, std::uint32_t& first, bool& address, TableColumnPlan& plan, std::vector<std::pair<IrBlock*, IrValue*>>& pendingPhis) {
        std::unordered_map<const IrBlock*, std::size_t> order;
        const auto& blocks = m_program.BlockOrder();
        for (std::size_t index = 0; index < blocks.size(); index++) {
            order.emplace(blocks[index], index);
        }

        std::vector<IrBlock*> rpo;
        std::unordered_map<const IrBlock*, std::size_t> rpoIndex;
        std::vector<std::size_t> domEnter;
        std::vector<std::size_t> domLeave;
        if (!blocks.empty()) {
            std::unordered_set<const IrBlock*> visited;
            std::vector<std::pair<IrBlock*, std::size_t>> stack{{blocks.front(), 0u}};
            visited.insert(blocks.front());
            while (!stack.empty()) {
                auto& [current, next] = stack.back();
                if (next < current->Successors().size()) {
                    IrBlock* succ = current->Successors()[next++];
                    if (visited.insert(succ).second) stack.emplace_back(succ, 0u);
                    continue;
                }
                rpo.push_back(current);
                stack.pop_back();
            }
            std::reverse(rpo.begin(), rpo.end());
            for (std::size_t i = 0; i < rpo.size(); ++i) rpoIndex.emplace(rpo[i], i);
            constexpr std::size_t noDom = static_cast<std::size_t>(-1);
            std::vector<std::size_t> idom(rpo.size(), noDom);
            idom[0] = 0u;
            const auto intersect = [&](std::size_t left, std::size_t right) {
                while (left != right) {
                    while (left > right) left = idom[left];
                    while (right > left) right = idom[right];
                }
                return left;
            };
            for (bool changed = true; changed;) {
                changed = false;
                for (std::size_t pos = 1; pos < rpo.size(); ++pos) {
                    std::size_t candidate = noDom;
                    for (const IrBlock* pred : rpo[pos]->Predecessors()) {
                        const auto found = rpoIndex.find(pred);
                        if (found == rpoIndex.end() || idom[found->second] == noDom) continue;
                        candidate = candidate == noDom ? found->second : intersect(candidate, found->second);
                    }
                    if (candidate != idom[pos]) {
                        idom[pos] = candidate;
                        changed = true;
                    }
                }
            }
            std::vector<std::vector<std::size_t>> children(rpo.size());
            for (std::size_t pos = 1; pos < rpo.size(); ++pos) {
                if (idom[pos] != noDom) children[idom[pos]].push_back(pos);
            }
            domEnter.assign(rpo.size(), 0u);
            domLeave.assign(rpo.size(), 0u);
            std::size_t clock = 0;
            std::vector<std::pair<std::size_t, std::size_t>> dfs{{0u, 0u}};
            domEnter[0] = clock++;
            while (!dfs.empty()) {
                auto& [node, next] = dfs.back();
                if (next < children[node].size()) {
                    const std::size_t child = children[node][next++];
                    domEnter[child] = clock++;
                    dfs.emplace_back(child, 0u);
                    continue;
                }
                domLeave[node] = clock++;
                dfs.pop_back();
            }
        }
        const auto domIndex = [&](std::size_t a, std::size_t b) {
            return domEnter[a] <= domEnter[b] && domLeave[b] <= domLeave[a];
        };
        const auto domBlock = [&](const IrBlock* a, const IrBlock* b) {
            const auto fa = rpoIndex.find(a);
            const auto fb = rpoIndex.find(b);
            return fa != rpoIndex.end() && fb != rpoIndex.end() && domIndex(fa->second, fb->second);
        };

        std::vector<BranchGuard> branchGuards;
        const auto& info = m_program.Metadata().blockInfo;
        if (blocks.size() == info.size() && !rpo.empty()) {
            std::unordered_map<std::uint32_t, const IrBlock*> byId;
            byId.reserve(blocks.size());
            for (std::size_t index = 0; index < blocks.size(); index++) {
                byId.emplace(info[index].id, blocks[index]);
            }
            for (std::size_t index = 0; index < blocks.size(); index++) {
                const auto& terminator = info[index].terminator;
                if (terminator.kind != TerminatorKind::ConditionalBranch || terminator.loopHeader || info[index].condition == nullptr || terminator.trueBlock == terminator.falseBlock) {
                    continue;
                }
                const auto brPos = rpoIndex.find(blocks[index]);
                if (brPos == rpoIndex.end()) continue;
                for (const auto& [target, branchValue] : {std::pair{terminator.trueBlock, true}, std::pair{terminator.falseBlock, false}}) {
                    GuardPredicate pred;
                    if (!ParseBranchGuard(info[index].condition, branchValue, pred) || !domBlock(pred.phi->Parent(), blocks[index])) {
                        continue;
                    }
                    const auto found = byId.find(target);
                    if (found == byId.end() || found->second->Predecessors().size() != 1u || found->second->Predecessors().front() != blocks[index]) {
                        continue;
                    }
                    const auto edgePos = rpoIndex.find(found->second);
                    if (edgePos == rpoIndex.end()) continue;
                    branchGuards.push_back(BranchGuard{brPos->second, edgePos->second, pred});
                }
            }
        }

        std::vector<GuardPredicate> guardFacts;
        const auto addFact = [&](const GuardPredicate& pred) {
            if (guardFacts.size() >= 64u) return;
            if (std::ranges::find(guardFacts, pred) == guardFacts.end()) {
                guardFacts.push_back(pred);
            }
        };
        const auto addDirectFactsFor = [&](const IrBlock* block) {
            const auto pos = rpoIndex.find(block);
            if (pos == rpoIndex.end()) return;
            for (const auto& guard : branchGuards) {
                if (domIndex(guard.edgeIndex, pos->second)) {
                    addFact(guard.predicate);
                }
            }
        };
        if (handle.Parent() != nullptr) {
            addDirectFactsFor(handle.Parent());
            for (std::size_t index = 0; index < guardFacts.size(); index++) {
                const GuardPredicate fact = guardFacts[index];
                const IrBlock* condBlock = fact.phi->Parent();
                if (condBlock == nullptr || fact.phi->PhiBlockCount() != condBlock->Predecessors().size()) continue;
                const IrBlock* onlyPred = nullptr;
                std::size_t liveCount = 0;
                for (const IrBlock* pred : condBlock->Predecessors()) {
                    const IrValue* incoming = PhiIncoming(*fact.phi, pred);
                    if (incoming == nullptr || !PredicateContradictsValue(incoming, fact)) {
                        onlyPred = pred;
                        ++liveCount;
                    }
                }
                if (liveCount == 1u && onlyPred != nullptr) {
                    addDirectFactsFor(onlyPred);
                    if (const IrValue* incoming = PhiIncoming(*fact.phi, onlyPred)) {
                        GuardPredicate nextPred;
                        if (StepGuardPredicate(incoming, fact, nextPred) && domBlock(nextPred.phi->Parent(), onlyPred)) {
                            addFact(nextPred);
                        }
                    }
                }
            }
        }

        auto edgeContradictsFact = [&](auto& self, const IrBlock* pred, const IrBlock* block, const GuardPredicate& fact, std::uint32_t depth) -> bool {
            if (depth == 0u || fact.phi == nullptr) return false;
            const IrBlock* condBlock = fact.phi->Parent();
            if (condBlock == nullptr) return false;
            if (condBlock == block) {
                const IrValue* incoming = PhiIncoming(*fact.phi, pred);
                return incoming != nullptr && PredicateContradictsValue(incoming, fact);
            }
            const auto blockPos = rpoIndex.find(block);
            const auto condPos = rpoIndex.find(condBlock);
            if (blockPos == rpoIndex.end() || condPos == rpoIndex.end() || !domIndex(blockPos->second, condPos->second)) {
                return false;
            }
            if (fact.phi->PhiBlockCount() != condBlock->Predecessors().size()) return false;
            for (const IrBlock* gp : condBlock->Predecessors()) {
                const IrValue* inVal = PhiIncoming(*fact.phi, gp);
                if (inVal == nullptr) return false;
                if (PredicateContradictsValue(inVal, fact)) continue;
                GuardPredicate nextFact;
                if (StepGuardPredicate(inVal, fact, nextFact) && self(self, pred, block, nextFact, depth - 1u)) continue;
                bool ruledOut = false;
                const auto gpPos = rpoIndex.find(gp);
                if (gpPos != rpoIndex.end()) {
                    for (const auto& guard : branchGuards) {
                        if (domIndex(guard.edgeIndex, gpPos->second) && domIndex(blockPos->second, guard.branchIndex)) {
                            if (self(self, pred, block, guard.predicate, depth - 1u)) {
                                ruledOut = true;
                                break;
                            }
                        }
                    }
                }
                if (!ruledOut) return false;
            }
            return true;
        };

        struct StateKey {
            std::vector<IrValue*> tuple;
            GuardPredicate guard;
            bool useFacts = false;
            bool operator<(const StateKey& other) const {
                return std::tie(tuple, guard, useFacts) < std::tie(other.tuple, other.guard, other.useFacts);
            }
        };
        std::map<StateKey, IrValue*> memo;
        std::unordered_map<IrValue*, IrValue*> invariantCache;
        std::vector<const IrBlock*> visitingBlocks;
        bool initialized = false;
        constexpr std::size_t maxStates = 128u;

        auto resolve = [&](auto& self, std::vector<IrValue*> tuple, const GuardPredicate& activeGuard) -> IrValue* {
            for (IrValue*& value : tuple) {
                value = value->Resolve();
                if (!value->IsPhi()) continue;
                const auto [entry, inserted] = invariantCache.try_emplace(value, value);
                if (inserted) {
                    IrValue* invariant = ResolveInvariantPhi(m_program.Resources(), value);
                    entry->second = invariant == nullptr ? value : invariant->Resolve();
                }
                value = entry->second;
            }
            const bool hasPhi = std::ranges::any_of(tuple, [](const IrValue* value) { return value->IsPhi(); });
            if (!hasPhi) {
                IrValue* leafOffset = nullptr;
                for (std::uint32_t dword = 0; dword < words; dword++) {
                    IrValue* read = tuple[dword];
                    std::uint32_t memoryIndex = 0;
                    const MemoryInfo* memory = ScalarReadMemory(*read, memoryIndex);
                    const bool addressRead = memory == nullptr && (memory = ScalarAddressMemory(*read, memoryIndex)) != nullptr;
                    if (memory == nullptr || !MemoryIndexBelongsTo(memoryIndex, *read)) {
                        return nullptr;
                    }
                    IrValue* currentHandle = read->Argument(0)->Resolve();
                    IrValue* currentOffset = read->Argument(1)->Resolve();
                    if (!initialized) {
                        if (dword != 0u) return nullptr;
                        initialized = true;
                        first = memory->offset;
                        address = addressRead;
                        heapHandle = currentHandle;
                    } else if (addressRead != address || memory->offset != first + dword * static_cast<std::uint32_t>(sizeof(std::uint32_t)) || (currentHandle != heapHandle && !EquivalentValue(m_program.Resources(), heapHandle, currentHandle))) {
                        return nullptr;
                    }
                    if (dword == 0u) {
                        leafOffset = currentOffset;
                    } else if (currentOffset != leafOffset && !EquivalentValue(m_program.Resources(), leafOffset, currentOffset)) {
                        return nullptr;
                    }
                    if (std::ranges::find(plan.reads, read) == plan.reads.end()) {
                        plan.memory.push_back(memoryIndex);
                        plan.reads.push_back(read);
                    }
                }
                return leafOffset;
            }
            IrBlock* block = nullptr;
            std::size_t latest = 0;
            for (const IrValue* value : tuple) {
                if (!value->IsPhi()) continue;
                const auto found = order.find(value->Parent());
                if (found == order.end()) return nullptr;
                if (block == nullptr || found->second > latest) {
                    block = value->Parent();
                    latest = found->second;
                }
            }
            if (block == nullptr) return nullptr;
            const auto& predecessors = block->Predecessors();
            if (predecessors.size() < 2u) return nullptr;
            for (const IrValue* value : tuple) {
                if (value->IsPhi() && value->Parent() == block && value->PhiBlockCount() != predecessors.size()) {
                    return nullptr;
                }
            }
            const bool allowFacts = std::ranges::find(visitingBlocks, block) == visitingBlocks.end();
            GuardPredicate blockGuard;
            if (activeGuard.phi != nullptr && activeGuard.phi->Parent() == block && activeGuard.phi->PhiBlockCount() == predecessors.size()) {
                blockGuard = activeGuard;
            } else if (allowFacts) {
                for (const auto& fact : guardFacts) {
                    if (fact.phi->Parent() == block && fact.phi->PhiBlockCount() == predecessors.size()) {
                        blockGuard = fact;
                        break;
                    }
                }
            }
            const StateKey key{tuple, blockGuard.phi != nullptr ? blockGuard : activeGuard, allowFacts && !guardFacts.empty()};
            if (const auto known = memo.find(key); known != memo.end()) {
                return known->second;
            }
            if (memo.size() >= maxStates) {
                return nullptr;
            }
            IrValue& phiOffset = m_program.CreateValue(IrOpcode::Phi, IrType::U32);
            memo.emplace(key, &phiOffset);
            pendingPhis.emplace_back(block, &phiOffset);
            visitingBlocks.push_back(block);
            std::size_t liveEdges = 0;
            for (IrBlock* predecessor : predecessors) {
                GuardPredicate nextGuard = blockGuard.phi == nullptr ? activeGuard : GuardPredicate{};
                bool edgeDead = false;
                if (blockGuard.phi != nullptr) {
                    const IrValue* incomingGuard = PhiIncoming(*blockGuard.phi, predecessor);
                    if (incomingGuard != nullptr) {
                        if (PredicateContradictsValue(incomingGuard, blockGuard)) {
                            edgeDead = true;
                        } else {
                            GuardPredicate stepped;
                            if (StepGuardPredicate(incomingGuard, blockGuard, stepped)) {
                                nextGuard = stepped;
                            }
                        }
                    }
                }
                if (!edgeDead && allowFacts) {
                    for (const auto& fact : guardFacts) {
                        if (edgeContradictsFact(edgeContradictsFact, predecessor, block, fact, 6u)) {
                            edgeDead = true;
                            break;
                        }
                    }
                }
                if (edgeDead) {
                    phiOffset.AddPhiOperand(predecessor, &m_builder.Constant(0u));
                    continue;
                }
                ++liveEdges;
                std::vector<IrValue*> incoming;
                incoming.reserve(words);
                for (IrValue* value : tuple) {
                    IrValue* edge = value->IsPhi() && value->Parent() == block ? PhiIncoming(*value, predecessor) : value;
                    if (edge == nullptr) {
                        visitingBlocks.pop_back();
                        return nullptr;
                    }
                    incoming.push_back(edge);
                }
                IrValue* edgeOffset = self(self, std::move(incoming), nextGuard);
                if (edgeOffset == nullptr) {
                    visitingBlocks.pop_back();
                    return nullptr;
                }
                phiOffset.AddPhiOperand(predecessor, edgeOffset);
            }
            visitingBlocks.pop_back();
            if (liveEdges == 0u) {
                return nullptr;
            }
            return &phiOffset;
        };

        std::vector<IrValue*> root;
        root.reserve(words);
        for (std::uint32_t dword = 0; dword < words; dword++) {
            root.push_back(handle.Argument(dword)->Resolve());
        }
        offset = resolve(resolve, std::move(root), GuardPredicate{});
        if (offset == nullptr || !initialized) {
            DiscardPendingPhis(pendingPhis);
            plan.memory.clear();
            plan.reads.clear();
            return false;
        }
        return true;
    }

    static bool ReadsThroughDescriptor(const IrValue* value) {
        std::vector<const IrValue*> pending{value};
        std::vector<const IrValue*> seen;
        while (!pending.empty()) {
            const IrValue* current = pending.back()->Resolve();
            pending.pop_back();
            if (std::find(seen.begin(), seen.end(), current) != seen.end()) continue;
            seen.push_back(current);
            if (current->Opcode() == IrOpcode::ReadConstBuffer || current->Opcode() == IrOpcode::LoadAddressU32) return true;
            for (std::size_t index = 0; index < current->ArgumentCount(); index++) pending.push_back(current->Argument(index));
        }
        return false;
    }

    std::optional<KeyDomain> MatchKeyDomain(IrValue* key) {
        key = key->Resolve();
        while (key->Opcode() == IrOpcode::ReadFirstLane && key->ArgumentCount() >= 1u) key = key->Argument(0)->Resolve();
        DescriptorSource direct;
        direct.dwordCount = 1u;
        direct.dwords[0] = key;
        std::uint32_t badDword = 0;
        if (ValidateSource(direct, badDword) && !ReadsThroughDescriptor(key)) {
            KeyDomain domain{InternSource(direct), 0u, 4u};
            domain.value = true;
            return domain;
        }
        std::uint32_t memoryIndex = 0;
        const MemoryInfo* memory = ScalarReadMemory(*key, memoryIndex);
        if (memory == nullptr) {
            return std::nullopt;
        }
        DescriptorSource source;
        std::uint32_t sourceIndex = 0;
        if (!MakeRuntimeSource(*key->Argument(0)->Resolve(), IrOpcode::GetBufferResource, 4u, sourceIndex, source)) {
            return std::nullopt;
        }
        IrValue* selector = nullptr;
        std::uint32_t stride = 0;
        std::uint32_t addend = 0;
        if (MatchAffineOffset(key->Argument(1), selector, stride, addend)) {
            return KeyDomain{sourceIndex, memory->offset + addend, stride};
        }
        const auto bits = possibleU32Bits(key->Argument(1));
        const auto zeros = bits == 0u ? 31u : std::min<std::uint32_t>(static_cast<std::uint32_t>(std::countr_zero(bits)), 31u);
        return KeyDomain{sourceIndex, memory->offset, std::max(4u, 1u << zeros)};
    }

    static IrValue* SplitImmediateAddend(IrValue* value, std::uint32_t& addend) {
        value = value->Resolve();
        addend = 0;
        while (value->Opcode() == IrOpcode::IAdd32 && value->ArgumentCount() == 2u) {
            std::uint32_t immediate = 0;
            IrValue* rest = nullptr;
            if (immediateU32(value->Argument(0), immediate)) rest = value->Argument(1)->Resolve();
            else if (immediateU32(value->Argument(1), immediate)) rest = value->Argument(0)->Resolve();
            else break;
            addend += immediate;
            value = rest;
        }
        return value;
    }

    struct FirstLaneOffset {
        const IrValue* lane = nullptr;
        const IrValue* mask = nullptr;
        std::uint32_t shift = 0;
        std::uint32_t addend = 0;
    };

    bool ParseFirstLaneOffset(IrValue* value, const IrValue* mask, FirstLaneOffset& form, std::uint32_t depth) const {
        std::uint32_t addend = 0;
        value = SplitImmediateAddend(value, addend);
        while (mask != nullptr && Detail::IsRuntimeSelect(value->Opcode()) && value->ArgumentCount() == 3u && Detail::ActiveMaskImplies(m_program.Resources(), mask, value->Argument(0))) {
            value = SplitImmediateAddend(value->Argument(1), addend);
        }
        std::uint32_t shift = 0;
        if (depth != 0u && value->Opcode() == IrOpcode::ShiftLeftLogical32 && value->ArgumentCount() == 2u && immediateU32(value->Argument(1), shift) && shift < 32u) {
            if (!ParseFirstLaneOffset(value->Argument(0)->Resolve(), mask, form, depth - 1u) || form.shift + shift >= 32u) return false;
            form.shift += shift;
            form.addend <<= shift;
        } else if (depth != 0u && mask == nullptr && value->Opcode() == IrOpcode::ReadFirstLane && value->ArgumentCount() == 2u) {
            if (!ParseFirstLaneOffset(value->Argument(0)->Resolve(), value->Argument(1)->Resolve(), form, depth - 1u)) return false;
        } else {
            if (mask == nullptr) return false;
            form = FirstLaneOffset{value, mask, 0u, 0u};
        }
        form.addend += addend;
        return true;
    }

    bool SameFirstLaneOffset(const FirstLaneOffset& left, const FirstLaneOffset& right) const {
        return left.shift == right.shift && EquivalentValue(m_program.Resources(), left.mask, right.mask) && EquivalentValue(m_program.Resources(), left.lane, right.lane);
    }

    bool TryMakeTableColumn(IrValue& handle, bool sampler, bool r128, TableColumnPlan& plan, bool buffer = false) {
        const auto expected = buffer ? IrOpcode::GetBufferResource : sampler ? IrOpcode::GetSamplerResource : IrOpcode::GetImageResource;
        const std::uint32_t width = sampler || buffer ? 4u : 8u;
        if (handle.Opcode() != expected || handle.ArgumentCount() != width) {
            return false;
        }
        std::uint32_t words = width;
        if (!sampler && r128) {
            bool zeroTail = true;
            for (std::uint32_t dword = 4u; dword < 8u; dword++) {
                std::uint32_t immediate = 0;
                zeroTail = zeroTail && immediateU32(handle.Argument(dword), immediate) && immediate == 0u;
            }
            if (zeroTail) {
                words = 4u;
            }
        }
        IrValue* heapHandle = nullptr;
        IrValue* offset = nullptr;
        IrValue* base = nullptr;
        std::uint32_t baseAddend = 0;
        std::uint32_t carry = 0;
        bool folded = false;
        std::uint32_t first = 0;
        bool address = false;
        FirstLaneOffset baseLane;
        bool laneBase = false;
        std::vector<std::pair<IrBlock*, IrValue*>> pendingPhis;
        bool hasNonInvariantPhi = false;
        for (std::uint32_t dword = 0; dword < words; dword++) {
            if (ResolveInvariantPhi(m_program.Resources(), handle.Argument(dword)) == nullptr) {
                hasNonInvariantPhi = true;
                break;
            }
        }
        if (hasNonInvariantPhi) {
            if (buffer || !TryMergePhiTableReads(handle, words, heapHandle, offset, first, address, plan, pendingPhis)) {
                return false;
            }
        } else {
            for (std::uint32_t dword = 0; dword < words; dword++) {
                IrValue* read = ResolveInvariantPhi(m_program.Resources(), handle.Argument(dword));
                if (read == nullptr) return false;
                std::uint32_t memoryIndex = 0;
                const MemoryInfo* memory = ScalarReadMemory(*read, memoryIndex);
                const bool addressRead = memory == nullptr && (memory = ScalarAddressMemory(*read, memoryIndex)) != nullptr;
                if (memory == nullptr || !MemoryIndexBelongsTo(memoryIndex, *read)) {
                    return false;
                }
                std::uint32_t readAddend = 0;
                IrValue* readBase = SplitImmediateAddend(read->Argument(1), readAddend);
                bool sameBase = dword == 0u || EquivalentValue(m_program.Resources(), base, readBase);
                if (!sameBase && laneBase) {
                    FirstLaneOffset lane;
                    if (ParseFirstLaneOffset(read->Argument(1)->Resolve(), nullptr, lane, phiSearchDepth) && SameFirstLaneOffset(baseLane, lane)) {
                        readAddend = lane.addend;
                        sameBase = true;
                    }
                }
                if (dword == 0u) {
                    first = memory->offset;
                    address = addressRead;
                    base = readBase;
                    baseAddend = readAddend;
                    laneBase = ParseFirstLaneOffset(read->Argument(1)->Resolve(), nullptr, baseLane, phiSearchDepth) && baseLane.addend == baseAddend;
                } else if (addressRead != address || memory->offset + readAddend != first + baseAddend + dword * static_cast<std::uint32_t>(sizeof(std::uint32_t))) {
                    return false;
                } else if (readAddend != baseAddend) {
                    if (readAddend < baseAddend || static_cast<std::uint64_t>(memory->offset) + (readAddend - baseAddend) > 0x7fffffffu) {
                        return false;
                    }
                    folded = true;
                    carry = std::max(carry, readAddend - baseAddend);
                }
                IrValue* currentHandle = read->Argument(0)->Resolve();
                if (heapHandle != nullptr && currentHandle != heapHandle && !EquivalentValue(m_program.Resources(), heapHandle, currentHandle)) {
                    return false;
                }
                if (heapHandle == nullptr) heapHandle = currentHandle;
                if (dword == 0u) {
                    offset = read->Argument(1)->Resolve();
                } else if (!sameBase) {
                    return false;
                }
                plan.memory.push_back(memoryIndex);
                plan.reads.push_back(read);
            }
        }
        const auto failColumn = [&]() {
            DiscardPendingPhis(pendingPhis);
            return false;
        };
        if (address && pendingPhis.empty()) {
            DescriptorSource direct;
            MakeSource(handle, width, false, false, direct);
            std::uint32_t badDword = 0;
            if (ValidateSource(direct, badDword)) {
                return failColumn();
            }
        }
        IrValue* key = nullptr;
        std::uint32_t stride = 0;
        std::uint32_t addend = 0;
        bool masked = false;
        std::uint32_t descriptorOffset = 0;
        if (address && immediateU32(offset, descriptorOffset)) {
            if (!pendingPhis.empty()) return failColumn();
            return TryMakeIndirectColumn(handle, *heapHandle, sampler, words, descriptorOffset, first, plan);
        }
        if (!MatchAffineOffset(offset, key, stride, addend)) {
            if (!MatchMaskedOffset(offset, key, stride) && !MatchKnownMultipleOffset(offset, key, stride)) {
                // XBOX-FORK (assumption; disable with XBOX_NO_OPAQUE_HEAP_INDEX=1): the heap offset is an opaque wave-uniform
                // value (e.g. a validity test selecting between a loaded index and a fallback). The emitter indexes the table by
                // the actual byte offset, so only a record size is needed: use the descriptor size, assuming heap entries are
                // size-aligned (an unaligned offset falls outside the table at run time).
                static const bool noOpaque = std::getenv("XBOX_NO_OPAQUE_HEAP_INDEX") != nullptr;
                IrValue* lane = offset->Resolve();
                if (noOpaque || !address || lane->Opcode() != IrOpcode::ReadFirstLane || lane->Type() != IrType::U32) {
                    return failColumn();
                }
                key = lane;
                stride = words * static_cast<std::uint32_t>(sizeof(std::uint32_t));
                addend = 0;
            }
            masked = true;
        }
        DescriptorSource heapSource;
        std::uint32_t heapIndex = 0;
        if (!MakeRuntimeSource(*heapHandle, address ? IrOpcode::GetAddressResource : IrOpcode::GetBufferResource, address ? 2u : 4u, heapIndex, heapSource)) {
            if (!pendingPhis.empty()) return failColumn();
            if (sampler || address || buffer || heapHandle->Opcode() != IrOpcode::GetBufferResource || !std::ranges::all_of(heapHandle->Uses(), [&](const IrValue* use) { return std::ranges::find(plan.reads, use) != plan.reads.end(); })) return false;
            const auto sameHandle = [&](const IrValue* user) {
                if (user->Opcode() != handle.Opcode() || user->ArgumentCount() != handle.ArgumentCount()) return false;
                for (std::uint32_t argument = 0; argument < handle.ArgumentCount(); argument++) {
                    if (user->Argument(argument)->Resolve() != handle.Argument(argument)->Resolve()) return false;
                }
                return true;
            };
            if (!std::ranges::all_of(plan.reads, [&](const IrValue* read) { return std::ranges::all_of(read->Uses(), sameHandle); })) return false;
            TableColumnPlan parent;
            if (!TryMakeTableColumn(*heapHandle, false, false, parent, true)) return false;
            if (parent.parentPointer == nullptr) return false;
            if (!std::ranges::all_of(parent.reads, [&](const IrValue* read) {
                return std::ranges::all_of(read->Uses(), [&](const IrValue* user) {
                    return user->Opcode() == IrOpcode::GetBufferResource && std::ranges::all_of(user->Uses(), [&](const IrValue* use) { return std::ranges::find(plan.reads, use) != plan.reads.end(); });
                });
            })) return false;
            heapIndex = parent.source;
            heapSource = m_sources[heapIndex];
            plan.parentPointer = parent.parentPointer;
            m_tableColumns.push_back(std::move(parent));
        }
        for (const auto& [block, phi] : pendingPhis) {
            block->InsertInstructionBefore(nullptr, phi);
        }
        TableColumn column;
        column.heapSource = heapIndex;
        column.stride = stride;
        column.addend = addend;
        column.offset = first;
        column.dwordCount = words;
        column.maxKey = possibleU32UpperBound(key);
        column.sampler = sampler;
        column.address = address;
        column.buffer = buffer;
        column.foldedAddend = folded ? baseAddend : 0u;
        column.foldedCarry = folded ? carry : 0u;
        if (!address && !sampler && std::ranges::find(m_unselectedHandles, &handle) == m_unselectedHandles.end()) {
            DescriptorSource direct;
            MakeSource(handle, width, false, false, direct);
            DescriptorSource selection;
            selection.dwordCount = 1u;
            selection.dwords[0] = offset;
            std::uint32_t badDword = 0;
            if (ValidateSource(direct, badDword) && ValidateSource(selection, badDword)) {
                column.selected = true;
                column.selection = InternSource(selection);
            }
        }
        const auto domain = masked ? std::optional<KeyDomain>{} : MatchKeyDomain(key);
        DescriptorSource columnSource = heapSource;
        columnSource.tableColumn = column;
        plan.handle = &handle;
        plan.source = InternSource(columnSource);
        plan.keyDomain = domain;
        auto& shared = *m_sources[plan.source].tableColumn;
        shared.maxKey = std::max(shared.maxKey, column.maxKey);
        if (!domain.has_value()) {
            shared.unkeyed = true;
            shared.keyDomains.clear();
        } else if (!shared.unkeyed && std::find(shared.keyDomains.begin(), shared.keyDomains.end(), *domain) == shared.keyDomains.end()) {
            shared.keyDomains.push_back(*domain);
        }
        plan.offset = offset;
        return true;
    }

    IrValue* PointerRecordOrigin(IrValue* value) const {
        std::vector<IrValue*> pending{value};
        std::unordered_set<const IrValue*> seen;
        IrValue* origin = nullptr;
        while (!pending.empty()) {
            auto* current = pending.back()->Resolve();
            pending.pop_back();
            if (!seen.insert(current).second) continue;
            if (current->IsPhi()) {
                for (std::size_t index = 0; index < current->ArgumentCount(); ++index) pending.push_back(current->Argument(index));
                continue;
            }
            if (current->HasImmediate() && current->Type() == IrType::U32) continue;
            std::uint32_t memory = 0;
            if (ScalarReadMemory(*current, memory) == nullptr || !MemoryIndexBelongsTo(memory, *current)) return nullptr;
            if (origin != nullptr && !EquivalentValue(m_program.Resources(), origin, current)) return nullptr;
            origin = current;
        }
        return origin;
    }

    IrValue* UnwrapTableRead(IrValue* value) const {
        if (value == nullptr) return nullptr;
        value = value->Resolve();
        if (!value->IsPhi()) return value;
        IrValue* candidate = nullptr;
        std::vector<IrValue*> pending{value};
        std::unordered_set<const IrValue*> visited;
        while (!pending.empty()) {
            auto* current = pending.back()->Resolve();
            pending.pop_back();
            if (!visited.insert(current).second) continue;
            if (current->IsPhi()) {
                for (std::size_t i = 0; i < current->ArgumentCount(); i++) {
                    pending.push_back(current->Argument(i));
                }
                continue;
            }
            std::uint32_t memoryIndex = 0;
            IrValue* key = nullptr;
            std::uint32_t stride = 0;
            std::uint32_t addend = 0;
            if (ScalarReadMemory(*current, memoryIndex) == nullptr || !MemoryIndexBelongsTo(memoryIndex, *current) ||
                !MatchAffineOffset(current->Argument(1), key, stride, addend)) {
                return nullptr;
            }
            if (candidate == nullptr) {
                candidate = current;
            } else if (!EquivalentValue(m_program.Resources(), candidate, current)) {
                std::uint32_t candidateIndex = 0;
                const auto* candMem = ScalarReadMemory(*candidate, candidateIndex);
                const auto* curMem = ScalarReadMemory(*current, memoryIndex);
                if (candMem == nullptr || curMem == nullptr || candMem->offset != curMem->offset ||
                    !EquivalentValue(m_program.Resources(), candidate->Argument(0), current->Argument(0)) ||
                    !EquivalentValue(m_program.Resources(), candidate->Argument(1), current->Argument(1))) {
                    return nullptr;
                }
            }
        }
        return candidate;
    }

    bool BlockDominates(const IrBlock* dominator, const IrBlock* block) const {
        if (dominator == block) return true;
        std::vector<const IrBlock*> pending{block};
        std::unordered_set<const IrBlock*> visited{block};
        while (!pending.empty()) {
            const IrBlock* current = pending.back();
            pending.pop_back();
            if (current->Predecessors().empty()) return false;
            for (const IrBlock* predecessor : current->Predecessors()) {
                if (predecessor != dominator && visited.insert(predecessor).second) pending.push_back(predecessor);
            }
        }
        return true;
    }

    bool AvailableAt(IrValue* value, const IrBlock* block) const {
        value = value->Resolve();
        if (value->HasImmediate() || value->Parent() == nullptr) return true;
        return BlockDominates(value->Parent(), block);
    }

    bool TryMakeIndirectColumn(IrValue& handle, IrValue& descriptorPointer, bool sampler, std::uint32_t words, std::uint32_t descriptorOffset, std::uint32_t descriptorImmediate, TableColumnPlan& plan) {
        std::vector<PointerHop> hops;
        IrValue* pointer = &descriptorPointer;
        while (pointer->Opcode() == IrOpcode::GetAddressResource && pointer->ArgumentCount() == 2u) {
            std::uint32_t memoryIndex = 0;
            if (ScalarAddressMemory(*pointer->Argument(0)->Resolve(), memoryIndex) == nullptr) {
                break;
            }
            PointerHop hop;
            IrValue* next = nullptr;
            for (std::uint32_t dword = 0; dword < 2u; dword++) {
                IrValue* read = pointer->Argument(dword)->Resolve();
                const MemoryInfo* memory = ScalarAddressMemory(*read, memoryIndex);
                std::uint32_t readOffset = 0;
                if (memory == nullptr || !MemoryIndexBelongsTo(memoryIndex, *read) || !immediateU32(read->Argument(1), readOffset)) {
                    return false;
                }
                IrValue* readPointer = read->Argument(0)->Resolve();
                if (dword == 0u) {
                    hop = PointerHop{readOffset, memory->offset};
                    next = readPointer;
                } else if (readOffset != hop.offset || memory->offset != hop.immediate + static_cast<std::uint32_t>(sizeof(std::uint32_t)) || (readPointer != next && !EquivalentValue(m_program.Resources(), next, readPointer))) {
                    return false;
                }
                plan.hopMemory.push_back(memoryIndex);
                plan.hopReads.push_back(read);
            }
            hops.insert(hops.begin(), hop);
            pointer = next;
        }
        if (pointer->Opcode() != IrOpcode::GetAddressResource || pointer->ArgumentCount() != 2u) {
            return false;
        }
        IrValue* bufferHandle = nullptr;
        IrValue* offset = nullptr;
        const bool buffer = handle.Opcode() == IrOpcode::GetBufferResource;
        if (buffer && !hops.empty()) return false;
        const bool merged = buffer && pointer->Argument(0)->Resolve()->IsPhi();
        auto* low = merged ? PointerRecordOrigin(pointer->Argument(0)) : pointer->Argument(0)->Resolve();
        if (low == nullptr) return false;
        std::uint32_t first = 0;
        for (std::uint32_t dword = 0; dword < 2u; dword++) {
            IrValue* read = dword == 0u && merged ? low : UnwrapTableRead(pointer->Argument(dword));
            if (read == nullptr) {
                return false;
            }
            std::uint32_t memoryIndex = 0;
            const MemoryInfo* memory = ScalarReadMemory(*read, memoryIndex);
            if (memory == nullptr || !MemoryIndexBelongsTo(memoryIndex, *read)) {
                return false;
            }
            if (dword == 0u) {
                first = memory->offset;
                bufferHandle = read->Argument(0)->Resolve();
                offset = read->Argument(1)->Resolve();
            } else if (memory->offset != first + static_cast<std::uint32_t>(sizeof(std::uint32_t)) || read->Argument(0)->Resolve() != bufferHandle || !EquivalentValue(m_program.Resources(), offset, read->Argument(1))) {
                return false;
            }
        }
        if (!AvailableAt(offset, handle.Parent())) {
            return false;
        }
        IrValue* key = nullptr;
        std::uint32_t stride = 0;
        std::uint32_t addend = 0;
        if (!MatchAffineOffset(offset, key, stride, addend)) {
            return false;
        }
        DescriptorSource heapSource;
        std::uint32_t heapIndex = 0;
        if (!MakeRuntimeSource(*bufferHandle, IrOpcode::GetBufferResource, 4u, heapIndex, heapSource)) {
            return false;
        }
        TableColumn column;
        column.heapSource = heapIndex;
        column.stride = stride;
        column.addend = addend;
        column.offset = first;
        column.dwordCount = words;
        column.maxKey = merged ? 0xffffffffu : possibleU32UpperBound(key);
        column.sampler = sampler;
        column.indirect = true;
        column.buffer = buffer;
        column.indirectOffset = descriptorOffset;
        column.indirectImmediate = descriptorImmediate;
        column.pointerHops = std::move(hops);
        const auto domain = merged ? std::optional<KeyDomain>{} : MatchKeyDomain(key);
        DescriptorSource columnSource = heapSource;
        columnSource.tableColumn = column;
        plan.handle = &handle;
        plan.source = InternSource(columnSource);
        plan.keyDomain = domain;
        auto& shared = *m_sources[plan.source].tableColumn;
        shared.maxKey = std::max(shared.maxKey, column.maxKey);
        if (!domain.has_value()) {
            shared.unkeyed = true;
            shared.keyDomains.clear();
        } else if (!shared.unkeyed && std::find(shared.keyDomains.begin(), shared.keyDomains.end(), *domain) == shared.keyDomains.end()) {
            shared.keyDomains.push_back(*domain);
        }
        plan.offset = offset;
        if (buffer) plan.parentPointer = pointer;
        return true;
    }

    const TableColumnPlan* FindTableColumn(const IrValue& handle) const {
        const auto found = std::ranges::find_if(m_tableColumns, [&](const TableColumnPlan& plan) {
            return plan.handle == &handle;
        });
        return found == m_tableColumns.end() ? nullptr : &*found;
    }

    bool PlanTableColumn(IrValue& handle, bool sampler, bool r128) {
        if (FindTableColumn(handle) != nullptr) {
            return true;
        }
        TableColumnPlan plan;
        if (!TryMakeTableColumn(handle, sampler, r128, plan)) {
            return false;
        }
        m_tableColumns.push_back(std::move(plan));
        return true;
    }

    static constexpr std::uint32_t phiSearchDepth = 8u;
    static constexpr std::uint32_t firstLaneMultipleDepth = 32u;

    static bool SplittableImageRead(const IrValue& inst) {
        switch (inst.Opcode()) {
            case IrOpcode::ImageSampleRaw:
            case IrOpcode::ImageGatherRaw:
            case IrOpcode::ImageQueryLod:
            case IrOpcode::ImageQueryDimensions:
            case IrOpcode::ImageRead: return inst.Type() == IrType::U32x4;
            // XBOX-FORK: stores are split too; each arm's copy runs only for the lanes of its arm (EXEC is narrowed per arm).
            case IrOpcode::ImageWrite: return std::getenv("XBOX_NO_STORE_SPLIT") == nullptr && inst.Type() == IrType::Void && inst.ArgumentCount() == 4u && inst.Argument(3u)->Type() == IrType::Bool;
            default: return SplittableImageAtomic(inst);
        }
    }

    static bool SplittableImageAtomic(const IrValue& inst) {
        return std::getenv("XBOX_NO_ATOMIC_SPLIT") == nullptr && ImageOpcodeInfoOf(inst.Opcode()).access == ImageAccess::Atomic && inst.Type() == IrType::U32 &&
            inst.ArgumentCount() >= 4u && inst.Argument(inst.ArgumentCount() - 1u)->Type() == IrType::Bool;
    }

    static bool NarrowsArmExec(const IrValue& inst) {
        return inst.Type() == IrType::Void || SplittableImageAtomic(inst);
    }

    void CollectDescriptorPhis(IrValue* value, std::vector<IrValue*>& phis, std::uint32_t depth) const {
        value = value->Resolve();
        if (value->HasImmediate()) {
            return;
        }
        if (value->IsPhi()) {
            if (ResolveInvariantPhi(m_program.Resources(), value) == nullptr && std::ranges::find(phis, value) == phis.end()) {
                phis.push_back(value);
            }
            return;
        }
        if (depth != 0u && value->Opcode() == IrOpcode::ReadFirstLane && value->ArgumentCount() == 2u) {
            CollectDescriptorPhis(value->Argument(0), phis, depth - 1u);
            return;
        }
        // XBOX-FORK: a descriptor dword loaded through a pointer whose halves are Phis (a heap base merged by a join) is split too.
        const bool pointerLoad = value->Opcode() == IrOpcode::GetAddressResource && value->ArgumentCount() == 2u;
        const bool addressLoad = value->Opcode() == IrOpcode::LoadAddressU32 && value->ArgumentCount() >= 2u;
        if (depth != 0u && (pointerLoad || addressLoad)) {
            const std::size_t operands = pointerLoad ? 2u : 2u;
            for (std::size_t index = 0; index < operands; index++) {
                CollectDescriptorPhis(value->Argument(index), phis, depth - 1u);
            }
            return;
        }
        if (depth == 0u || !Detail::IsRuntimeUniformOp(value->Opcode())) {
            return;
        }
        for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
            CollectDescriptorPhis(value->Argument(index), phis, depth - 1u);
        }
    }

    static IrValue* PhiIncoming(const IrValue& phi, const IrBlock* predecessor) {
        for (std::size_t index = 0; index < phi.ArgumentCount(); index++) {
            if (phi.PhiBlock(index) == predecessor) {
                return phi.Argument(index);
            }
        }
        return nullptr;
    }

    IrValue* EmitBefore(IrValue& position, IrOpcode op, IrType type, std::span<IrValue* const> arguments, std::uint64_t flags = 0) {
        IrValue& value = m_program.CreateValue(op, type, flags);
        for (IrValue* argument : arguments) {
            value.AddArgument(argument);
        }
        position.Parent()->InsertInstructionBefore(&position, &value);
        return &value;
    }

    void EmitDescriptorFault(IrValue& inst, IrValue& condition, IrValue& low, IrValue& high) {
        IrValue* exec = inst.Argument(inst.ArgumentCount() - 1u);
        if (exec->Type() != IrType::Bool) {
            fail("a split image instruction has no EXEC operand");
        }
        const std::array<IrValue*, 2> active {&condition, exec};
        IrValue* fault = EmitBefore(inst, IrOpcode::LogicalAnd, IrType::Bool, active);
        const std::array<IrValue*, 4> arguments {fault, &low, &high, &m_builder.Constant(inst.Flags<MemoryFlags>().pc)};
        static_cast<void>(EmitBefore(inst, IrOpcode::DescriptorFault, IrType::Void, arguments));
        m_info.usesFaultBuffer = true;
    }

    IrValue& UndefinedImageWords(IrValue& inst, const std::array<IrValue*, 3>& words) {
        const auto emit = [&](IrOpcode op, IrType type, std::initializer_list<IrValue*> arguments) {
            return EmitBefore(inst, op, type, std::vector<IrValue*>(arguments));
        };
        const auto constant = [&](std::uint32_t value) { return &m_builder.Constant(value); };
        IrValue* address = emit(IrOpcode::LogicalOr, IrType::Bool, {emit(IrOpcode::INotEqual32, IrType::Bool, {words[0], constant(0u)}), emit(IrOpcode::INotEqual32, IrType::Bool, {emit(IrOpcode::BitwiseAnd32, IrType::U32, {words[1], constant(0xffu)}), constant(0u)})});
        IrValue* type = emit(IrOpcode::ShiftRightLogical32, IrType::U32, {words[2], constant(28u)});
        IrValue* format = emit(IrOpcode::BitwiseAnd32, IrType::U32, {emit(IrOpcode::ShiftRightLogical32, IrType::U32, {words[1], constant(20u)}), constant(0x1ffu)});
        IrValue* defined = nullptr;
        for (const auto& range : DefinedImageFormats) {
            IrValue* inside = emit(IrOpcode::LogicalAnd, IrType::Bool, {emit(IrOpcode::UGreaterThanEqual32, IrType::Bool, {format, constant(range.first)}), emit(IrOpcode::ULessThan32, IrType::Bool, {format, constant(range.last + 1u)})});
            defined = defined == nullptr ? inside : emit(IrOpcode::LogicalOr, IrType::Bool, {defined, inside});
        }
        IrValue* notImage = emit(IrOpcode::ULessThan32, IrType::Bool, {type, constant(static_cast<std::uint32_t>(ImageType::Color1D))});
        IrValue* undefined = emit(IrOpcode::LogicalOr, IrType::Bool, {notImage, emit(IrOpcode::LogicalNot, IrType::Bool, {defined})});
        IrValue* typed = emit(IrOpcode::UGreaterThanEqual32, IrType::Bool, {type, constant(2u)});
        return *emit(IrOpcode::LogicalAnd, IrType::Bool, {emit(IrOpcode::LogicalAnd, IrType::Bool, {address, typed}), undefined});
    }

    IrValue* EmitCopy(const IrValue& original, std::span<IrValue* const> arguments, IrValue& position) {
        if (Detail::IsRawRead(m_program.Resources(), original)) {
            auto flags = original.Flags<MemoryFlags>();
            auto memory = m_program.Resources().memoryInfo[flags.index];
            memory.planningOnly = true;
            flags.index = static_cast<std::uint32_t>(m_program.Resources().memoryInfo.size());
            m_program.Resources().memoryInfo.push_back(memory);
            std::uint64_t rawFlags = 0;
            std::memcpy(&rawFlags, &flags, sizeof(flags));
            IrValue* copy = EmitBefore(position, original.Opcode(), original.Type(), arguments, rawFlags);
            const auto origin = m_readOrigins.find(&original);
            m_readOrigins.emplace(copy, origin != m_readOrigins.end() ? origin->second : original.Parent());
            return copy;
        }
        return EmitBefore(position, original.Opcode(), original.Type(), arguments, original.Flags<std::uint64_t>());
    }

    bool Dominates(const IrBlock* definition, const IrBlock* use) const {
        const auto& order = m_program.BlockOrder();
        if (definition == nullptr || use == nullptr || order.empty() ||
            std::ranges::find(order, definition) == order.end() || std::ranges::find(order, use) == order.end()) return false;
        if (definition == use) return true;
        std::vector<const IrBlock*> pending{order.front()};
        std::unordered_set<const IrBlock*> visited;
        while (!pending.empty()) {
            const IrBlock* block = pending.back();
            pending.pop_back();
            if (!visited.insert(block).second || block == definition) continue;
            if (block == use) return false;
            for (const IrBlock* successor : block->Successors()) pending.push_back(successor);
        }
        return true;
    }

    bool AvailableBefore(const IrValue& definition, const IrValue& position) const {
        if (definition.Parent() == nullptr || position.Parent() == nullptr || &definition == &position) return false;
        if (definition.Parent() != position.Parent()) return Dominates(definition.Parent(), position.Parent());
        for (const IrValue* instruction : definition.Parent()->Instructions()) {
            if (instruction == &definition) return true;
            if (instruction == &position) return false;
        }
        return false;
    }

    bool RegisterOnlyOffset(IrValue* value, std::uint32_t depth) const {
        value = value->Resolve();
        if (value->HasImmediate()) return true;
        if (value->IsPhi()) {
            IrValue* invariant = ResolveInvariantPhi(m_program.Resources(), value);
            return invariant != nullptr && !invariant->IsPhi() && RegisterOnlyOffset(invariant, depth);
        }
        if (value->Opcode() == IrOpcode::GetUserData) return value->ArgumentCount() == 1u;
        if (depth == 0u || !Detail::IsRuntimeUniformOp(value->Opcode())) return false;
        for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
            if (!RegisterOnlyOffset(value->Argument(index), depth - 1u)) return false;
        }
        return true;
    }

    IrValue* Rematerialize(IrValue* value, IrValue& position, bool emit, std::uint32_t depth, bool pointerOperand = false) {
        value = value->Resolve();
        if (value->HasImmediate()) {
            return value;
        }
        if (value->IsPhi()) {
            IrValue* invariant = ResolveInvariantPhi(m_program.Resources(), value);
            if (invariant == nullptr) return pointerOperand && AvailableBefore(*value, position) ? value : nullptr;
            return invariant->IsPhi() ? nullptr : Rematerialize(invariant, position, emit, depth, pointerOperand);
        }
        if (pointerOperand && Detail::IsRawRead(m_program.Resources(), *value) && AvailableBefore(*value, position)) return value;
        if (depth == 0u) {
            return nullptr;
        }
        const auto op = value->Opcode();
        const bool register_ = op == IrOpcode::GetUserData;
        const bool argumentless = (op == IrOpcode::GetShaderBase || op == IrOpcode::GetSrtResource) && value->ArgumentCount() == 0u;
        const bool srtRead = op == IrOpcode::ReadConst && value->ArgumentCount() == 2u && value->Argument(0)->Resolve()->Opcode() == IrOpcode::GetSrtResource && value->Argument(1)->Resolve()->HasImmediate();
        std::uint32_t memoryIndex = 0u;
        const bool rawRead = op == IrOpcode::LoadAddressU32 && ScalarAddressMemory(*value, memoryIndex) != nullptr &&
            RegisterOnlyOffset(value->Argument(1), phiSearchDepth);
        const bool sourceHandle = op == IrOpcode::GetAddressResource && value->ArgumentCount() == 2u;
        if (!register_ && !argumentless && !srtRead && !rawRead && !sourceHandle && !Detail::IsRuntimeUniformOp(op)) {
            return nullptr;
        }
        std::vector<IrValue*> arguments;
        for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
            IrValue* argument = register_ ? value->Argument(index) : Rematerialize(value->Argument(index), position, emit, depth - 1u, pointerOperand || (rawRead && index == 0u));
            if (argument == nullptr) {
                return nullptr;
            }
            arguments.push_back(argument);
        }
        return emit ? EmitCopy(*value, arguments, position) : value;
    }

    IrValue* SubstituteEdge(IrValue* value, const IrBlock* predecessor, IrValue& position, bool emit, std::uint32_t depth) {
        value = value->Resolve();
        std::vector<IrValue*> phis;
        CollectDescriptorPhis(value, phis, depth);
        if (phis.empty()) {
            return value;
        }
        if (value->IsPhi()) {
            IrValue* incoming = PhiIncoming(*value, predecessor);
            return incoming == nullptr ? nullptr : Rematerialize(incoming, position, emit, phiSearchDepth);
        }
        std::vector<IrValue*> arguments;
        for (std::size_t index = 0; index < value->ArgumentCount(); index++) {
            IrValue* argument = SubstituteEdge(value->Argument(index), predecessor, position, emit, depth - 1u);
            if (argument == nullptr) {
                return nullptr;
            }
            arguments.push_back(argument);
        }
        return emit ? EmitCopy(*value, arguments, position) : value;
    }

    IrValue& EdgeSelector(IrBlock& block, const std::vector<bool>& edges) {
        for (const auto& selector : m_edgeSelectors) {
            if (selector.block == &block && selector.edges == edges) {
                return *selector.value;
            }
        }
        IrValue& phi = m_program.CreateValue(IrOpcode::Phi, IrType::Bool);
        const auto& predecessors = block.Predecessors();
        for (std::size_t index = 0; index < predecessors.size(); index++) {
            phi.AddPhiOperand(predecessors[index], &m_builder.ConstantBool(edges[index]));
        }
        block.InsertInstructionBefore(nullptr, &phi);
        m_edgeSelectors.push_back({&block, edges, &phi});
        return phi;
    }

    void SplitDescriptorPhi(IrValue& inst) {
        const auto info = ImageOpcodeInfoOf(inst.Opcode());
        const std::uint32_t handleCount = info.needsSampler ? 2u : 1u;
        if (inst.ArgumentCount() < handleCount || inst.Parent() == nullptr) {
            return;
        }
        std::array<IrValue*, 2> handles {};
        std::array<bool, 2> split {};
        std::array<std::vector<IrValue*>, 2> slotPhis {};
        std::vector<IrValue*> phis;
        for (std::uint32_t slot = 0; slot < handleCount; slot++) {
            IrValue* handle = inst.Argument(slot)->Resolve();
            const auto expected = slot == 0u ? IrOpcode::GetImageResource : IrOpcode::GetSamplerResource;
            if (handle->Opcode() != expected || handle->ArgumentCount() != (slot == 0u ? 8u : 4u)) {
                return;
            }
            handles[slot] = handle;
            for (std::size_t dword = 0; dword < handle->ArgumentCount(); dword++) {
                CollectDescriptorPhis(handle->Argument(dword), slotPhis[slot], phiSearchDepth);
            }
            split[slot] = !slotPhis[slot].empty();
            for (IrValue* phi : slotPhis[slot]) {
                if (std::ranges::find(phis, phi) == phis.end()) {
                    phis.push_back(phi);
                }
            }
        }
        if (phis.empty()) {
            return;
        }
        const bool both = split[0] && split[1];
        if (SplitDescriptorEdges(inst, handles, split, handleCount, phis) || SplitDescriptorWeb(inst, handles, split, handleCount, !both)) {
            return;
        }
        if (both) {
            const std::array<bool, 2> samplerOnly {false, true};
            if (SplitDescriptorEdges(inst, handles, samplerOnly, handleCount, slotPhis[1]) || SplitDescriptorWeb(inst, handles, samplerOnly, handleCount, false)) {
                return;
            }
            const std::array<bool, 2> imageOnly {true, false};
            if (SplitDescriptorEdges(inst, handles, imageOnly, handleCount, slotPhis[0]) || SplitDescriptorWeb(inst, handles, imageOnly, handleCount, false)) {
                return;
            }
            static_cast<void>(SplitDescriptorWeb(inst, handles, split, handleCount, true));
        }
    }

    bool SplitDescriptorEdges(IrValue& inst, const std::array<IrValue*, 2>& handles, const std::array<bool, 2>& split, std::uint32_t handleCount, const std::vector<IrValue*>& phis) {
        IrBlock* block = phis.front()->Parent();
        if (block == nullptr) {
            return false;
        }
        const auto& predecessors = block->Predecessors();
        if (predecessors.size() < 2u) {
            return false;
        }
        for (const IrValue* phi : phis) {
            if (phi->Parent() != block || phi->PhiBlockCount() != predecessors.size()) {
                return false;
            }
            for (const IrBlock* predecessor : predecessors) {
                if (PhiIncoming(*phi, predecessor) == nullptr) {
                    return false;
                }
            }
        }

        std::vector<std::uint32_t> edgeArm(predecessors.size());
        std::vector<const IrBlock*> arms;
        for (std::size_t edge = 0; edge < predecessors.size(); edge++) {
            std::uint32_t arm = 0;
            for (; arm < arms.size(); arm++) {
                const bool same = std::ranges::all_of(phis, [&](const IrValue* phi) {
                    return EquivalentValue(m_program.Resources(), PhiIncoming(*phi, predecessors[edge]), PhiIncoming(*phi, arms[arm]));
                });
                if (same) {
                    break;
                }
            }
            if (arm == arms.size()) {
                arms.push_back(predecessors[edge]);
            }
            edgeArm[edge] = arm;
        }
        for (const IrBlock* arm : arms) {
            for (std::uint32_t slot = 0; slot < handleCount; slot++) {
                for (std::size_t dword = 0; split[slot] && dword < handles[slot]->ArgumentCount(); dword++) {
                    if (SubstituteEdge(handles[slot]->Argument(dword), arm, inst, false, phiSearchDepth) == nullptr) {
                        return false;
                    }
                }
            }
        }

        std::vector<IrValue*> conditions;
        for (std::uint32_t arm = 0; arm + 1u < arms.size(); arm++) {
            std::vector<bool> edges(edgeArm.size());
            for (std::size_t edge = 0; edge < edgeArm.size(); edge++) {
                edges[edge] = edgeArm[edge] == arm;
            }
            conditions.push_back(&EdgeSelector(*block, edges));
        }
        ReplaceWithArmCopies(inst, handles, split, handleCount, conditions, false, [&](std::uint32_t arm, std::uint32_t slot, std::size_t dword) {
            return SubstituteEdge(handles[slot]->Argument(dword), arms[arm], inst, true, phiSearchDepth);
        });
        return true;
    }

    template <typename Dword>
    void ReplaceWithArmCopies(IrValue& inst, const std::array<IrValue*, 2>& handles, const std::array<bool, 2>& split, std::uint32_t handleCount, std::span<IrValue* const> conditions, bool optionalArms, Dword&& dword) {
        std::vector<IrValue*> copies;
        const auto flags = inst.Flags<MemoryFlags>();
        const bool optional = optionalArms && !conditions.empty() && split[0];
        if (optional) {
            m_program.Resources().memoryInfo.at(flags.index).descriptorWebArm = true;
        }
        std::vector<std::array<IrValue*, 3>> armWords;
        const auto memory = m_program.Resources().memoryInfo.at(flags.index);
        for (std::uint32_t arm = 0; arm <= conditions.size(); arm++) {
            std::vector<IrValue*> arguments(inst.Arguments().begin(), inst.Arguments().end());
            for (std::uint32_t slot = 0; slot < handleCount; slot++) {
                if (!split[slot]) {
                    continue;
                }
                std::vector<IrValue*> dwords;
                for (std::size_t index = 0; index < handles[slot]->ArgumentCount(); index++) {
                    dwords.push_back(dword(arm, slot, index));
                }
                if (slot == 0u && optional) {
                    armWords.push_back({dwords[0], dwords[1], dwords[3]});
                }
                arguments[slot] = EmitCopy(*handles[slot], dwords, inst);
            }
            if (NarrowsArmExec(inst)) {
                IrValue* active = inst.Argument(inst.ArgumentCount() - 1u);
                for (std::uint32_t before = 0; before < arm && before < conditions.size(); ++before) {
                    const std::array<IrValue*, 1> negate {conditions[before]};
                    IrValue* other = EmitBefore(inst, IrOpcode::LogicalNot, IrType::Bool, negate);
                    const std::array<IrValue*, 2> both {active, other};
                    active = EmitBefore(inst, IrOpcode::LogicalAnd, IrType::Bool, both);
                }
                if (arm < conditions.size()) {
                    const std::array<IrValue*, 2> both {active, conditions[arm]};
                    active = EmitBefore(inst, IrOpcode::LogicalAnd, IrType::Bool, both);
                }
                arguments.back() = active;
            }
            auto copyFlags = flags;
            if (arm != 0u) {
                copyFlags.index = static_cast<std::uint32_t>(m_program.Resources().memoryInfo.size());
                m_program.Resources().memoryInfo.push_back(memory);
            }
            std::uint64_t rawFlags = 0;
            std::memcpy(&rawFlags, &copyFlags, sizeof(copyFlags));
            copies.push_back(EmitBefore(inst, inst.Opcode(), inst.Type(), arguments, rawFlags));
        }

        if (optional) {
            std::array<IrValue*, 3> selected = armWords.back();
            for (std::uint32_t arm = static_cast<std::uint32_t>(armWords.size()) - 1u; arm-- > 0u;) {
                for (std::size_t word = 0; word < selected.size(); word++) {
                    const std::array<IrValue*, 3> arguments {conditions[arm], armWords[arm][word], selected[word]};
                    selected[word] = EmitBefore(inst, IrOpcode::SelectU32, IrType::U32, arguments);
                }
            }
            EmitDescriptorFault(inst, UndefinedImageWords(inst, selected), *selected[0], *selected[1]);
        }

        if (inst.Type() == IrType::Void) {
            inst.Invalidate();
            inst.Parent()->RemoveInstruction(&inst);
            return;
        }
        IrValue* result = copies.back();
        if (copies.size() > 1u && inst.Type() == IrType::U32) {
            for (std::uint32_t arm = static_cast<std::uint32_t>(copies.size()) - 1u; arm-- > 0u;) {
                const std::array<IrValue*, 3> arguments {conditions[arm], copies[arm], result};
                result = EmitBefore(inst, IrOpcode::SelectU32, IrType::U32, arguments);
            }
        } else if (copies.size() > 1u) {
            std::array<IrValue*, 4> components {};
            for (std::uint32_t component = 0; component < components.size(); component++) {
                const auto extract = [&](IrValue* vector) {
                    const std::array<IrValue*, 2> arguments {vector, &m_builder.Constant(component)};
                    return EmitBefore(inst, IrOpcode::CompositeExtractU32x4, IrType::U32, arguments);
                };
                IrValue* selected = extract(copies.back());
                for (std::uint32_t arm = static_cast<std::uint32_t>(copies.size()) - 1u; arm-- > 0u;) {
                    const std::array<IrValue*, 3> arguments {conditions[arm], extract(copies[arm]), selected};
                    selected = EmitBefore(inst, IrOpcode::SelectU32, IrType::U32, arguments);
                }
                components[component] = selected;
            }
            result = EmitBefore(inst, IrOpcode::CompositeConstructU32x4, IrType::U32x4, components);
        }
        inst.ReplaceAllUsesWith(result);
        inst.Invalidate();
        inst.Parent()->RemoveInstruction(&inst);
    }

    struct DescriptorWeb {
        static constexpr std::size_t maxStates = 512u;
        static constexpr std::size_t maxArms = 64u;
        static constexpr std::uint32_t foldedArm = 0xffffffffu;
        std::string failure;
        bool foldedValue = false;
        std::vector<std::vector<IrValue*>> arms;
        std::map<std::pair<std::vector<IrValue*>, const IrValue*>, IrValue*> selectors;
        std::vector<std::pair<IrBlock*, IrValue*>> phis;
        std::vector<std::tuple<IrBlock*, IrValue*, IrValue*>> lanes;
        std::unordered_map<IrValue*, IrValue*> resolved;
        std::unordered_map<const IrBlock*, std::size_t> order;
    };

    static bool StrictDescriptorWebs() {
        static const bool strict = [] {
            const char* value = std::getenv("APS5_STRICT_DESCRIPTOR_WEBS");
            return value != nullptr && std::strcmp(value, "0") != 0;
        }();
        return strict;
    }

    static IrValue* GiveUp(DescriptorWeb& web, std::string reason) {
        if (web.failure.empty()) {
            web.failure = std::move(reason);
        }
        return nullptr;
    }

    std::string RebuildBlocker(IrValue* value, std::uint32_t depth) const {
        value = value->Resolve();
        if (value->HasImmediate()) {
            return {};
        }
        if (value->IsPhi()) {
            IrValue* invariant = ResolveInvariantPhi(m_program.Resources(), value);
            if (invariant == nullptr || invariant->IsPhi()) {
                return "a Phi that does not resolve to one value: " + describeValueChain(value, 3u);
            }
            return RebuildBlocker(invariant, depth);
        }
        if (depth == 0u) {
            return "a chain deeper than " + std::to_string(phiSearchDepth) + " operations";
        }
        const auto op = value->Opcode();
        const bool register_ = op == IrOpcode::GetUserData;
        const bool argumentless = (op == IrOpcode::GetShaderBase || op == IrOpcode::GetSrtResource) && value->ArgumentCount() == 0u;
        const bool srtRead = op == IrOpcode::ReadConst && value->ArgumentCount() == 2u && value->Argument(0)->Resolve()->Opcode() == IrOpcode::GetSrtResource && value->Argument(1)->Resolve()->HasImmediate();
        if (!register_ && !argumentless && !srtRead && !Detail::IsRuntimeUniformOp(op)) {
            return describeValueChain(value, 3u);
        }
        for (std::size_t index = 0; !register_ && index < value->ArgumentCount(); index++) {
            auto blocker = RebuildBlocker(value->Argument(index), depth - 1u);
            if (!blocker.empty()) {
                return blocker;
            }
        }
        return {};
    }

    static std::size_t InstructionPosition(const IrBlock* block, const IrValue* value) {
        if (block == nullptr) return 0u;
        std::size_t pos = 0u;
        for (const IrValue* inst : block->Instructions()) {
            if (inst == value) return pos;
            ++pos;
        }
        return pos;
    }

    IrValue* SimplifyUnderActiveMask(IrValue* value, const IrValue* activeMask) const {
        value = value->Resolve();
        while (activeMask != nullptr && value->Opcode() == IrOpcode::SelectU32 && value->ArgumentCount() == 3u) {
            if (Detail::ActiveMaskImplies(m_program.Resources(), activeMask, value->Argument(0))) {
                value = value->Argument(1)->Resolve();
            } else if (Detail::ActiveMaskDisjoint(m_program.Resources(), activeMask, value->Argument(0))) {
                value = value->Argument(2)->Resolve();
            } else {
                break;
            }
        }
        return value;
    }

    IrValue* WebSelector(std::vector<IrValue*> tuple, DescriptorWeb& web, IrValue& position, const IrValue* activeMask = nullptr) {
        for (IrValue*& value : tuple) {
            value = SimplifyUnderActiveMask(value, activeMask);
            if (!value->IsPhi()) {
                continue;
            }
            const auto [entry, inserted] = web.resolved.try_emplace(value, value);
            if (inserted) {
                IrValue* invariant = ResolveInvariantPhi(m_program.Resources(), value);
                entry->second = invariant == nullptr ? value : invariant->Resolve();
            }
            value = SimplifyUnderActiveMask(entry->second, activeMask);
        }
        const auto key = std::make_pair(tuple, activeMask);
        if (const auto known = web.selectors.find(key); known != web.selectors.end()) {
            return known->second;
        }
        if (!StrictDescriptorWebs()) {
            for (IrValue* value : tuple) {
                if (value->IsPhi() || value->Opcode() == IrOpcode::SelectU32 || value->Opcode() == IrOpcode::ReadFirstLane) {
                    continue;
                }
                if (Detail::HoldsFoldableValue(value, phiSearchDepth) || Rematerialize(value, position, false, phiSearchDepth) == nullptr) {
                    web.foldedValue = true;
                    return &m_builder.Constant(DescriptorWeb::foldedArm);
                }
            }
        }
        if (web.order.empty()) {
            const auto& order = m_program.BlockOrder();
            for (std::size_t index = 0; index < order.size(); index++) {
                web.order.emplace(order[index], index);
            }
        }
        IrBlock* block = nullptr;
        std::size_t latest = 0;
        for (const IrValue* value : tuple) {
            if (!value->IsPhi()) {
                continue;
            }
            const auto found = web.order.find(value->Parent());
            if (found == web.order.end()) {
                return GiveUp(web, "a descriptor Phi is in a block outside the program order");
            }
            if (block == nullptr || found->second > latest) {
                block = value->Parent();
                latest = found->second;
            }
        }
        IrValue* lane = nullptr;
        std::size_t laneOrder = 0;
        std::size_t lanePos = 0;
        for (IrValue* value : tuple) {
            if (value->Opcode() != IrOpcode::SelectU32 && value->Opcode() != IrOpcode::ReadFirstLane) {
                continue;
            }
            const auto found = web.order.find(value->Parent());
            const std::size_t index = found == web.order.end() ? 0u : found->second;
            const std::size_t pos = InstructionPosition(value->Parent(), value);
            if (lane == nullptr || index > laneOrder || (index == laneOrder && pos > lanePos)) {
                lane = value;
                laneOrder = index;
                lanePos = pos;
            }
        }
        if (lane != nullptr && (block == nullptr || laneOrder >= latest)) {
            return LaneSelector(tuple, *lane, web, position, activeMask);
        }
        if (block == nullptr) {
            if (std::ranges::any_of(tuple, [](IrValue* value) { return Detail::HoldsFoldableValue(value, phiSearchDepth); })) {
                if (StrictDescriptorWebs()) {
                    return GiveUp(web, "a descriptor dword depends on the program counter, EXEC, a lane mask, the lane index or texels");
                }
                web.foldedValue = true;
                return &m_builder.Constant(DescriptorWeb::foldedArm);
            }
            for (IrValue* value : tuple) {
                if (Rematerialize(value, position, false, phiSearchDepth) == nullptr) {
                    if (StrictDescriptorWebs()) {
                        return GiveUp(web, "a descriptor dword is not rebuilt from user data, the SRT and uniform operations before the read (" + RebuildBlocker(value, phiSearchDepth) + ")");
                    }
                    web.foldedValue = true;
                    return &m_builder.Constant(DescriptorWeb::foldedArm);
                }
            }
            const auto same = std::ranges::find_if(web.arms, [&](const std::vector<IrValue*>& arm) {
                return std::ranges::equal(tuple, arm, [&](IrValue* left, IrValue* right) { return EquivalentValue(m_program.Resources(), left, right); });
            });
            if (same != web.arms.end()) {
                return &m_builder.Constant(static_cast<std::uint32_t>(same - web.arms.begin()));
            }
            if (web.arms.size() == DescriptorWeb::maxArms) {
                return GiveUp(web, "more than 64 distinct descriptors");
            }
            web.arms.push_back(std::move(tuple));
            return &m_builder.Constant(static_cast<std::uint32_t>(web.arms.size() - 1u));
        }
        if (web.selectors.size() == DescriptorWeb::maxStates) {
            return GiveUp(web, "more than 512 descriptor states");
        }
        const auto& predecessors = block->Predecessors();
        if (predecessors.size() < 2u) {
            return GiveUp(web, "a descriptor Phi's block has fewer than two predecessors");
        }
        for (const IrValue* value : tuple) {
            if (value->IsPhi() && value->Parent() == block && value->PhiBlockCount() != predecessors.size()) {
                return GiveUp(web, "a descriptor Phi's operands do not match its block's predecessors");
            }
        }
        IrValue& selector = m_program.CreateValue(IrOpcode::Phi, IrType::U32);
        web.selectors.emplace(key, &selector);
        web.phis.emplace_back(block, &selector);
        for (IrBlock* predecessor : predecessors) {
            std::vector<IrValue*> incoming;
            for (IrValue* value : tuple) {
                IrValue* edge = value->IsPhi() && value->Parent() == block ? PhiIncoming(*value, predecessor) : value;
                if (edge == nullptr) {
                    return GiveUp(web, "a descriptor Phi has no value for one of its predecessors");
                }
                incoming.push_back(edge);
            }
            IrValue* edgeSelector = WebSelector(std::move(incoming), web, position, activeMask);
            if (edgeSelector == nullptr) {
                return nullptr;
            }
            selector.AddPhiOperand(predecessor, edgeSelector);
        }
        return &selector;
    }

    IrValue* LaneSelector(const std::vector<IrValue*>& tuple, IrValue& first, DescriptorWeb& web, IrValue& position, const IrValue* activeMask) {
        IrBlock* block = first.Parent();
        const auto op = first.Opcode();
        const bool select = op == IrOpcode::SelectU32;
        if (block == nullptr || first.ArgumentCount() != (select ? 3u : 2u)) {
            return GiveUp(web, "a per-lane descriptor selection has an unexpected form");
        }
        if (web.selectors.size() == DescriptorWeb::maxStates) {
            return GiveUp(web, "more than 512 descriptor states");
        }
        const auto blockOrderIt = web.order.find(block);
        const std::size_t blockOrder = blockOrderIt == web.order.end() ? 0u : blockOrderIt->second;
        const std::size_t firstPos = InstructionPosition(block, &first);
        IrValue* shared = first.Argument(select ? 0u : 1u);
        std::vector<IrValue*> matched;
        std::vector<IrValue*> left;
        std::vector<IrValue*> right;
        for (IrValue* value : tuple) {
            if (value->Opcode() == op && value->Parent() == block && value->ArgumentCount() == first.ArgumentCount() && EquivalentValue(m_program.Resources(), value->Argument(select ? 0u : 1u), shared)) {
                matched.push_back(value);
                left.push_back(value->Argument(select ? 1u : 0u));
                if (select) {
                    right.push_back(value->Argument(2u));
                }
            } else {
                const bool leaf = !value->IsPhi() && value->Opcode() != IrOpcode::SelectU32 && value->Opcode() != IrOpcode::ReadFirstLane;
                const bool precedes = value->HasImmediate() || value->Parent() == nullptr ||
                    (value->Parent() == block ? (value->IsPhi() || InstructionPosition(block, value) < firstPos)
                                              : (web.order.contains(value->Parent()) && web.order.at(value->Parent()) < blockOrder));
                const bool uniformNonLane = !select && ((leaf && !Detail::HoldsFoldableValue(value, phiSearchDepth)) || (value->IsPhi() && precedes && activeMask == nullptr));
                if (value->HasImmediate() || (select && (leaf || precedes)) || uniformNonLane) {
                    left.push_back(value);
                    if (select) {
                        right.push_back(value);
                    }
                } else {
                    return GiveUp(web, "a per-lane descriptor selection mixes dwords selected differently");
                }
            }
        }
        auto& instructions = block->Instructions();
        auto latest = instructions.end();
        for (auto it = instructions.begin(); it != instructions.end(); ++it) {
            if (std::ranges::find(matched, *it) != matched.end()) {
                latest = it;
            }
        }
        if (latest == instructions.end()) {
            return GiveUp(web, "a per-lane descriptor selection is not in its block");
        }
        IrValue* before = std::next(latest) == instructions.end() ? nullptr : *std::next(latest);
        const IrValue* nextMask = select ? activeMask : shared;
        IrValue* leftSelector = WebSelector(std::move(left), web, position, nextMask);
        if (leftSelector == nullptr) {
            return nullptr;
        }
        if (select) {
            IrValue* rightSelector = WebSelector(std::move(right), web, position, activeMask);
            if (rightSelector == nullptr) {
                return nullptr;
            }
            if (leftSelector == rightSelector) {
                web.selectors.emplace(std::make_pair(tuple, activeMask), leftSelector);
                return leftSelector;
            }
            IrValue& node = m_program.CreateValue(op, IrType::U32, first.Flags<std::uint64_t>());
            node.AddArgument(shared);
            node.AddArgument(leftSelector);
            node.AddArgument(rightSelector);
            web.selectors.emplace(std::make_pair(tuple, activeMask), &node);
            web.lanes.emplace_back(block, before, &node);
            return &node;
        }
        if (leftSelector->HasImmediate()) {
            web.selectors.emplace(std::make_pair(tuple, activeMask), leftSelector);
            return leftSelector;
        }
        IrValue& node = m_program.CreateValue(op, IrType::U32, first.Flags<std::uint64_t>());
        node.AddArgument(leftSelector);
        node.AddArgument(shared);
        web.selectors.emplace(std::make_pair(tuple, activeMask), &node);
        web.lanes.emplace_back(block, before, &node);
        return &node;
    }

    IrValue* PointerSplitAddress(IrValue* handle) const {
        IrValue* addr = nullptr;
        for (std::size_t dword = 0; dword < handle->ArgumentCount(); dword++) {
            IrValue* load = handle->Argument(dword)->Resolve();
            std::uint32_t memoryIndex = 0u;
            if (load->Opcode() != IrOpcode::LoadAddressU32 || load->ArgumentCount() < 2u ||
                ScalarAddressMemory(*load, memoryIndex) == nullptr ||
                !RegisterOnlyOffset(load->Argument(1), phiSearchDepth)) {
                return nullptr;
            }
            IrValue* currentAddr = load->Argument(0)->Resolve();
            if (currentAddr->Opcode() != IrOpcode::GetAddressResource || currentAddr->ArgumentCount() != 2u) {
                return nullptr;
            }
            if (addr == nullptr) {
                addr = currentAddr;
            } else if (currentAddr != addr && !EquivalentValue(m_program.Resources(), addr, currentAddr)) {
                return nullptr;
            }
        }
        if (addr == nullptr) {
            return nullptr;
        }
        std::vector<IrValue*> addrPhis;
        CollectDescriptorPhis(addr->Argument(0), addrPhis, phiSearchDepth);
        CollectDescriptorPhis(addr->Argument(1), addrPhis, phiSearchDepth);
        return addrPhis.empty() ? nullptr : addr;
    }

    bool SplitDescriptorWeb(IrValue& inst, const std::array<IrValue*, 2>& handles, const std::array<bool, 2>& split, std::uint32_t handleCount, bool recordFailure = true) {
        std::vector<IrValue*> tuple;
        std::array<std::size_t, 2> offsets {};
        std::array<IrValue*, 2> pointerAddrs {};
        for (std::uint32_t slot = 0; slot < handleCount; slot++) {
            offsets[slot] = tuple.size();
            if (!split[slot]) {
                continue;
            }
            pointerAddrs[slot] = PointerSplitAddress(handles[slot]);
            if (pointerAddrs[slot] != nullptr) {
                tuple.push_back(pointerAddrs[slot]->Argument(0));
                tuple.push_back(pointerAddrs[slot]->Argument(1));
            } else {
                for (std::size_t dword = 0; dword < handles[slot]->ArgumentCount(); dword++) {
                    tuple.push_back(handles[slot]->Argument(dword));
                }
            }
        }
        DescriptorWeb web;
        IrValue* selector = WebSelector(tuple, web, inst);
        const bool select = selector != nullptr && !web.arms.empty() && (web.arms.size() > 1u || web.foldedValue);
        if (select) {
            for (const auto& [block, phi] : web.phis) {
                block->InsertInstructionBefore(nullptr, phi);
            }
            for (const auto& [block, before, node] : web.lanes) {
                if (before == nullptr) {
                    block->AppendInstruction(node);
                } else {
                    block->InsertInstructionBefore(before, node);
                }
            }
        } else {
            for (const auto& [block, phi] : web.phis) {
                for (std::size_t index = 0; index < phi->ArgumentCount(); index++) {
                    phi->ReplaceArgument(index, nullptr);
                }
            }
            for (const auto& [block, before, node] : web.lanes) {
                for (std::size_t index = 0; index < node->ArgumentCount(); index++) {
                    node->ReplaceArgument(index, nullptr);
                }
            }
            for (const auto& [block, phi] : web.phis) {
                phi->Invalidate();
            }
            for (const auto& [block, before, node] : web.lanes) {
                node->Invalidate();
            }
        }
        if (selector == nullptr || web.arms.empty()) {
            if (recordFailure) {
                const std::string reason = !web.failure.empty() ? web.failure : web.foldedValue ? std::string("no path's descriptor can be rebuilt from supported runtime values") : std::string("no descriptor was found on any path");
                for (std::uint32_t slot = 0; slot < handleCount; slot++) {
                    if (split[slot]) {
                        m_splitFailures.try_emplace(handles[slot], reason);
                    }
                }
            }
            return false;
        }

        if (web.foldedValue) {
            m_foldedWeb = true;
            const std::array<IrValue*, 2> compare {selector, &m_builder.Constant(DescriptorWeb::foldedArm)};
            IrValue* folded = EmitBefore(inst, IrOpcode::IEqual32, IrType::Bool, compare);
            EmitDescriptorFault(inst, *folded, m_builder.Constant(0u), m_builder.Constant(0u));
            const std::array<IrValue*, 3> first {folded, &m_builder.Constant(0u), selector};
            selector = EmitBefore(inst, IrOpcode::SelectU32, IrType::U32, first);
        }
        std::vector<IrValue*> conditions;
        for (std::uint32_t arm = 0; arm + 1u < web.arms.size(); arm++) {
            const std::array<IrValue*, 2> compare {selector, &m_builder.Constant(arm)};
            conditions.push_back(EmitBefore(inst, IrOpcode::IEqual32, IrType::Bool, compare));
        }
        std::vector<std::array<IrValue*, 2>> rebuiltAddrs(web.arms.size(), {nullptr, nullptr});
        ReplaceWithArmCopies(inst, handles, split, handleCount, conditions, pointerAddrs[0] == nullptr, [&](std::uint32_t arm, std::uint32_t slot, std::size_t dword) {
            if (pointerAddrs[slot] != nullptr) {
                IrValue*& armAddr = rebuiltAddrs[arm][slot];
                if (armAddr == nullptr) {
                    const std::array<IrValue*, 2> addrArgs {
                        Rematerialize(web.arms[arm][offsets[slot]], inst, true, phiSearchDepth),
                        Rematerialize(web.arms[arm][offsets[slot] + 1u], inst, true, phiSearchDepth),
                    };
                    armAddr = EmitCopy(*pointerAddrs[slot], addrArgs, inst);
                }
                const IrValue* origLoad = handles[slot]->Argument(dword)->Resolve();
                std::vector<IrValue*> loadArgs;
                loadArgs.reserve(origLoad->ArgumentCount());
                loadArgs.push_back(armAddr);
                for (std::size_t idx = 1u; idx < origLoad->ArgumentCount(); idx++) {
                    loadArgs.push_back(Rematerialize(origLoad->Argument(idx), inst, true, phiSearchDepth));
                }
                return EmitCopy(*origLoad, loadArgs, inst);
            }
            return Rematerialize(web.arms[arm][offsets[slot] + dword], inst, true, phiSearchDepth);
        });
        return true;
    }

    std::string SplitFailure(const IrValue& handle) const {
        const auto found = m_splitFailures.find(&handle);
        return found == m_splitFailures.end() ? std::string() : "; descriptor web: " + found->second;
    }

    void SplitDescriptorPhis() {
        std::vector<IrValue*> candidates;
        for (auto& block : m_program.Blocks()) {
            for (IrValue* inst : block->Instructions()) {
                if (SplittableImageRead(*inst)) {
                    candidates.push_back(inst);
                }
            }
        }
        for (IrValue* inst : candidates) {
            SplitDescriptorPhi(*inst);
        }
    }

    void PlanTableColumns() {
        static const bool enabled = std::getenv("APS5_NO_IMAGE_TABLES") == nullptr;
        if (!enabled) {
            return;
        }
        const auto sources = m_sources;
        const auto columns = m_tableColumns;
        const auto reads = m_tablePlanningReads;
        const auto memory = m_tablePlanningMemory;
        PlanTableColumnPass();
        while (!PlannedWithinLimits()) {
            const auto group = LargestSelectedGroup();
            if (group.empty()) {
                return;
            }
            m_unselectedHandles.insert(m_unselectedHandles.end(), group.begin(), group.end());
            m_sources = sources;
            m_tableColumns = columns;
            m_tablePlanningReads = reads;
            m_tablePlanningMemory = memory;
            PlanTableColumnPass();
        }
    }

    bool PlannedWithinLimits() {
        std::vector<ImageResource> images;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;
        std::vector<const IrValue*> directImages;
        std::vector<std::pair<const IrValue*, bool>> directSamplers;
        const auto direct = std::numeric_limits<std::uint32_t>::max() / 2u;
        for (auto& block : m_program.Blocks()) {
            for (IrValue* inst : block->Instructions()) {
                const auto imageInfo = ImageOpcodeInfoOf(inst->Opcode());
                if (inst->Opcode() == IrOpcode::ImageBvhIntersectRay || imageInfo.access == ImageAccess::None || imageInfo.resourceClass == ImageResourceClass::None || inst->ArgumentCount() == 0u) {
                    continue;
                }
                const auto flags = inst->Flags<MemoryFlags>();
                if (flags.index >= m_program.Resources().memoryInfo.size()) {
                    continue;
                }
                const auto& memory = m_program.Resources().memoryInfo[flags.index];
                if (memory.kind != ResourceKind::Image || memory.planningOnly || IsTablePlanningMemory(flags.index)) {
                    continue;
                }
                const IrValue* handle = inst->Argument(0)->Resolve();
                const TableColumnPlan* column = FindTableColumn(*handle);
                std::uint32_t source = 0;
                if (column != nullptr) {
                    source = column->source;
                } else {
                    const auto found = std::ranges::find_if(directImages, [&](const IrValue* other) { return SameHandle(*other, *handle); });
                    source = direct + static_cast<std::uint32_t>(found - directImages.begin());
                    if (found == directImages.end()) directImages.push_back(handle);
                }
                const auto key = ImageKey(source, memory, inst->Opcode());
                const auto image = std::ranges::find_if(images, [&](const ImageResource& other) { return SameImageKey(other, key); });
                const auto imageIndex = static_cast<std::uint32_t>(image - images.begin());
                if (image == images.end()) images.push_back(key);
                if (!imageInfo.needsSampler || inst->ArgumentCount() < 2u) {
                    continue;
                }
                const IrValue* samplerHandle = inst->Argument(1)->Resolve();
                const TableColumnPlan* samplerColumn = FindTableColumn(*samplerHandle);
                std::uint32_t sampler = 0;
                if (samplerColumn != nullptr) {
                    sampler = samplerColumn->source;
                } else {
                    const bool sampleAdjust = (memory.imageSampleFlags & RdnaImageSampleFlagAdjust) != 0;
                    const auto found = std::ranges::find_if(directSamplers, [&](const auto& other) { return other.second == sampleAdjust && SameHandle(*other.first, *samplerHandle); });
                    sampler = direct + static_cast<std::uint32_t>(found - directSamplers.begin());
                    if (found == directSamplers.end()) directSamplers.emplace_back(samplerHandle, sampleAdjust);
                }
                if (std::ranges::find(pairs, std::pair{imageIndex, sampler}) == pairs.end()) pairs.emplace_back(imageIndex, sampler);
            }
        }
        return images.size() <= ShaderInfo::MaxImages && pairs.size() <= ShaderInfo::MaxSampledPairs;
    }

    std::vector<const IrValue*> LargestSelectedGroup() const {
        const auto unselected = [](TableColumn column) {
            column.selected = false;
            column.selection = 0u;
            return column;
        };
        std::vector<std::vector<std::uint32_t>> groups;
        for (const auto& plan : m_tableColumns) {
            const auto& column = m_sources[plan.source].tableColumn;
            if (!column.has_value()) {
                continue;
            }
            const auto group = std::ranges::find_if(groups, [&](const std::vector<std::uint32_t>& members) { return unselected(*m_sources[members.front()].tableColumn) == unselected(*column); });
            if (group == groups.end()) {
                groups.push_back({plan.source});
            } else if (std::ranges::find(*group, plan.source) == group->end()) {
                group->push_back(plan.source);
            }
        }
        const std::vector<std::uint32_t>* largest = nullptr;
        for (const auto& group : groups) {
            const bool selected = std::ranges::any_of(group, [&](std::uint32_t source) { return m_sources[source].tableColumn->selected; });
            if (selected && group.size() > 1u && (largest == nullptr || group.size() > largest->size())) {
                largest = &group;
            }
        }
        std::vector<const IrValue*> handles;
        if (largest == nullptr) {
            return handles;
        }
        for (const auto& plan : m_tableColumns) {
            if (std::ranges::find(*largest, plan.source) != largest->end()) {
                handles.push_back(plan.handle);
            }
        }
        return handles;
    }

    void PlanTableColumnPass() {
        IndexMemoryUsers();
        const auto directImages = DirectImageHandles();
        for (auto& block : m_program.Blocks()) {
            for (IrValue* inst : block->Instructions()) {
                const auto imageInfo = ImageOpcodeInfoOf(inst->Opcode());
                if (imageInfo.access == ImageAccess::None || inst->ArgumentCount() == 0u) {
                    continue;
                }
                const auto flags = inst->Flags<MemoryFlags>();
                if (flags.index >= m_program.Resources().memoryInfo.size()) {
                    continue;
                }
                const auto memory = m_program.Resources().memoryInfo[flags.index];
                IrValue& image = *inst->Argument(0)->Resolve();
                if (std::ranges::none_of(directImages, [&](const IrValue* handle) { return SameHandle(*handle, image); })) {
                    static_cast<void>(PlanTableColumn(image, false, memory.imageR128));
                }
                if (imageInfo.needsSampler && inst->ArgumentCount() > 1u && !DirectSampler(*inst->Argument(1)->Resolve())) {
                    static_cast<void>(PlanTableColumn(*inst->Argument(1)->Resolve(), true, false));
                }
            }
        }
        const auto onlyUsedByTableColumns = [&](const IrValue* value) {
            std::vector<const IrValue*> pending(value->Uses().begin(), value->Uses().end());
            std::unordered_set<const IrValue*> seen;
            while (!pending.empty()) {
                const IrValue* user = pending.back();
                pending.pop_back();
                if (!seen.insert(user).second) continue;
                if (FindTableColumn(*user) != nullptr) continue;
                if (!user->IsPhi()) return false;
                pending.insert(pending.end(), user->Uses().begin(), user->Uses().end());
            }
            return true;
        };
        for (const auto& plan : m_tableColumns) {
            for (std::size_t index = 0; index < plan.reads.size(); index++) {
                const IrValue* read = plan.reads[index];
                const bool planned = onlyUsedByTableColumns(read);
                if (planned && std::ranges::find(m_tablePlanningReads, read) == m_tablePlanningReads.end()) {
                    m_tablePlanningReads.push_back(read);
                    m_tablePlanningMemory.push_back(plan.memory[index]);
                }
            }
        }
        for (bool marked = true; marked;) {
            marked = false;
            for (const auto& plan : m_tableColumns) {
                for (std::size_t index = 0; index < plan.hopReads.size(); index++) {
                    const IrValue* read = plan.hopReads[index];
                    if (std::ranges::find(m_tablePlanningReads, read) != m_tablePlanningReads.end()) {
                        continue;
                    }
                    const bool planned = std::ranges::all_of(read->Uses(), [&](const IrValue* pointer) {
                        return pointer->Opcode() == IrOpcode::GetAddressResource && std::ranges::all_of(pointer->Uses(), [&](const IrValue* user) {
                            return std::ranges::find(m_tablePlanningReads, user) != m_tablePlanningReads.end();
                        });
                    });
                    if (planned) {
                        m_tablePlanningReads.push_back(read);
                        m_tablePlanningMemory.push_back(plan.hopMemory[index]);
                        marked = true;
                    }
                }
            }
        }
        m_memoryUsersIndexed = false;
        m_memoryUsers.clear();
    }

    void KeepSourceAlive(IrValue& position, const DescriptorSource& source, std::vector<const IrValue*>& referenced) {
        for (std::uint32_t dword = 0; dword < source.dwordCount; dword++) {
            IrValue* value = source.dwords[dword]->Resolve();
            if (value->HasImmediate() || std::ranges::find(referenced, value) != referenced.end()) {
                continue;
            }
            referenced.push_back(value);
            const std::array<IrValue*, 1> arguments{value};
            EmitBefore(position, IrOpcode::ReferenceU32, IrType::Void, arguments);
        }
    }

    bool DirectSampler(const IrValue& handle) {
        if (handle.Opcode() != IrOpcode::GetSamplerResource || handle.ArgumentCount() != 4u || FindTableColumn(handle) != nullptr) {
            return false;
        }
        DescriptorSource descriptor;
        MakeSource(handle, 4u, true, false, descriptor);
        std::uint32_t badDword = 0;
        return ValidateSource(descriptor, badDword);
    }

    bool DirectImage(const IrValue& handle) {
        if (handle.Opcode() != IrOpcode::GetImageResource || handle.ArgumentCount() != 8u) {
            return false;
        }
        DescriptorSource descriptor;
        MakeSource(handle, 8u, false, false, descriptor);
        std::uint32_t badDword = 0;
        return ValidateSource(descriptor, badDword);
    }

    std::vector<const IrValue*> DirectImageHandles() {
        std::vector<const IrValue*> handles;
        for (auto& block : m_program.Blocks()) {
            for (IrValue* inst : block->Instructions()) {
                const auto imageInfo = ImageOpcodeInfoOf(inst->Opcode());
                if (imageInfo.access == ImageAccess::None || inst->ArgumentCount() == 0u) {
                    continue;
                }
                const auto flags = inst->Flags<MemoryFlags>();
                if (flags.index >= m_program.Resources().memoryInfo.size()) {
                    continue;
                }
                const auto& memory = m_program.Resources().memoryInfo[flags.index];
                const bool tableUnsupported = imageInfo.resourceClass == ImageResourceClass::Storage || memory.imagePacked || memory.imageByElements != 0u;
                const IrValue* handle = inst->Argument(0)->Resolve();
                if (tableUnsupported && DirectImage(*handle)) {
                    handles.push_back(handle);
                }
            }
        }
        return handles;
    }

    bool SameHandle(const IrValue& left, const IrValue& right) const {
        if (&left == &right) {
            return true;
        }
        if (left.Opcode() != right.Opcode() || left.ArgumentCount() != right.ArgumentCount()) {
            return false;
        }
        for (std::size_t index = 0; index < left.ArgumentCount(); index++) {
            if (!EquivalentValue(m_program.Resources(), left.Argument(index), right.Argument(index))) {
                return false;
            }
        }
        return true;
    }

    bool IsTablePlanningMemory(std::uint32_t index) const {
        return std::ranges::find(m_tablePlanningMemory, index) != m_tablePlanningMemory.end();
    }

    bool TableSource(std::uint32_t source) const {
        return source < m_sources.size() && m_sources[source].tableColumn.has_value();
    }

    bool SameBufferSource(const IrValue& handle, std::uint32_t source) const {
        const auto& current = m_sources[source];
        if (current.tableColumn.has_value() || current.dwordCount != 4u || handle.ArgumentCount() != 4u) {
            return false;
        }
        for (std::uint32_t dword = 0; dword < 4u; dword++) {
            if (!EquivalentValue(m_program.Resources(), current.dwords[dword], handle.Argument(dword))) {
                return false;
            }
        }
        return true;
    }

    bool SelectionWritten(const TableColumn& column) const {
        const auto written = [&](const auto& matches) {
            return std::any_of(m_info.buffers.begin(), m_info.buffers.end(), [&](const auto& buffer) { return (buffer.written || buffer.atomic) && matches(buffer.source); });
        };
        std::vector<const IrValue*> pending{m_sources[column.selection].dwords[0]};
        std::vector<const IrValue*> seen;
        while (!pending.empty()) {
            const IrValue* current = pending.back()->Resolve();
            pending.pop_back();
            if (std::ranges::find(seen, current) != seen.end()) {
                continue;
            }
            seen.push_back(current);
            const bool pointer = current->Opcode() == IrOpcode::LoadAddressU32;
            const bool buffer = current->Opcode() == IrOpcode::ReadConstBuffer && current->ArgumentCount() != 0u;
            if ((pointer || buffer) && m_info.bdaWrites) {
                return true;
            }
            if (pointer && written([](std::uint32_t) { return true; })) {
                return true;
            }
            if (buffer) {
                const IrValue* handle = current->Argument(0)->Resolve();
                if (written([&](std::uint32_t source) { return SameBufferSource(*handle, source); })) {
                    return true;
                }
            }
            for (std::size_t index = 0; index < current->ArgumentCount(); index++) {
                pending.push_back(current->Argument(index));
            }
        }
        return false;
    }

    void CheckTableStores() {
        for (auto& source : m_sources) {
            if (source.tableColumn.has_value() && source.tableColumn->selected && SelectionWritten(*source.tableColumn)) {
                source.tableColumn->selected = false;
                source.tableColumn->keyDomains.clear();
            }
            if (!source.tableColumn.has_value() || source.tableColumn->keyDomains.empty()) {
                continue;
            }
            auto& column = *source.tableColumn;
            const bool written = std::any_of(column.keyDomains.begin(), column.keyDomains.end(), [&](const KeyDomain& domain) {
                return std::any_of(m_info.buffers.begin(), m_info.buffers.end(), [&](const auto& buffer) { return (buffer.written || buffer.atomic) && buffer.source == domain.source; });
            });
            if (written) {
                column.keyDomains.clear();
            }
        }
    }

    void AssignTables() {
        std::uint32_t next = 0;
        for (auto& image : m_info.images) {
            if (!TableSource(image.source)) {
                continue;
            }
            image.table = next++;
        }
        for (auto& sampler : m_info.samplers) {
            if (TableSource(sampler.source)) {
                sampler.table = next++;
            }
        }
    }

    void GetHandle(IrValue* value, IrOpcode expected, std::uint32_t width, IrValue*& handle, std::uint32_t& source, bool sampler = false, bool sampleAdjust = false) {
        handle = value->Resolve();
        if (handle->Opcode() != expected) {
            fail("memory operation requires " + std::string(IrOpcodeName(expected)));
        }
        DescriptorSource descriptor;
        MakeSource(*handle, width, sampler, sampleAdjust, descriptor);
        std::uint32_t badDword = 0;
        if (!ValidateSource(descriptor, badDword)) {
            fail(std::string(IrOpcodeName(expected)) + " dword " + std::to_string(badDword) + " is not a valid runtime value; chain: " + describeValueChain(descriptor.dwords[badDword], 8u) + SplitFailure(*handle));
        }
        source = InternSource(descriptor);
    }

    bool TakeGpuDescriptor(IrValue& inst, std::uint32_t memoryIndex) {
        const IrValue* handle = inst.Argument(0)->Resolve();
        if (handle->Opcode() != IrOpcode::GetBufferResource || handle->ArgumentCount() != 4u) {
            return false;
        }
        DescriptorSource descriptor;
        MakeSource(*handle, 4u, false, false, descriptor);
        std::uint32_t badDword = 0;
        auto& memory = m_program.Resources().memoryInfo[memoryIndex];
        const auto access = BufferAccessOf(inst.Opcode());
        if (ValidateSource(descriptor, badDword)) {
            const auto source = InternSource(descriptor);
            const auto resource = AddBuffer(source, memory, inst.Opcode(), inst.Flags<MemoryFlags>().pc);
            if (resource == std::numeric_limits<std::uint32_t>::max()) fail("buffer resource limit exceeded");
            AddMemoryPatch(memoryIndex, resource, 0u, false);
            memory.gpuDescriptor = false;
            m_info.usesFaultBuffer = m_info.usesFaultBuffer || access == BufferAccess::Atomic;
            return true;
        }
        memory.gpuDescriptor = true;
        m_info.usesDma = true;
        m_info.bdaWrites = m_info.bdaWrites || access == BufferAccess::Write || access == BufferAccess::Atomic;
        return true;
    }

    void ValidateAddressHandle(IrValue* value) const {
        const IrValue* handle = value->Resolve();
        if (handle->Opcode() != IrOpcode::GetAddressResource) {
            fail("address operation requires GetAddressResource");
        }
        if (handle->ArgumentCount() != 2) {
            fail("GetAddressResource must have two address dwords");
        }
    }

    std::uint32_t AddBuffer(std::uint32_t source, const MemoryInfo& memory, IrOpcode op, std::uint32_t pc) {
        for (std::uint32_t i = 0; i < m_info.buffers.size(); i++) {
            if (m_info.buffers[i].source == source) {
                Merge(m_info.buffers[i], memory, op, pc);
                return i;
            }
        }
        BufferResource resource;
        resource.source = source;
        resource.firstUsePc = pc;
        Merge(resource, memory, op, pc);
        m_info.buffers.push_back(resource);
        return static_cast<std::uint32_t>(m_info.buffers.size() - 1);
    }

    static void Merge(BufferResource& resource, const MemoryInfo& memory, IrOpcode op, std::uint32_t pc) {
        const auto access = BufferAccessOf(op);
        const bool atomic = access == BufferAccess::Atomic;
        const bool write = access == BufferAccess::Write || atomic;
        resource.firstUsePc = std::min(resource.firstUsePc, pc);
        resource.maxByteExtent = std::max(resource.maxByteExtent, byteExtent(memory));
        resource.read = resource.read || !write || atomic;
        resource.written = resource.written || write;
        resource.atomic = resource.atomic || atomic;
        resource.formatted = resource.formatted || memory.formatted;
        resource.descriptorFormatted = resource.descriptorFormatted || (memory.formatted && !memory.typed);
        if (memory.formatted && memory.typed) {
            const auto format = GetFormatInfo(DecodeTBufferFormat(memory.dataFormat, memory.numberFormat));
            if (format.byteSize == 0u) fail("typed buffer instruction has an invalid format");
            resource.typedAlignment = std::max(resource.typedAlignment, static_cast<std::uint8_t>(std::min(format.byteSize, 4u)));
        }
        if (memory.formatted && !memory.typed && !write) resource.formattedReadMask |= (1u << std::min(memory.dataDwords, 4u)) - 1u;
        resource.scalar = resource.scalar || op == IrOpcode::ReadConstBuffer || memory.kind == ResourceKind::ScalarBuffer;
    }

    static ImageResource ImageKey(std::uint32_t source, const MemoryInfo& memory, IrOpcode op) {
        ImageResource image;
        image.source = source;
        image.resourceClass = ImageOpcodeInfoOf(op).resourceClass;
        image.depthCompare = (memory.imageSampleFlags & RdnaImageSampleFlagCompare) != 0;
        image.remappedDepth1D = image.depthCompare && (memory.imageDimension == RdnaImageDimension::Dim1D || memory.imageDimension == RdnaImageDimension::Dim1DArray);
        image.dimension = image.remappedDepth1D
            ? (memory.imageDimension == RdnaImageDimension::Dim1D ? RdnaImageDimension::Dim2D : RdnaImageDimension::Dim2DArray)
            : memory.imageDimension;
        image.mipMode = image.resourceClass == ImageResourceClass::Storage && memory.imageHasMip ? ImageMipMode::DynamicStorage : ImageMipMode::None;
        image.r128 = memory.imageR128;
        image.packed = memory.imagePacked;
        image.byElements = memory.imageByElements;
        image.byComponents = memory.imageByElements != 0u ? memory.dataDwords / memory.imageByElements : 0u;
        return image;
    }

    static bool SameImageKey(const ImageResource& image, const ImageResource& key) {
        return image.source == key.source && image.resourceClass == key.resourceClass && image.dimension == key.dimension && image.mipMode == key.mipMode && image.depthCompare == key.depthCompare && image.remappedDepth1D == key.remappedDepth1D && image.r128 == key.r128 && image.packed == key.packed && image.byElements == key.byElements && image.byComponents == key.byComponents;
    }

    std::uint32_t AddImage(std::uint32_t source, const MemoryInfo& memory, IrOpcode op, std::uint32_t pc) {
        const auto key = ImageKey(source, memory, op);
        for (std::uint32_t i = 0; i < m_info.images.size(); i++) {
            auto& image = m_info.images[i];
            if (SameImageKey(image, key)) {
                Merge(image, memory, op, pc);
                return i;
            }
        }
        if (m_info.images.size() >= ShaderInfo::MaxImages) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        ImageResource image = key;
        image.singleMipPlaneBiasCompatible = m_program.Resources().stage == IrShaderStage::Pixel;
        image.volumeArraySampleCompatible = true;
        image.firstUsePc = pc;
        image.optionalDescriptor = memory.descriptorWebArm;
        Merge(image, memory, op, pc);
        m_info.images.push_back(image);
        return static_cast<std::uint32_t>(m_info.images.size() - 1);
    }

    static void Merge(ImageResource& image, const MemoryInfo& memory, IrOpcode op, std::uint32_t pc) {
        image.optionalDescriptor = image.optionalDescriptor && memory.descriptorWebArm;
        const auto access = ImageOpcodeInfoOf(op).access;
        if (memory.imagePacked && op != IrOpcode::ImageRead && op != IrOpcode::ImageWrite) image.tableOperation = TableOperation::Unsupported;
        const bool atomic = access == ImageAccess::Atomic;
        const bool write = access == ImageAccess::Write || atomic;
        const bool atomic64 = IsImageAtomic64Opcode(op);
        if ((image.read || image.written) && image.atomic64 != atomic64) {
            throw std::runtime_error("an image accessed by 64-bit atomics is also accessed in another way");
        }
        image.atomic64 = atomic64;
        image.firstUsePc = std::min(image.firstUsePc, pc);
        image.read = image.read || !write || atomic;
        image.written = image.written || write;
        image.atomic = image.atomic || atomic;
        image.gathered = image.gathered || op == IrOpcode::ImageGatherRaw;
        image.srgbDecodeCompatible = image.srgbDecodeCompatible && !ImageOpcodeInfoOf(op).needsSampler;
        image.fmaskCompatible = image.fmaskCompatible && op == IrOpcode::ImageRead && memory.dataBits == 32u;
        image.singleMipPlaneBiasCompatible = image.singleMipPlaneBiasCompatible && op == IrOpcode::ImageSampleRaw && memory.imageSampleFlags == RdnaImageSampleFlagBias && memory.dataBits == 32u && !memory.imageR128 && !memory.imagePacked && memory.imageByElements == 0u;
        image.depthBitsCompatible = image.depthBitsCompatible && memory.dataBits == 32u;
        image.constantSwizzleCompatible = image.constantSwizzleCompatible && (op == IrOpcode::ImageSampleRaw || op == IrOpcode::ImageGatherRaw);
        const bool cubeFaceLoad = op == IrOpcode::ImageRead && !memory.imageHasMip && memory.imageSampleFlags == 0u;
        const auto addressedFlags = memory.imageSampleFlags & ~RdnaImageSampleFlagAdjust;
        image.adjustedSample = image.adjustedSample || (op == IrOpcode::ImageSampleRaw && (memory.imageSampleFlags & RdnaImageSampleFlagAdjust) != 0u);
        const bool cubeFaceSample = op == IrOpcode::ImageSampleRaw && (addressedFlags == 0u || addressedFlags == RdnaImageSampleFlagLevelZero || addressedFlags == RdnaImageSampleFlagLod);
        const bool cubeFaceStore = op == IrOpcode::ImageWrite && !memory.imageHasMip;
        image.cubeFaceCompatible = image.cubeFaceCompatible && (cubeFaceLoad || cubeFaceSample || cubeFaceStore) && !memory.imagePacked && memory.imageByElements == 0u;
        image.volumeArraySampleCompatible = image.volumeArraySampleCompatible && cubeFaceSample && !memory.imageR128 && !memory.imagePacked && memory.imageByElements == 0u;
        const bool flatVolumeLoad = op == IrOpcode::ImageRead && memory.imageSampleFlags == 0u;
        const bool flatVolumeSample = op == IrOpcode::ImageSampleRaw && memory.imageSampleFlags == RdnaImageSampleFlagLevelZero;
        const bool flatVolumeQuery = op == IrOpcode::ImageQueryDimensions;
        image.volumeGather = image.volumeGather || (op == IrOpcode::ImageGatherRaw && image.dimension == RdnaImageDimension::Dim3D);
        image.flatVolumeCompatible = image.flatVolumeCompatible && (flatVolumeLoad || flatVolumeSample || flatVolumeQuery) && !memory.imagePacked && memory.imageByElements == 0u;
        const bool flatLineLoad = op == IrOpcode::ImageRead && !memory.imageHasMip && memory.imageSampleFlags == 0u;
        const auto lineSampleFlags = memory.imageSampleFlags & ~RdnaImageSampleFlagCompare;
        const bool flatLineSample = op == IrOpcode::ImageSampleRaw && (lineSampleFlags == RdnaImageSampleFlagLevelZero || lineSampleFlags == 0u);
        const bool flatLineExplicitLod = op == IrOpcode::ImageSampleRaw && memory.imageSampleFlags == RdnaImageSampleFlagLod;
        const bool flatLineLevelZero = op == IrOpcode::ImageSampleRaw && memory.imageSampleFlags == RdnaImageSampleFlagLevelZero;
        const bool flatLineGather = op == IrOpcode::ImageGatherRaw && memory.imageSampleFlags == RdnaImageSampleFlagLevelZero;
        const bool flatLineAccess = image.dimension == RdnaImageDimension::Dim2DArray ? flatLineLevelZero || flatLineExplicitLod : flatLineLoad || flatLineSample || flatLineExplicitLod || flatLineGather;
        image.flatLineCompatible = image.flatLineCompatible && flatLineAccess && !memory.imagePacked && memory.imageByElements == 0u;
        image.flatVolumeLineCompatible = image.flatVolumeLineCompatible && (flatLineLoad || flatLineSample) && !memory.imagePacked && memory.imageByElements == 0u;
        const bool layerRowsSample = op == IrOpcode::ImageSampleRaw && (memory.imageSampleFlags == RdnaImageSampleFlagLevelZero || memory.imageSampleFlags == RdnaImageSampleFlagLod);
        image.layerRowsCompatible = image.layerRowsCompatible && layerRowsSample && !memory.imageR128 && !memory.imagePacked && memory.imageByElements == 0u;
        image.dimensionQueryOnly = image.dimensionQueryOnly && op == IrOpcode::ImageQueryDimensions;
        image.sampledOnly = image.sampledOnly && ImageOpcodeInfoOf(op).needsSampler;
        if ((memory.imageSampleFlags & RdnaImageSampleFlagCompare) != 0u) {
            constexpr auto unsupported = RdnaImageSampleFlagLod | RdnaImageSampleFlagDerivative;
            if (op == IrOpcode::ImageGatherRaw || (memory.imageSampleFlags & unsupported) != 0u) image.emulatedCompare |= EmulatedCompare::Unsupported;
            if ((memory.imageSampleFlags & RdnaImageSampleFlagLevelZero) == 0u) image.emulatedCompare |= EmulatedCompare::RequiresSingleLevel;
        } else if (ImageOpcodeInfoOf(op).needsSampler) {
            if (op != IrOpcode::ImageSampleRaw) image.emulatedCompare |= EmulatedCompare::ConvertedSampleUnsupported;
            if ((memory.imageSampleFlags & RdnaImageSampleFlagLevelZero) == 0u) image.emulatedCompare |= EmulatedCompare::ConvertedSampleRequiresSingleLevel;
        }
    }

    std::uint32_t AddSampler(std::uint32_t source, std::uint32_t pc) {
        for (std::uint32_t i = 0; i < m_info.samplers.size(); i++) {
            if (m_info.samplers[i].source == source) {
                m_info.samplers[i].firstUsePc = std::min(m_info.samplers[i].firstUsePc, pc);
                return i;
            }
        }
        if (m_info.samplers.size() >= ShaderInfo::MaxSamplers) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        m_info.samplers.push_back({source, pc});
        return static_cast<std::uint32_t>(m_info.samplers.size() - 1);
    }

    void AddSampledPair(std::uint32_t image, std::uint32_t sampler, std::uint32_t pc, bool volumeGather) {
        for (auto& pair : m_info.sampledPairs) {
            if (pair.image == image && pair.sampler == sampler) {
                pair.firstUsePc = std::min(pair.firstUsePc, pc);
                pair.volumeGather = pair.volumeGather || volumeGather;
                return;
            }
        }
        if (m_info.sampledPairs.size() >= ShaderInfo::MaxSampledPairs) {
            fail("sampled image/sampler pair limit exceeded");
        }
        m_info.sampledPairs.push_back({image, sampler, pc, volumeGather});
    }

    void AddHandlePatch(IrValue* handle, std::uint32_t resource) {
        for (const auto& patch : m_handlePatches) {
            if (patch.handle == handle) {
                if (patch.resource != resource) {
                    fail(std::string(IrOpcodeName(handle->Opcode())) + " is reused with incompatible resource classes");
                }
                return;
            }
        }
        m_handlePatches.push_back({handle, resource});
    }

    void AddMemoryPatch(std::uint32_t index, std::uint32_t resource, std::uint32_t sampler, bool hasSampler) {
        for (auto& patch : m_memoryPatches) {
            if (patch.index != index) {
                continue;
            }
            if (patch.resource != resource || (hasSampler && patch.hasSampler && patch.sampler != sampler)) {
                fail("memory metadata is reused with incompatible resources");
            }
            if (hasSampler) {
                patch.sampler = sampler;
                patch.hasSampler = true;
            }
            return;
        }
        m_memoryPatches.push_back({index, resource, sampler, hasSampler});
    }

    void Collect(IrValue& inst) {
        const auto op = inst.Opcode();
        if (op == IrOpcode::ImageBvhIntersectRay) {
            m_info.usesDma = true;
            return;
        }
        const auto buffer = BufferAccessOf(op);
        const auto addressInfo = AddressOpcodeInfoOf(op);
        const auto imageInfo = ImageOpcodeInfoOf(op);
        if (buffer == BufferAccess::None && addressInfo.access == AddressAccess::None && imageInfo.access == ImageAccess::None) {
            return;
        }
        const auto flags = inst.Flags<MemoryFlags>();
        if (flags.index >= m_program.Resources().memoryInfo.size()) {
            fail("memory metadata index " + std::to_string(flags.index) + " is out of range");
        }
        if (inst.ArgumentCount() == 0) {
            fail("memory operation has no resource handle");
        }
        const auto& memory = m_program.Resources().memoryInfo[flags.index];
        if (memory.planningOnly || IsTablePlanningMemory(flags.index)) {
            return;
        }
        IrValue* handle = nullptr;
        std::uint32_t source = 0;
        std::uint32_t resource = 0;

        if (buffer != BufferAccess::None) {
            if (!TakeGpuDescriptor(inst, flags.index)) fail("buffer operation requires a four-dword runtime V#");
            return;
        }
        if (addressInfo.access != AddressAccess::None) {
            if (!IsAddressResourceKind(memory.kind)) {
                fail("address operation has invalid resource kind");
            }
            if (memory.kind == ResourceKind::Scratch) {
                handle = inst.Argument(0)->Resolve();
                if (handle->Opcode() != IrOpcode::GetScratchResource || handle->ArgumentCount() != 0) {
                    fail("scratch operation requires GetScratchResource");
                }
                if (m_info.scratchDwords == 0) {
                    fail("scratch operation requires a nonzero AGC per-thread size");
                }
                return;
            }
            ValidateAddressHandle(inst.Argument(0));
            // Debug aid: APS5_TRACE_BDA=1 names every access that makes the program address-based
            // (a raw scalar load the SRT walker left in place, or a flat/global access), so the
            // reason a stage takes the BDA path can be read off without a shader dump.
            // The first few accesses of a program are printed (a Bink kernel has hundreds); Run
            // reports how many more there were.
            if (bdaTraceEnabled() && ++m_bdaTraces <= bdaTraceLimit) {
                const auto* kind = memory.kind == ResourceKind::ScalarAddress ? "scalar address" : memory.kind == ResourceKind::Global ? "global" : "flat";
                const IrValue* offset = inst.ArgumentCount() > 1 ? inst.Argument(1)->Resolve() : nullptr;
                const bool immediateOffset = offset != nullptr && offset->HasImmediate();
                std::fprintf(stderr, "[bda] %s at pc 0x%08x: %s access, offset %s%s\n", std::string(IrOpcodeName(op)).c_str(), flags.pc, kind, immediateOffset ? "immediate" : "dynamic", memory.kind == ResourceKind::ScalarAddress && !immediateOffset ? " (a register offset is not planned by the SRT walker)" : "");
            }
            m_info.usesDma = true;
            m_info.bdaWrites = m_info.bdaWrites || addressInfo.access == AddressAccess::Write || addressInfo.access == AddressAccess::Atomic;
            return;
        }

        if (memory.kind != ResourceKind::Image || imageInfo.resourceClass == ImageResourceClass::None) {
            fail("image operation has invalid resource kind");
        }
        handle = inst.Argument(0)->Resolve();
        const TableColumnPlan* column = FindTableColumn(*handle);
        if (column != nullptr) {
            source = column->source;
        } else {
            GetHandle(inst.Argument(0), IrOpcode::GetImageResource, 8, handle, source);
        }
        resource = AddImage(source, memory, op, flags.pc);
        NoteBlockImage(resource);
        if (resource == std::numeric_limits<std::uint32_t>::max()) {
            fail("image resource limit exceeded (" + std::to_string(m_info.images.size() + 1u) + " of " + std::to_string(ShaderInfo::MaxImages) + ")");
        }
        AddHandlePatch(handle, resource);
        std::uint32_t sampler = 0;
        if (imageInfo.needsSampler) {
            if (inst.ArgumentCount() < 2) {
                fail("sampled image operation has no sampler handle");
            }
            IrValue* samplerHandle = inst.Argument(1)->Resolve();
            std::uint32_t samplerSource = 0;
            const TableColumnPlan* samplerColumn = FindTableColumn(*samplerHandle);
            if (samplerColumn != nullptr) {
                samplerSource = samplerColumn->source;
            } else {
                const bool sampleAdjust = (memory.imageSampleFlags & RdnaImageSampleFlagAdjust) != 0;
                GetHandle(inst.Argument(1), IrOpcode::GetSamplerResource, 4, samplerHandle, samplerSource, true, sampleAdjust);
            }
            sampler = AddSampler(samplerSource, flags.pc);
            if (sampler == std::numeric_limits<std::uint32_t>::max()) {
                fail("sampler resource limit exceeded");
            }
            AddHandlePatch(samplerHandle, sampler);
            AddSampledPair(resource, sampler, flags.pc, op == IrOpcode::ImageGatherRaw && m_info.images[resource].dimension == RdnaImageDimension::Dim3D);
        }
        AddMemoryPatch(flags.index, resource, sampler, imageInfo.needsSampler);
    }

    const DescriptorSource* Source(std::uint32_t source) const {
        return source < m_sources.size() ? &m_sources[source] : nullptr;
    }

    void LinkImageAliases() {
        for (auto& buffer : m_info.buffers) {
            const DescriptorSource* bufferSource = Source(buffer.source);
            if (bufferSource == nullptr || bufferSource->dwordCount != 4) {
                continue;
            }
            for (std::uint32_t image = 0; image < m_info.images.size(); image++) {
                const DescriptorSource* imageSource = Source(m_info.images[image].source);
                if (imageSource == nullptr || imageSource->dwordCount != 8 || imageSource->tableColumn.has_value()) {
                    continue;
                }
                bool alias = true;
                for (std::uint32_t dword = 0; dword < 4; dword++) {
                    alias = alias && EquivalentValue(m_program.Resources(), bufferSource->dwords[dword], imageSource->dwords[dword]);
                }
                if (alias) {
                    buffer.imageAlias = image;
                    break;
                }
            }
        }
    }

    IrProgram& m_program;
    ShaderInfo m_info;
    IrBuilder m_builder;
    std::vector<DescriptorSource> m_sources;
    std::unordered_map<const IrValue*, const IrBlock*> m_readOrigins;
    std::vector<std::vector<std::uint32_t>> m_blockSources;
    std::vector<std::vector<std::uint32_t>> m_blockImages;
    std::uint32_t m_currentBlock = std::numeric_limits<std::uint32_t>::max();
    std::vector<HandlePatch> m_handlePatches;
    std::vector<MemoryPatch> m_memoryPatches;
    // APS5_TRACE_BDA: address accesses seen by Collect (the first bdaTraceLimit are printed).
    unsigned m_bdaTraces = 0;
    std::unordered_map<std::uint32_t, std::vector<const IrValue*>> m_memoryUsers;
    bool m_memoryUsersIndexed = false;
    std::vector<TableColumnPlan> m_tableColumns;
    std::vector<const IrValue*> m_tablePlanningReads;
    std::vector<std::uint32_t> m_tablePlanningMemory;
    std::vector<const IrValue*> m_unselectedHandles;
    struct EdgeSelectorEntry {
        IrBlock* block = nullptr;
        std::vector<bool> edges;
        IrValue* value = nullptr;
    };
    std::vector<EdgeSelectorEntry> m_edgeSelectors;
    std::unordered_map<const IrValue*, std::string> m_splitFailures;
    bool m_foldedWeb = false;
};

}

void ResourceTracker::Track(IrProgram& program) const {
    Tracker(program).Run();
}

}
