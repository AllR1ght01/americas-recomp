#include "ControlFlow/Structurizer.hpp"
#include "ControlFlow/ControlFlowHelpers.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <tuple>
#include <stdexcept>

#include <chrono>
#include <cstdio>
#include <string>

namespace ShaderRecompiler {

// XBOX-FORK: per-thread request to use flag structurization (see Recompile's retry on IR verification failure).
static thread_local bool g_forcedFlagStructurization = false;
void SetForcedFlagStructurization(bool value) { g_forcedFlagStructurization = value; }
bool ForcedFlagStructurization() { return g_forcedFlagStructurization; }

// XBOX-FORK: optional profiling, enabled with XBOX_STRUCT_PROF=1 (prints to stderr at exit).
namespace StructProf {
struct Entry { double ms = 0; unsigned long calls = 0; };
inline bool enabled() { static const bool e = std::getenv("XBOX_STRUCT_PROF") != nullptr; return e; }
inline std::map<std::string, Entry>& table() { static std::map<std::string, Entry> t; return t; }
inline void report() {
    for (const auto& [k, v] : table()) std::fprintf(stderr, "  %-28s %10.1f ms %8lu calls\n", k.c_str(), v.ms, v.calls);
}
struct Scope {
    const char* name; std::chrono::steady_clock::time_point t;
    explicit Scope(const char* n) : name(n), t(std::chrono::steady_clock::now()) {}
    ~Scope() {
        if (!enabled()) return;
        (void)table(); static const bool reg = (std::atexit(report), true); (void)reg;
        auto& e = table()[name]; e.calls++;
        e.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    }
};
}

// XBOX-FORK: the shared-merge split budget was 4x the block count. Failing attempts always ran all of it (the graph
// grows ~1 block per split) before the next attempt started. On a 4,147 shader corpus no completed split loop needed
// more than 0.72x (large graphs: <0.37x), so the cascade first runs with a fast budget and, only if that fails, reruns
// with the original 400%. XBOX_SPLIT_PCT overrides the fast budget (400 = original behaviour).
namespace StructBudget {
inline thread_local std::uint32_t percent = 400u;
inline thread_local bool skippedRelaxedReturnJoin = false;
inline std::uint32_t fastPercent() {
    static const std::uint32_t value = [] {
        const char* text = std::getenv("XBOX_SPLIT_PCT");
        const long parsed = text != nullptr ? std::strtol(text, nullptr, 10) : 60;
        return static_cast<std::uint32_t>(parsed < 1 ? 60 : parsed);
    }();
    return value;
}
}

namespace {

std::vector<std::uint32_t> allBlockIds(std::uint32_t count) {
    std::vector<std::uint32_t> ids;
    ids.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ids.push_back(i);
    }
    return ids;
}

std::vector<std::uint32_t> reversePostOrder(const ControlFlowGraph& graph);

std::vector<std::uint32_t> analysisOrder(const ControlFlowGraph& graph, bool successorsFirst) {
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    std::vector<std::uint32_t> order;
    std::vector<bool> placed(count, false);
    if (graph.entryBlock < count) {
        order = reversePostOrder(graph);
        if (successorsFirst) {
            std::reverse(order.begin(), order.end());
        }
        for (const auto id : order) {
            placed[id] = true;
        }
    }
    for (std::uint32_t id = 0; id < count; ++id) {
        if (!placed[id]) {
            order.push_back(id);
        }
    }
    return order;
}

bool inPlaceIntersections() {
    static const bool enabled = std::getenv("APS5_NO_PERF_DOM_INTERSECTION") == nullptr;
    return enabled;
}

bool dominatorTrees() {
    static const bool enabled = std::getenv("APS5_NO_PERF_DOMINATOR_TREE") == nullptr;
    return enabled;
}

template <typename IncomingOf, typename OutgoingOf>
bool assignTreeDominators(std::vector<BasicBlock>& blocks, const std::vector<std::uint32_t>& roots, IncomingOf incomingOf, OutgoingOf outgoingOf, ControlFlowGraph::TreeIndex& tree, std::uint32_t BasicBlock::*parent = nullptr) {
    const auto count = static_cast<std::uint32_t>(blocks.size());
    const auto virtualRoot = count;
    thread_local std::vector<std::uint8_t> isRoot;
    thread_local std::vector<std::uint32_t> postOrder;
    thread_local std::vector<std::uint32_t> order;
    thread_local std::vector<std::pair<std::uint32_t, std::size_t>> stack;
    thread_local std::vector<std::uint32_t> immediate;
    thread_local std::vector<std::uint32_t> firstChild;
    thread_local std::vector<std::uint32_t> nextSibling;
    thread_local std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> dfs;
    isRoot.assign(count, 0u);
    for (const auto root : roots) {
        isRoot[root] = 1u;
    }
    postOrder.assign(count + 1u, InvalidControlFlowId);
    order.clear();
    order.reserve(count + 1u);
    stack.clear();
    stack.reserve(count + 1u);
    stack.emplace_back(virtualRoot, 0u);
    postOrder[virtualRoot] = 0;
    while (!stack.empty()) {
        auto& [node, next] = stack.back();
        const auto& edges = node == virtualRoot ? roots : outgoingOf(node);
        if (next < edges.size()) {
            const auto target = edges[next++];
            if (node != virtualRoot && isRoot[target]) continue;
            if (postOrder[target] == InvalidControlFlowId) {
                postOrder[target] = 0;
                stack.emplace_back(target, 0u);
            }
            continue;
        }
        postOrder[node] = static_cast<std::uint32_t>(order.size());
        order.push_back(node);
        stack.pop_back();
    }
    if (order.size() != count + 1u && (parent == nullptr || order.size() <= 1u)) {
        tree.valid = false;
        return false;
    }
    immediate.assign(count + 1u, InvalidControlFlowId);
    immediate[virtualRoot] = virtualRoot;
    const auto intersect = [&](std::uint32_t first, std::uint32_t second) {
        while (first != second) {
            while (postOrder[first] < postOrder[second]) first = immediate[first];
            while (postOrder[second] < postOrder[first]) second = immediate[second];
        }
        return first;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto position = order.size() - 1u; position-- > 0u;) {
            const auto node = order[position];
            auto next = isRoot[node] ? virtualRoot : InvalidControlFlowId;
            if (!isRoot[node]) {
                for (const auto from : incomingOf(node)) {
                    if (immediate[from] != InvalidControlFlowId) next = next == InvalidControlFlowId ? from : intersect(from, next);
                }
            }
            if (immediate[node] != next) {
                immediate[node] = next;
                changed = true;
            }
        }
    }
    tree.tin.assign(count, InvalidControlFlowId);
    tree.tout.assign(count, 0u);
    tree.depth.assign(count, count);
    tree.parent.assign(count, InvalidControlFlowId);
    firstChild.assign(count + 1u, InvalidControlFlowId);
    nextSibling.assign(count, InvalidControlFlowId);
    for (auto position = order.size() - 1u; position-- > 0u;) {
        const auto node = order[position];
        const auto imm = immediate[node];
        nextSibling[node] = firstChild[imm];
        firstChild[imm] = node;
        const auto resolved = imm == virtualRoot ? node : imm;
        tree.parent[node] = resolved;
        if (parent != nullptr) blocks[node].*parent = resolved;
    }
    if (order.size() != count + 1u && parent != nullptr) {
        for (std::uint32_t id = 0; id < count; ++id) {
            if (postOrder[id] == InvalidControlFlowId) blocks[id].*parent = InvalidControlFlowId;
        }
    }
    std::uint32_t clock = 0u;
    dfs.clear();
    dfs.reserve(order.size());
    dfs.emplace_back(virtualRoot, firstChild[virtualRoot], 0u);
    while (!dfs.empty()) {
        auto& [node, child, depth] = dfs.back();
        if (child != InvalidControlFlowId) {
            const auto next = child;
            child = nextSibling[next];
            tree.tin[next] = clock++;
            tree.depth[next] = depth + 1u;
            dfs.emplace_back(next, firstChild[next], depth + 1u);
            continue;
        }
        if (node != virtualRoot) {
            tree.tout[node] = clock;
        }
        dfs.pop_back();
    }
    tree.valid = true;
    return true;
}

void materializeTreeSets(std::vector<BasicBlock>& blocks, const ControlFlowGraph::TreeIndex& tree, std::vector<std::uint32_t> BasicBlock::*sets) {
    if (!tree.valid || blocks.size() != tree.tin.size()) return;
    const auto count = static_cast<std::uint32_t>(blocks.size());
    constexpr std::uint64_t maxEntries = 4ull * 1024ull * 1024ull;
    std::uint64_t totalEntries = 0u;
    for (std::uint32_t id = 0; id < count; ++id) {
        totalEntries += tree.tin[id] == InvalidControlFlowId ? count : tree.depth[id];
        if (totalEntries > maxEntries) {
            for (auto& block : blocks) (block.*sets).clear();
            return;
        }
    }
    std::vector<std::uint32_t> order;
    order.reserve(count);
    for (std::uint32_t id = 0; id < count; ++id) {
        if (tree.tin[id] != InvalidControlFlowId) order.push_back(id);
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return tree.tin[a] < tree.tin[b]; });
    for (const auto node : order) {
        auto& set = blocks[node].*sets;
        if (tree.parent[node] == node) {
            set.assign(1u, node);
        } else {
            const auto& parentSet = blocks[tree.parent[node]].*sets;
            const auto split = static_cast<std::size_t>(std::upper_bound(parentSet.begin(), parentSet.end(), node) - parentSet.begin());
            set.resize(parentSet.size() + 1u);
            std::copy_n(parentSet.begin(), split, set.begin());
            set[split] = node;
            std::copy(parentSet.begin() + static_cast<std::ptrdiff_t>(split), parentSet.end(), set.begin() + static_cast<std::ptrdiff_t>(split) + 1);
        }
    }
    if (order.size() != count) {
        const auto all = allBlockIds(count);
        for (std::uint32_t id = 0; id < count; ++id) {
            if (tree.tin[id] == InvalidControlFlowId) blocks[id].*sets = all;
        }
    }
}

struct BlockSet {
    std::vector<std::uint64_t> words;

    static std::size_t wordCount(std::uint32_t count) {
        return (static_cast<std::size_t>(count) + 63u) / 64u;
    }

    static BlockSet filled(std::uint32_t count, std::uint64_t fill) {
        BlockSet set;
        set.words.assign(wordCount(count), fill);
        return set;
    }

    static BlockSet none(std::uint32_t count) {
        return filled(count, 0);
    }

    static BlockSet all(std::uint32_t count) {
        auto set = filled(count, ~std::uint64_t{0});
        if (const auto tail = count % 64u) set.words.back() &= (std::uint64_t{1} << tail) - 1u;
        return set;
    }

    static BlockSet only(std::uint32_t count, std::uint32_t id) {
        auto set = none(count);
        set.add(id);
        return set;
    }

    static BlockSet fromIds(std::uint32_t count, const std::vector<std::uint32_t>& ids) {
        auto set = none(count);
        for (const auto id : ids) set.add(id);
        return set;
    }

    void add(std::uint32_t id) {
        words[static_cast<std::size_t>(id) / 64u] |= std::uint64_t{1} << (id % 64u);
    }

    bool has(std::uint32_t id) const {
        return (words[static_cast<std::size_t>(id) / 64u] >> (id % 64u) & 1u) != 0u;
    }

    void intersect(const BlockSet& other) {
        for (std::size_t i = 0; i < words.size(); ++i) words[i] &= other.words[i];
    }

    bool operator==(const BlockSet& other) const {
        return words == other.words;
    }

    std::vector<std::uint32_t> ids() const {
        std::vector<std::uint32_t> out;
        for (std::size_t word = 0; word < words.size(); ++word) {
            auto bits = words[word];
            while (bits != 0) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(bits));
                out.push_back(static_cast<std::uint32_t>(word * 64u) + bit);
                bits &= bits - 1u;
            }
        }
        return out;
    }
};

bool replaceSet(BlockSet& destination, BlockSet&& next) {
    if (next == destination) return false;
    destination = std::move(next);
    return true;
}

std::vector<std::vector<std::uint32_t>> analysisVectors(const std::vector<BlockSet>& sets) {
    std::vector<std::vector<std::uint32_t>> vectors(sets.size());
    for (std::size_t i = 0; i < sets.size(); ++i) vectors[i] = sets[i].ids();
    return vectors;
}

void intersectAnalysisSet(std::vector<std::uint32_t>& values, const std::vector<std::uint32_t>& other) {
    if (!inPlaceIntersections()) {
        values = intersectSorted(values, other);
        return;
    }
    std::size_t left = 0;
    std::size_t right = 0;
    std::size_t count = 0;
    while (left < values.size() && right < other.size()) {
        if (values[left] < other[right]) {
            ++left;
        } else if (other[right] < values[left]) {
            ++right;
        } else {
            values[count++] = values[left++];
            ++right;
        }
    }
    values.resize(count);
}

void replaceAnalysisSet(std::vector<std::uint32_t>& destination, std::vector<std::uint32_t>&& values) {
    if (inPlaceIntersections() && values.capacity() - values.size() > values.size()) {
        std::vector<std::uint32_t>(values).swap(values);
    }
    destination = std::move(values);
}

void addSortedUnique(std::vector<std::uint32_t>& values, std::uint32_t value) {
    const auto position = std::lower_bound(values.begin(), values.end(), value);
    if (position == values.end() || *position != value) {
        values.insert(position, value);
    }
}

bool containsSorted(const std::vector<std::uint32_t>& values, std::uint32_t value) {
    return std::binary_search(values.begin(), values.end(), value);
}

bool replaceValue(std::vector<std::uint32_t>& values, std::uint32_t oldValue, std::uint32_t newValue) {
    bool changed = false;
    for (auto& value : values) {
        if (value == oldValue) {
            value = newValue;
            changed = true;
        }
    }
    if (changed) {
        sortUnique(values);
    }
    return changed;
}

bool removeValue(std::vector<std::uint32_t>& values, std::uint32_t value) {
    const auto oldSize = values.size();
    values.erase(std::remove(values.begin(), values.end(), value), values.end());
    return values.size() != oldSize;
}

bool replaceTerminatorTarget(Terminator& terminator, std::uint32_t oldValue, std::uint32_t newValue) {
    bool changed = false;
    if (terminator.trueBlock == oldValue) {
        terminator.trueBlock = newValue;
        changed = true;
    }
    if (terminator.falseBlock == oldValue) {
        terminator.falseBlock = newValue;
        changed = true;
    }
    return changed;
}

std::vector<std::uint32_t> applyBlockOrder(ControlFlowGraph& graph, std::vector<BasicBlock> blocks) {
    std::vector<std::uint32_t> idMap(blocks.size(), InvalidControlFlowId);
    for (std::uint32_t i = 0; i < blocks.size(); ++i) {
        idMap[blocks[i].id] = i;
    }

    graph.blocks = std::move(blocks);
    graph.layoutOrder.clear();
    graph.domTree.valid = false;
    graph.pdomTree.valid = false;
    graph.entryBlock = remapId(graph.entryBlock, idMap);
    for (auto& block : graph.blocks) {
        block.id = remapId(block.id, idMap);
        block.immediatePostDominator = remapId(block.immediatePostDominator, idMap);
        remapIds(block.predecessors, idMap);
        remapIds(block.successors, idMap);
        remapIds(block.dominators, idMap);
        remapIds(block.postDominators, idMap);
        block.terminator.trueBlock = remapId(block.terminator.trueBlock, idMap);
        block.terminator.falseBlock = remapId(block.terminator.falseBlock, idMap);
        block.terminator.mergeBlock = remapId(block.terminator.mergeBlock, idMap);
        block.terminator.continueBlock = remapId(block.terminator.continueBlock, idMap);
        for (auto& target : block.terminator.indirectTargets) {
            target = remapId(target, idMap);
        }
        for (auto& target : block.terminator.indirectSelectorTargets) {
            target = remapId(target, idMap);
        }
    }

    return idMap;
}

std::uint32_t moveBlockBefore(ControlFlowGraph& graph, std::uint32_t blockId, std::uint32_t beforeId) {
    if (blockId == beforeId || blockId >= graph.blocks.size() || beforeId >= graph.blocks.size()) {
        return blockId;
    }

    if (!graph.layoutOrder.empty()) {
        // XBOX-FORK: stable-id mode: only the layout order changes, no block is moved or renumbered.
        auto& order = graph.layoutOrder;
        order.erase(std::find(order.begin(), order.end(), blockId));
        order.insert(std::find(order.begin(), order.end(), beforeId), blockId);
        return blockId;
    }

    const auto blockPos = blockId;
    const auto beforePos = beforeId;
    std::vector<BasicBlock> oldBlocks = std::move(graph.blocks);
    std::vector<BasicBlock> newBlocks;
    newBlocks.reserve(oldBlocks.size());

    for (std::uint32_t i = 0; i < oldBlocks.size(); ++i) {
        if (i == beforePos) {
            newBlocks.push_back(std::move(oldBlocks[blockPos]));
        }
        if (i != blockPos) {
            newBlocks.push_back(std::move(oldBlocks[i]));
        }
    }

    const auto idMap = applyBlockOrder(graph, std::move(newBlocks));
    return remapId(blockId, idMap);
}

// XBOX-FORK: leave stable-id mode: lay the blocks out in `layoutOrder` (renumbering them once). Analyses must be recomputed after.
void materializeLayout(ControlFlowGraph& graph) {
    if (graph.layoutOrder.empty()) {
        return;
    }
    std::vector<bool> placed(graph.blocks.size(), false);
    std::vector<BasicBlock> ordered;
    ordered.reserve(graph.blocks.size());
    for (const auto blockId : graph.layoutOrder) {
        if (blockId < graph.blocks.size() && !placed[blockId]) {
            placed[blockId] = true;
            ordered.push_back(std::move(graph.blocks[blockId]));
        }
    }
    for (std::uint32_t blockId = 0; blockId < graph.blocks.size(); ++blockId) {
        if (!placed[blockId]) {
            ordered.push_back(std::move(graph.blocks[blockId]));
        }
    }
    applyBlockOrder(graph, std::move(ordered));
}

std::vector<std::uint32_t> dominatedBlocks(const ControlFlowGraph& graph, std::uint32_t headerBlock, std::uint32_t stopBlock = InvalidControlFlowId) {
    std::vector<std::uint32_t> blocks;
    if (headerBlock == stopBlock || !graph.Dominates(headerBlock, headerBlock)) {
        return blocks;
    }
    std::vector<std::uint32_t> stack = {headerBlock};
    std::vector<bool> visited(graph.blocks.size());

    while (!stack.empty()) {
        const auto blockId = stack.back();
        stack.pop_back();
        if (blockId >= visited.size()) {
            visited.resize(blockId + 1);
        }
        if (visited[blockId]) {
            continue;
        }

        const auto& block = graph.FindBlock(blockId);
        visited[blockId] = true;
        blocks.push_back(blockId);
        for (const auto successor : block.successors) {
            if (successor != stopBlock && (successor >= visited.size() || !visited[successor]) && graph.Dominates(headerBlock, successor)) {
                stack.push_back(successor);
            }
        }
    }

    sortUnique(blocks);
    return blocks;
}

std::uint32_t appendSyntheticBranchBlock(ControlFlowGraph& graph, std::uint32_t target) {
    const auto& targetBlock = graph.FindBlock(target);

    BasicBlock block;
    block.id = static_cast<std::uint32_t>(graph.blocks.size());
    block.startProgramCounter = targetBlock.startProgramCounter;
    block.endProgramCounter = block.startProgramCounter;
    block.instructionBegin = targetBlock.instructionBegin;
    block.instructionEnd = block.instructionBegin;
    block.successors = {target};
    block.terminator.kind = TerminatorKind::Branch;
    block.terminator.condition = BranchCondition::Always;
    block.terminator.trueBlock = target;
    graph.blocks.push_back(std::move(block));
    if (!graph.layoutOrder.empty()) graph.layoutOrder.push_back(graph.blocks.back().id);
    return graph.blocks.back().id;
}

bool isolateSemanticLoopHeader(ControlFlowGraph& graph, std::uint32_t oldHeader, const std::function<void(ControlFlowGraph&)>& recompute) {
    const auto& header = graph.FindBlock(oldHeader);
    if (header.instructionBegin == header.instructionEnd) {
        return false;
    }

    const auto predecessors = header.predecessors;
    const auto newHeader = appendSyntheticBranchBlock(graph, oldHeader);
    for (const auto predecessor : predecessors) {
        auto& block = graph.FindBlock(predecessor);
        replaceValue(block.successors, oldHeader, newHeader);
        replaceTerminatorTarget(block.terminator, oldHeader, newHeader);
    }
    if (graph.entryBlock == oldHeader) {
        graph.entryBlock = newHeader;
    }
    moveBlockBefore(graph, newHeader, oldHeader);
    rebuildPredecessors(graph);
    recompute(graph);
    return true;
}

const NaturalLoop* findInnermostContainingLoop(const ControlFlowGraph& graph, std::uint32_t blockId) {
    const NaturalLoop* innermost = nullptr;
    for (const auto& loop : graph.naturalLoops) {
        if (containsSorted(loop.blocks, blockId) && (innermost == nullptr || loop.blocks.size() < innermost->blocks.size())) {
            innermost = &loop;
        }
    }
    return innermost;
}

bool isInsideLoopConstruct(const ControlFlowGraph& graph, const NaturalLoop& loop, std::uint32_t blockId) {
    return blockId != InvalidControlFlowId && blockId != loop.mergeBlock && blockId != loop.continueBlock && graph.Dominates(loop.headerBlock, blockId) && !graph.Dominates(loop.mergeBlock, blockId);
}

bool isLoopControlGateway(const ControlFlowGraph& graph, const NaturalLoop& loop, std::uint32_t blockId) {
    const auto& block = graph.FindBlock(blockId);
    if (block.terminator.kind != TerminatorKind::ConditionalBranch) {
        return false;
    }
    const auto isControlTarget = [&](std::uint32_t target) {
        return target == loop.mergeBlock || target == loop.continueBlock;
    };
    return isControlTarget(block.terminator.trueBlock) && isControlTarget(block.terminator.falseBlock);
}

bool hasLinearPathToTerminal(const ControlFlowGraph& graph, std::uint32_t start) {
    std::vector<bool> visited(graph.blocks.size(), false);
    for (auto blockId = start;;) {
        if (visited[blockId]) {
            return false;
        }
        const auto& block = graph.FindBlock(blockId);
        if (block.successors.empty()) {
            return true;
        }
        if (block.successors.size() != 1) {
            return false;
        }
        visited[blockId] = true;
        blockId = block.successors.front();
    }
}

bool isEnclosingLinearExit(const ControlFlowGraph& graph, std::uint32_t header, std::uint32_t blockId) {
    if (!hasLinearPathToTerminal(graph, blockId)) {
        return false;
    }
    const BasicBlock* block = &graph.FindBlock(blockId);
    while (graph.Dominates(header, block->id) && block->successors.size() == 1u) {
        block = &graph.FindBlock(block->successors.front());
    }
    if (graph.Dominates(header, block->id) || block->predecessors.empty()) {
        return false;
    }
    return std::all_of(block->predecessors.begin(), block->predecessors.end(), [&](std::uint32_t predecessor) {
        return graph.Dominates(header, predecessor) || graph.Dominates(predecessor, header);
    });
}

// XBOX-FORK: post-dominator dataflow restricted to `domain` (blocks that can reach `root`), on dense indices.
// The original allocated count x count bits per call and converted every set to an id list; the fixpoint is the same
// because every post-dominator set of a domain block is a subset of the domain. Output keeps the original shape
// (indexed by block id, filled for domain blocks only).
std::vector<std::vector<std::uint32_t>> densePostDominators(const ControlFlowGraph& graph, const std::vector<std::uint32_t>& order, const std::vector<bool>& inDomain, const std::vector<std::uint32_t>& domain, std::uint32_t root) {
    StructProf::Scope prof("h:densePostDominators");
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    const auto size = static_cast<std::uint32_t>(domain.size());
    std::vector<std::uint32_t> dense(count, 0xffffffffu);
    for (std::uint32_t i = 0; i < size; ++i) dense[domain[i]] = i;
    const auto universe = BlockSet::all(size);
    std::vector<BlockSet> sets(size, universe);
    sets[dense[root]] = BlockSet::only(size, dense[root]);
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto blockId : order) {
            if (!inDomain[blockId] || blockId == root) continue;
            auto next = universe;
            for (const auto successor : graph.FindBlock(blockId).successors) {
                if (inDomain[successor]) next.intersect(sets[dense[successor]]);
            }
            next.add(dense[blockId]);
            changed = replaceSet(sets[dense[blockId]], std::move(next)) || changed;
        }
    }
    std::vector<std::vector<std::uint32_t>> result(count);
    for (std::uint32_t i = 0; i < size; ++i) {
        auto ids = sets[i].ids();
        for (auto& id : ids) id = domain[id];
        result[domain[i]] = std::move(ids);
    }
    return result;
}

bool canReachBefore(const ControlFlowGraph& graph, std::uint32_t start, std::uint32_t target, std::uint32_t stop) {
    std::vector<std::uint32_t> pending = {start};
    std::vector<bool> visited(graph.blocks.size(), false);
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (blockId == target) {
            return true;
        }
        if (blockId == stop || visited[blockId]) {
            continue;
        }
        visited[blockId] = true;
        const auto& block = graph.FindBlock(blockId);
        pending.insert(pending.end(), block.successors.begin(), block.successors.end());
    }
    return false;
}

bool reachesWithinIteration(const ControlFlowGraph& graph, const NaturalLoop& loop, std::uint32_t start, std::uint32_t target) {
    if (start == target) return true;
    if (start == loop.continueBlock || start == loop.mergeBlock) return false;
    std::vector<std::uint32_t> pending = {start};
    std::vector<bool> visited(graph.blocks.size(), false);
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (blockId == target) {
            return true;
        }
        if (blockId == loop.continueBlock || blockId == loop.mergeBlock || visited[blockId]) {
            continue;
        }
        visited[blockId] = true;
        const auto& block = graph.FindBlock(blockId);
        pending.insert(pending.end(), block.successors.begin(), block.successors.end());
    }
    return false;
}

std::uint32_t joinWithinIteration(const ControlFlowGraph& graph, const NaturalLoop& loop, std::uint32_t trueTarget, std::uint32_t falseTarget, std::uint32_t globalMerge) {
    StructProf::Scope prof_joinWithinIteration("h:joinWithinIteration");
    std::vector<bool> reached(graph.blocks.size(), false);
    std::vector<std::uint32_t> pending = {trueTarget, falseTarget};
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (blockId == globalMerge || reached[blockId] || !isInsideLoopConstruct(graph, loop, blockId)) {
            continue;
        }
        reached[blockId] = true;
        const auto& successors = graph.FindBlock(blockId).successors;
        pending.insert(pending.end(), successors.begin(), successors.end());
    }
    std::vector<bool> joins(graph.blocks.size(), false);
    std::vector<std::uint32_t> joining = {globalMerge};
    joins[globalMerge] = true;
    for (std::size_t index = 0; index < joining.size(); ++index) {
        for (const auto predecessor : graph.FindBlock(joining[index]).predecessors) {
            if (reached[predecessor] && !joins[predecessor]) {
                joins[predecessor] = true;
                joining.push_back(predecessor);
            }
        }
    }
    sortUnique(joining);
    if (!joins[trueTarget] || !joins[falseTarget]) {
        return globalMerge;
    }
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    std::vector<std::vector<std::uint32_t>> postDominators(count);
    const auto order = analysisOrder(graph, true);
    if (inPlaceIntersections()) {
        postDominators = densePostDominators(graph, order, joins, joining, globalMerge);
    } else {
        for (const auto blockId : joining) postDominators[blockId] = blockId == globalMerge ? std::vector<std::uint32_t>{globalMerge} : joining;
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto blockId : order) {
                if (!joins[blockId] || blockId == globalMerge) continue;
                auto next = joining;
                for (const auto successor : graph.FindBlock(blockId).successors) {
                    if (joins[successor]) next = intersectSorted(next, postDominators[successor]);
                }
                addSortedUnique(next, blockId);
                if (next != postDominators[blockId]) {
                    postDominators[blockId] = std::move(next);
                    changed = true;
                }
            }
        }
    }
    const auto common = intersectSorted(postDominators[trueTarget], postDominators[falseTarget]);
    const auto merge = std::find_if(common.begin(), common.end(), [&](std::uint32_t candidate) {
        return std::all_of(common.begin(), common.end(), [&](std::uint32_t other) { return contains(postDominators[candidate], other); });
    });
    return merge == common.end() ? globalMerge : *merge;
}

std::vector<std::uint32_t> linearTail(const ControlFlowGraph& graph, std::uint32_t start) {
    std::vector<std::uint32_t> tail;
    for (auto blockId = start; !contains(tail, blockId);) {
        tail.push_back(blockId);
        const auto& block = graph.FindBlock(blockId);
        if (block.successors.size() != 1u) {
            break;
        }
        blockId = block.successors.front();
    }
    return tail;
}

std::uint32_t mergeBesideReturns(const ControlFlowGraph& graph, const BasicBlock& header) {
    StructProf::Scope prof_mergeBesideReturns("h:mergeBesideReturns");
    const auto reach = [&](std::uint32_t start) {
        std::vector<std::uint32_t> reached;
        std::vector<std::uint32_t> pending = {start};
        while (!pending.empty()) {
            const auto blockId = pending.back();
            pending.pop_back();
            if (blockId == header.id || contains(reached, blockId)) {
                continue;
            }
            reached.push_back(blockId);
            const auto& successors = graph.FindBlock(blockId).successors;
            pending.insert(pending.end(), successors.begin(), successors.end());
        }
        sortUnique(reached);
        return reached;
    };
    const auto trueReach = reach(header.terminator.trueBlock);
    const auto falseReach = reach(header.terminator.falseBlock);
    if (intersectSorted(trueReach, falseReach).empty()) {
        const auto returnsWithin = [&](const std::vector<std::uint32_t>& reached) {
            return std::all_of(reached.begin(), reached.end(), [&](std::uint32_t blockId) { return graph.Dominates(header.id, blockId); });
        };
        if (returnsWithin(trueReach) && graph.Dominates(header.id, header.terminator.falseBlock)) {
            return header.terminator.falseBlock;
        }
        if (returnsWithin(falseReach) && graph.Dominates(header.id, header.terminator.trueBlock)) {
            return header.terminator.trueBlock;
        }
    }
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    enum class Returning : std::uint8_t { Unknown, Visiting, No, Yes };
    std::vector<Returning> returning(count, Returning::Unknown);
    const auto returns = [&](std::uint32_t start) {
        std::vector<std::uint32_t> pending{start};
        while (!pending.empty()) {
            const auto blockId = pending.back();
            const auto& block = graph.FindBlock(blockId);
            if (returning[blockId] == Returning::Unknown) {
                if (block.predecessors.size() == 1u && graph.Dominates(header.id, blockId)) {
                    returning[blockId] = Returning::Visiting;
                    std::copy_if(block.successors.begin(), block.successors.end(), std::back_inserter(pending), [&](std::uint32_t successor) { return returning[successor] == Returning::Unknown; });
                    continue;
                }
                returning[blockId] = Returning::No;
            } else if (returning[blockId] == Returning::Visiting) {
                const bool privateReturn = std::all_of(block.successors.begin(), block.successors.end(), [&](std::uint32_t successor) { return returning[successor] == Returning::Yes; });
                returning[blockId] = privateReturn ? Returning::Yes : Returning::No;
            }
            pending.pop_back();
        }
        return returning[start] == Returning::Yes;
    };
    std::vector<std::vector<std::uint32_t>> kept(count);
    for (const auto& block : graph.blocks) {
        for (const auto successor : block.successors) {
            if (!returns(successor)) kept[block.id].push_back(successor);
        }
    }
    const auto order = analysisOrder(graph, true);
    std::vector<std::vector<std::uint32_t>> postDominators;
    if (inPlaceIntersections()) {
        std::vector<BlockSet> sets(count, BlockSet::all(count));
        for (const auto& block : graph.blocks) {
            if (kept[block.id].empty()) sets[block.id] = BlockSet::only(count, block.id);
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto blockId : order) {
                const auto& block = graph.blocks[blockId];
                if (kept[block.id].empty()) continue;
                auto next = sets[kept[block.id].front()];
                for (std::size_t i = 1; i < kept[block.id].size(); ++i) next.intersect(sets[kept[block.id][i]]);
                next.add(block.id);
                changed = replaceSet(sets[block.id], std::move(next)) || changed;
            }
        }
        postDominators = analysisVectors(sets);
    } else {
        postDominators.assign(count, allBlockIds(count));
        for (const auto& block : graph.blocks) {
            if (kept[block.id].empty()) postDominators[block.id] = {block.id};
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto blockId : order) {
                const auto& block = graph.blocks[blockId];
                if (kept[block.id].empty()) continue;
                auto next = postDominators[kept[block.id].front()];
                for (std::size_t i = 1; i < kept[block.id].size(); ++i) intersectAnalysisSet(next, postDominators[kept[block.id][i]]);
                addSortedUnique(next, block.id);
                if (next != postDominators[block.id]) {
                    replaceAnalysisSet(postDominators[block.id], std::move(next));
                    changed = true;
                }
            }
        }
    }
    const auto common = intersectSorted(postDominators[header.terminator.trueBlock], postDominators[header.terminator.falseBlock]);
    const auto merge = std::find_if(common.begin(), common.end(), [&](std::uint32_t candidate) {
        return std::all_of(common.begin(), common.end(), [&](std::uint32_t other) { return contains(postDominators[candidate], other); });
    });
    if (merge == common.end() || *merge == header.id) {
        return InvalidControlFlowId;
    }
    const auto inConstruct = [&](std::uint32_t blockId) {
        return blockId != *merge && graph.Dominates(header.id, blockId) && !graph.Dominates(*merge, blockId);
    };
    for (const auto& block : graph.blocks) {
        if (block.id == header.id || !inConstruct(block.id)) {
            continue;
        }
        for (const auto successor : block.successors) {
            if (successor != *merge && !inConstruct(successor)) {
                return InvalidControlFlowId;
            }
        }
    }
    if (canReachBefore(graph, *merge, header.id, InvalidControlFlowId)) {
        return InvalidControlFlowId;
    }
    for (const auto& block : graph.blocks) {
        if (block.id != header.id && inConstruct(block.id) && canReachBefore(graph, *merge, block.id, header.id)) {
            return InvalidControlFlowId;
        }
    }
    return *merge;
}

std::uint32_t iterationSelectionMerge(const ControlFlowGraph& graph, const NaturalLoop& loop, const BasicBlock& header, std::uint32_t join) {
    StructProf::Scope prof_iterationSelectionMerge("h:iterationSelectionMerge");
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    static const bool reuse = std::getenv("APS5_NO_PERF_ITERATION_SELECTION") == nullptr;
    const auto escapes = [&](std::uint32_t blockId) { return blockId == loop.continueBlock || blockId == loop.mergeBlock; };
    std::vector<bool> joins(count, false);
    std::vector<std::uint32_t> pending = {join};
    joins[join] = true;
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        for (const auto predecessor : graph.FindBlock(blockId).predecessors) {
            if (!joins[predecessor] && !escapes(predecessor)) {
                joins[predecessor] = true;
                pending.push_back(predecessor);
            }
        }
    }
    const auto trueTarget = header.terminator.trueBlock;
    const auto falseTarget = header.terminator.falseBlock;
    if (!joins[trueTarget] || !joins[falseTarget]) {
        return InvalidControlFlowId;
    }
    const auto order = analysisOrder(graph, true);
    std::vector<std::vector<std::uint32_t>> postDominators;
    if (reuse && inPlaceIntersections()) {
        std::vector<std::uint32_t> domain;
        for (std::uint32_t id = 0; id < count; ++id) {
            if (joins[id]) domain.push_back(id);
        }
        postDominators = densePostDominators(graph, order, joins, domain, join);
    } else {
        postDominators.assign(count, allBlockIds(count));
        postDominators[join] = {join};
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto blockId : order) {
                const auto& block = graph.blocks[blockId];
                if (!joins[block.id] || block.id == join) continue;
                bool seeded = !reuse;
                auto next = seeded ? allBlockIds(count) : std::vector<std::uint32_t>{};
                for (const auto successor : block.successors) {
                    if (joins[successor]) {
                        if (!seeded) {
                            next = postDominators[successor];
                            seeded = true;
                        } else if (reuse) {
                            intersectAnalysisSet(next, postDominators[successor]);
                        } else {
                            next = intersectSorted(next, postDominators[successor]);
                        }
                    }
                }
                if (!seeded) next = allBlockIds(count);
                if (reuse) addSortedUnique(next, block.id);
                else {
                    addUnique(next, block.id);
                    sortUnique(next);
                }
                if (next != postDominators[block.id]) {
                    if (reuse) replaceAnalysisSet(postDominators[block.id], std::move(next));
                    else postDominators[block.id] = std::move(next);
                    changed = true;
                }
            }
        }
    }
    const auto common = intersectSorted(postDominators[trueTarget], postDominators[falseTarget]);
    const auto merge = std::find_if(common.begin(), common.end(), [&](std::uint32_t candidate) {
        return std::all_of(common.begin(), common.end(), [&](std::uint32_t other) { return contains(postDominators[candidate], other); });
    });
    if (merge == common.end() || *merge == join || *merge == header.id || !graph.Dominates(header.id, *merge) || !isInsideLoopConstruct(graph, loop, *merge)) {
        return InvalidControlFlowId;
    }
    return *merge;
}

std::uint32_t findSelectionMerge(const ControlFlowGraph& graph, const BasicBlock& block, bool routeLeavingExits = false, bool routeSharedExits = false) {
    std::uint32_t globalMerge;
    { StructProf::Scope q1("fsm:nearestCommonPostDom"); globalMerge = graph.FindNearestCommonPostDominator(block.terminator.trueBlock, block.terminator.falseBlock); }
    const NaturalLoop* loop;
    { StructProf::Scope q2("fsm:innermostLoop"); loop = findInnermostContainingLoop(graph, block.id); }
    StructProf::Scope q0("fsm:rest");
    const auto trueTarget = block.terminator.trueBlock;
    const auto falseTarget = block.terminator.falseBlock;
    if (loop == nullptr) {
        const bool globalMergeIsExit = globalMerge == InvalidControlFlowId || graph.FindBlock(globalMerge).successors.empty();
        if (globalMergeIsExit) {
            const bool falseReachesTrue = canReachBefore(graph, falseTarget, trueTarget, globalMerge);
            const bool trueReachesFalse = canReachBefore(graph, trueTarget, falseTarget, globalMerge);
            if (falseReachesTrue != trueReachesFalse) {
                return falseReachesTrue ? trueTarget : falseTarget;
            }
            if (globalMerge == InvalidControlFlowId) {
                if (isEnclosingLinearExit(graph, block.id, trueTarget)) {
                    return trueTarget;
                }
                if (isEnclosingLinearExit(graph, block.id, falseTarget)) {
                    return falseTarget;
                }
                if (graph.Dominates(block.id, falseTarget) && hasLinearPathToTerminal(graph, trueTarget)) {
                    return falseTarget;
                }
                if (graph.Dominates(block.id, trueTarget) && hasLinearPathToTerminal(graph, falseTarget)) {
                    return trueTarget;
                }
                return mergeBesideReturns(graph, block);
            }
        }
        return globalMerge;
    }

    if (routeSharedExits && reachesWithinIteration(graph, *loop, trueTarget, loop->continueBlock) && reachesWithinIteration(graph, *loop, falseTarget, loop->continueBlock)) {
        return joinWithinIteration(graph, *loop, trueTarget, falseTarget, loop->continueBlock);
    }
    if (isLoopControlGateway(graph, *loop, trueTarget) && graph.Dominates(block.id, trueTarget) && isInsideLoopConstruct(graph, *loop, falseTarget)) {
        return trueTarget;
    }
    if (isLoopControlGateway(graph, *loop, falseTarget) && graph.Dominates(block.id, falseTarget) && isInsideLoopConstruct(graph, *loop, trueTarget)) {
        return falseTarget;
    }
    const auto joinsAt = [&](std::uint32_t join, std::uint32_t other) {
        const auto& arm = graph.FindBlock(other);
        const bool armLeavesOrJoins = arm.terminator.kind == TerminatorKind::ConditionalBranch && arm.predecessors.size() == 1u && std::all_of(arm.successors.begin(), arm.successors.end(), [&](std::uint32_t successor) {
            return successor == join || successor == loop->continueBlock || successor == loop->mergeBlock;
        });
        return armLeavesOrJoins && isInsideLoopConstruct(graph, *loop, other) && isInsideLoopConstruct(graph, *loop, join) && graph.Dominates(block.id, join);
    };
    if (joinsAt(trueTarget, falseTarget)) {
        return trueTarget;
    }
    if (joinsAt(falseTarget, trueTarget)) {
        return falseTarget;
    }
    if (globalMerge != InvalidControlFlowId) {
        const bool trueJoins = reachesWithinIteration(graph, *loop, trueTarget, globalMerge);
        const bool falseJoins = reachesWithinIteration(graph, *loop, falseTarget, globalMerge);
        if (trueJoins != falseJoins) {
            return trueJoins ? trueTarget : falseTarget;
        }
        const auto escapesAround = [&](std::uint32_t arm) {
            return canReachBefore(graph, arm, loop->continueBlock, globalMerge) || canReachBefore(graph, arm, loop->mergeBlock, globalMerge);
        };
        if (trueJoins && !graph.Dominates(block.id, globalMerge) && (escapesAround(trueTarget) || escapesAround(falseTarget))) {
            if (const auto merge = iterationSelectionMerge(graph, *loop, block, globalMerge); merge != InvalidControlFlowId) {
                return merge;
            }
        }
    }
    return globalMerge;
}

bool isInnermostLoopControlConditional(const ControlFlowGraph& graph, const BasicBlock& block) {
    if (block.terminator.kind != TerminatorKind::ConditionalBranch) {
        return false;
    }
    const auto* loop = findInnermostContainingLoop(graph, block.id);
    if (loop == nullptr || loop->mergeBlock == InvalidControlFlowId || loop->continueBlock == InvalidControlFlowId) {
        return false;
    }
    const auto trueTarget = block.terminator.trueBlock;
    const auto falseTarget = block.terminator.falseBlock;
    if (block.id == loop->continueBlock) {
        const auto isRepeatTarget = [&](std::uint32_t target) {
            return target == loop->headerBlock || target == loop->mergeBlock;
        };
        return isRepeatTarget(trueTarget) && isRepeatTarget(falseTarget);
    }
    const bool trueInBody = containsSorted(loop->blocks, trueTarget);
    const bool falseInBody = containsSorted(loop->blocks, falseTarget);
    if (trueInBody != falseInBody) {
        return true;
    }
    const auto isControlTarget = [&](std::uint32_t target) {
        return target == loop->mergeBlock || target == loop->continueBlock;
    };
    return (isControlTarget(trueTarget) && (isControlTarget(falseTarget) || isInsideLoopConstruct(graph, *loop, falseTarget))) || (isControlTarget(falseTarget) && isInsideLoopConstruct(graph, *loop, trueTarget));
}

std::uint32_t exitTailSelectionMerge(const ControlFlowGraph& graph, const BasicBlock& block, bool privateTail) {
    if (block.terminator.kind != TerminatorKind::ConditionalBranch) {
        return InvalidControlFlowId;
    }
    const auto* loop = findInnermostContainingLoop(graph, block.id);
    if (loop == nullptr || loop->mergeBlock == InvalidControlFlowId || loop->continueBlock == InvalidControlFlowId || block.id == loop->continueBlock) {
        return InvalidControlFlowId;
    }
    const auto trueTarget = block.terminator.trueBlock;
    const auto falseTarget = block.terminator.falseBlock;
    const bool trueInBody = containsSorted(loop->blocks, trueTarget);
    if (trueInBody == containsSorted(loop->blocks, falseTarget)) {
        return InvalidControlFlowId;
    }
    const auto body = trueInBody ? trueTarget : falseTarget;
    const auto tail = trueInBody ? falseTarget : trueTarget;
    if (tail == loop->mergeBlock || tail == loop->continueBlock || body == loop->continueBlock || !isInsideLoopConstruct(graph, *loop, tail) || !graph.Dominates(block.id, body) || !graph.Dominates(block.id, tail) || (privateTail && reachesWithinIteration(graph, *loop, body, tail))) {
        return InvalidControlFlowId;
    }
    std::vector<std::uint32_t> pending{tail};
    std::vector<std::uint32_t> seen;
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (contains(seen, blockId)) {
            continue;
        }
        seen.push_back(blockId);
        for (const auto successor : graph.FindBlock(blockId).successors) {
            if (successor == loop->mergeBlock) {
                continue;
            }
            if (containsSorted(loop->blocks, successor) || !graph.Dominates(tail, successor) || !isInsideLoopConstruct(graph, *loop, successor)) {
                return InvalidControlFlowId;
            }
            pending.push_back(successor);
        }
    }
    return body;
}

bool isStructuredLoopExit(const ControlFlowGraph& graph, const BasicBlock& block, bool privateTail) {
    if (!isInnermostLoopControlConditional(graph, block)) {
        return false;
    }
    if (!privateTail) {
        return true;
    }
    const auto* loop = findInnermostContainingLoop(graph, block.id);
    const auto isControlTarget = [&](std::uint32_t target) {
        return target == loop->mergeBlock || target == loop->continueBlock || (block.id == loop->continueBlock && target == loop->headerBlock);
    };
    return isControlTarget(block.terminator.trueBlock) || isControlTarget(block.terminator.falseBlock) || exitTailSelectionMerge(graph, block, true) != InvalidControlFlowId;
}

bool mergeLeavesContainingLoop(const ControlFlowGraph& graph, std::uint32_t header, std::uint32_t merge) {
    for (const auto& loop : graph.naturalLoops) {
        if (loop.headerBlock != header && isInsideLoopConstruct(graph, loop, header) && !isInsideLoopConstruct(graph, loop, merge)) {
            return true;
        }
    }
    return false;
}

template <typename InConstruct>
bool splitSharedMergeBlock(ControlFlowGraph& graph, std::uint32_t merge, InConstruct inConstruct, bool forceSplit = false) {
    if (merge == InvalidControlFlowId || merge >= graph.blocks.size() || inConstruct(merge)) {
        return false;
    }

    const auto& mergeBlock = graph.FindBlock(merge);
    std::vector<std::uint32_t> constructPredecessors;
    bool hasExternalPredecessor = false;
    for (const auto predecessor : mergeBlock.predecessors) {
        if (inConstruct(predecessor)) {
            addUnique(constructPredecessors, predecessor);
        } else {
            hasExternalPredecessor = true;
        }
    }

    if (constructPredecessors.empty() || (!forceSplit && !hasExternalPredecessor)) {
        return false;
    }

    const auto syntheticMerge = appendSyntheticBranchBlock(graph, merge);
    auto& syntheticBlock = graph.FindBlock(syntheticMerge);
    syntheticBlock.predecessors = constructPredecessors;
    sortUnique(syntheticBlock.predecessors);

    for (const auto predecessor : constructPredecessors) {
        auto& block = graph.FindBlock(predecessor);
        replaceValue(block.successors, merge, syntheticMerge);
        sortUnique(block.successors);
        replaceTerminatorTarget(block.terminator, merge, syntheticMerge);
    }

    auto& oldMerge = graph.FindBlock(merge);
    for (const auto predecessor : constructPredecessors) {
        removeValue(oldMerge.predecessors, predecessor);
    }
    addUnique(oldMerge.predecessors, syntheticMerge);
    sortUnique(oldMerge.predecessors);

    { StructProf::Scope pm("split:moveBlockBefore"); moveBlockBefore(graph, syntheticMerge, merge); }
    return true;
}

bool splitSharedMergeBlock(ControlFlowGraph& graph, std::uint32_t merge, const std::vector<std::uint32_t>& constructBlocks, bool forceSplit = false) {
    return splitSharedMergeBlock(graph, merge, [&](std::uint32_t blockId) { return containsSorted(constructBlocks, blockId); }, forceSplit);
}

bool constructsFollowDominance(const ControlFlowGraph& graph) {
    if (!inPlaceIntersections() || !dominatorTrees() || graph.entryBlock >= graph.blocks.size()) {
        return false;
    }
    if (!(graph.domTree.valid && graph.blocks.size() == graph.domTree.tin.size()) && reversePostOrder(graph).size() != graph.blocks.size()) {
        return false;
    }
    for (const auto& block : graph.blocks) {
        for (const auto successor : block.successors) {
            if (!containsSorted(graph.blocks[successor].predecessors, block.id)) {
                return false;
            }
        }
        for (const auto predecessor : block.predecessors) {
            if (!contains(graph.blocks[predecessor].successors, block.id)) {
                return false;
            }
        }
    }
    return true;
}

bool splitOneReturnJoin(ControlFlowGraph& graph, bool relaxReturnJoins) {
    for (const auto& block : graph.blocks) {
        if (block.terminator.kind != TerminatorKind::ConditionalBranch || findInnermostContainingLoop(graph, block.id) != nullptr || findSelectionMerge(graph, block) != InvalidControlFlowId) {
            continue;
        }
        const auto returns = [&](std::uint32_t target) { return graph.Dominates(block.id, target) && hasLinearPathToTerminal(graph, target); };
        const auto joinsEnclosing = [&](std::uint32_t target, bool relax) {
            const auto& predecessors = graph.FindBlock(target).predecessors;
            return !graph.Dominates(block.id, target) && (relax || std::all_of(predecessors.begin(), predecessors.end(), [&](std::uint32_t predecessor) {
                return graph.Dominates(block.id, predecessor) || graph.Dominates(predecessor, block.id);
            }));
        };
        const auto trueTarget = block.terminator.trueBlock;
        const auto falseTarget = block.terminator.falseBlock;
        const bool trueReturns = returns(trueTarget);
        const bool falseReturns = returns(falseTarget);
        const auto join = trueReturns && joinsEnclosing(falseTarget, relaxReturnJoins) ? falseTarget : falseReturns && joinsEnclosing(trueTarget, relaxReturnJoins) ? trueTarget : InvalidControlFlowId;
        if (join == InvalidControlFlowId && !relaxReturnJoins && ((trueReturns && joinsEnclosing(falseTarget, true)) || (falseReturns && joinsEnclosing(trueTarget, true)))) {
            StructBudget::skippedRelaxedReturnJoin = true;
        }
        if (join != InvalidControlFlowId && splitSharedMergeBlock(graph, join, dominatedBlocks(graph, block.id, join))) {
            return true;
        }
    }
    return false;
}

bool splitOneLoopMerge(ControlFlowGraph& graph, std::size_t& cursor) {
    // XBOX-FORK: resume at the loop that acted last (the caller rescans from 0 when a resumed scan finds nothing).
    for (std::size_t loopIndex = std::min(cursor, graph.naturalLoops.size()); loopIndex < graph.naturalLoops.size(); ++loopIndex) {
        const auto& loop = graph.naturalLoops[loopIndex];
        // XBOX-FORK: skip the O(blocks) construct walk when splitSharedMergeBlock cannot act. It returns false for an
        // invalid merge, for a merge without predecessors, and for a merge with exactly one predecessor unless forceSplit
        // is set (one predecessor cannot be both inside and outside the construct).
        if (loop.mergeBlock == InvalidControlFlowId || loop.mergeBlock >= graph.blocks.size()) {
            continue;
        }
        const auto& mergePredecessors = graph.FindBlock(loop.mergeBlock).predecessors;
        const auto predecessorCount = mergePredecessors.size();
        if (predecessorCount == 0u) {
            continue;
        }
        // A predecessor can only be inside the construct (blocks dominated by the header) if the header dominates it.
        if (std::none_of(mergePredecessors.begin(), mergePredecessors.end(), [&](std::uint32_t predecessor) { return graph.Dominates(loop.headerBlock, predecessor); })) {
            continue;
        }
        bool forceSplit = false;
        bool forceKnown = false;
        if (predecessorCount == 1u) {
            forceSplit = mergeLeavesContainingLoop(graph, loop.headerBlock, loop.mergeBlock);
            if (!forceSplit) {
                continue;
            }
            forceKnown = true;
        }
        const auto constructBlocks = dominatedBlocks(graph, loop.headerBlock, loop.mergeBlock);
        if (!forceKnown) {
            forceSplit = mergeLeavesContainingLoop(graph, loop.headerBlock, loop.mergeBlock);
        }
        if (splitSharedMergeBlock(graph, loop.mergeBlock, constructBlocks, forceSplit)) {
            cursor = loopIndex;
            return true;
        }
    }
    return false;
}

std::vector<std::uint32_t> selectionRegion(const ControlFlowGraph& graph, const BasicBlock& header, std::uint32_t merge) {
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    auto region = BlockSet::none(count);
    std::vector<std::uint32_t> pending = {header.terminator.trueBlock, header.terminator.falseBlock};
    const auto* loop = findInnermostContainingLoop(graph, header.id);
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (blockId == merge || (blockId < count && region.has(blockId)) || (loop != nullptr && (blockId == loop->mergeBlock || blockId == loop->continueBlock))) {
            continue;
        }
        const auto& block = graph.FindBlock(blockId);
        region.add(blockId);
        pending.insert(pending.end(), block.successors.begin(), block.successors.end());
    }
    return region.ids();
}

constexpr std::uint32_t CloneWordLimit = 1024u;
constexpr std::uint32_t CloneWordFloor = 256u;
constexpr std::uint32_t CloneShareDivisor = 8u;
constexpr std::uint32_t CloneBudgetDivisor = 4u;
constexpr std::uint32_t SplitGrowthFactor = 16u;
constexpr std::uint32_t SplitGrowthFloor = 64u;

struct SplitBudget {
    std::uint32_t programWords = 0;
    std::uint32_t remainingCloneWords = 0;
    std::uint32_t remainingFallbackWords = 0;
    std::uint32_t nextGotoVariable = 0;
};

void redirectEdge(BasicBlock& block, std::uint32_t oldTarget, std::uint32_t newTarget) {
    replaceValue(block.successors, oldTarget, newTarget);
    replaceTerminatorTarget(block.terminator, oldTarget, newTarget);
    for (auto& target : block.terminator.indirectTargets) {
        if (target == oldTarget) {
            target = newTarget;
        }
    }
    for (auto& target : block.terminator.indirectSelectorTargets) {
        if (target == oldTarget) {
            target = newTarget;
        }
    }
}

std::vector<std::uint32_t> reversePostOrder(const ControlFlowGraph& graph) {
    std::vector<std::uint32_t> postOrder;
    std::vector<bool> visited(graph.blocks.size(), false);
    std::vector<std::pair<std::uint32_t, std::size_t>> stack = {{graph.entryBlock, 0u}};
    visited[graph.entryBlock] = true;
    while (!stack.empty()) {
        auto& [blockId, next] = stack.back();
        const auto& successors = graph.FindBlock(blockId).successors;
        if (next < successors.size()) {
            const auto successor = successors[next++];
            if (!visited[successor]) {
                visited[successor] = true;
                stack.emplace_back(successor, 0u);
            }
            continue;
        }
        postOrder.push_back(blockId);
        stack.pop_back();
    }
    return {postOrder.rbegin(), postOrder.rend()};
}

using Edge = std::pair<std::uint32_t, std::uint32_t>;

std::vector<Edge> externalEntryEdges(const ControlFlowGraph& graph, std::uint32_t header, const std::vector<std::uint32_t>& region) {
    std::vector<Edge> edges;
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    const auto inRegion = BlockSet::fromIds(count, region);
    for (const auto member : region) {
        for (const auto predecessor : graph.FindBlock(member).predecessors) {
            if (predecessor == header || (predecessor < count && inRegion.has(predecessor))) {
                continue;
            }
            if (graph.Dominates(member, predecessor)) {
                throw std::runtime_error("selection header block " + std::to_string(header) + " region block " + std::to_string(member) + " is entered by a back edge from block " + std::to_string(predecessor));
            }
            edges.emplace_back(predecessor, member);
        }
    }
    return edges;
}

std::vector<std::uint32_t> reachableWithin(const ControlFlowGraph& graph, const std::vector<Edge>& entryEdges, const std::vector<std::uint32_t>& region) {
    std::vector<std::uint32_t> reached;
    std::vector<bool> isReached(graph.blocks.size(), false);
    std::vector<std::uint32_t> pending;
    for (const auto& edge : entryEdges) {
        pending.push_back(edge.second);
    }
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (isReached[blockId]) {
            continue;
        }
        isReached[blockId] = true;
        reached.push_back(blockId);
        for (const auto successor : graph.FindBlock(blockId).successors) {
            if (containsSorted(region, successor)) {
                pending.push_back(successor);
            }
        }
    }
    sortUnique(reached);
    return reached;
}

std::uint32_t estimatedSpirvWords(const ControlFlowGraph& graph, const std::vector<std::uint32_t>& blocks) {
    std::uint32_t words = 0;
    for (const auto blockId : blocks) {
        words += graph.FindBlock(blockId).estimatedSpirvWords;
    }
    return words;
}

std::string cloneCostRefusal(const SplitBudget& budget, std::uint32_t words) {
    const auto shareLimit = std::max(CloneWordFloor, budget.programWords / CloneShareDivisor);
    const auto prefix = "cloning an estimated " + std::to_string(words) + " SPIR-V words exceeds ";
    if (words > CloneWordLimit) {
        return prefix + "the clone limit of " + std::to_string(CloneWordLimit);
    }
    if (words > shareLimit) {
        return prefix + std::to_string(shareLimit) + ", the share of the program's " + std::to_string(budget.programWords) + " a clone may take";
    }
    if (words > budget.remainingCloneWords) {
        return prefix + "the remaining clone budget of " + std::to_string(budget.remainingCloneWords);
    }
    return {};
}

std::string cloneRefusal(const ControlFlowGraph& graph, std::uint32_t header, const std::vector<std::uint32_t>& blocks) {
    for (const auto blockId : blocks) {
        const auto& block = graph.FindBlock(blockId);
        if (graph.Dominates(blockId, header)) {
            return "the region wraps around to dominating block " + std::to_string(blockId);
        }
        if (block.terminator.kind == TerminatorKind::IndirectBranch || block.terminator.kind == TerminatorKind::Unsupported) {
            return "block " + std::to_string(blockId) + " ends in an indirect or unsupported branch";
        }
    }
    for (const auto& loop : graph.naturalLoops) {
        const bool headerCloned = contains(blocks, loop.headerBlock);
        for (const auto member : loop.blocks) {
            if (headerCloned && !contains(blocks, member)) {
                return "loop " + std::to_string(loop.headerBlock) + " would be cloned without its body block " + std::to_string(member);
            }
        }
        if (!headerCloned && contains(blocks, loop.continueBlock)) {
            return "the continue block " + std::to_string(loop.continueBlock) + " of loop " + std::to_string(loop.headerBlock) + " would be cloned without its header";
        }
    }
    return {};
}

void cloneBlocks(ControlFlowGraph& graph, const std::vector<std::uint32_t>& blocks, const std::vector<Edge>& entryEdges) {
    std::map<std::uint32_t, std::uint32_t> cloneOf;
    for (const auto blockId : blocks) {
        cloneOf.emplace(blockId, static_cast<std::uint32_t>(graph.blocks.size() + cloneOf.size()));
    }
    std::vector<BasicBlock> clones;
    clones.reserve(blocks.size());
    for (const auto blockId : blocks) {
        BasicBlock copy = graph.FindBlock(blockId);
        copy.id = cloneOf.at(blockId);
        copy.predecessors.clear();
        copy.dominators.clear();
        copy.postDominators.clear();
        copy.immediatePostDominator = InvalidControlFlowId;
        for (const auto& [original, clone] : cloneOf) {
            redirectEdge(copy, original, clone);
        }
        clones.push_back(std::move(copy));
    }
    for (auto& clone : clones) {
        graph.blocks.push_back(std::move(clone));
    }
    for (const auto& [predecessor, entry] : entryEdges) {
        redirectEdge(graph.FindBlock(predecessor), entry, cloneOf.at(entry));
    }
    rebuildPredecessors(graph);

    const auto firstClone = static_cast<std::uint32_t>(graph.blocks.size() - blocks.size());
    if (!graph.layoutOrder.empty()) {
        // XBOX-FORK: stable-id mode: the clones keep their ids and are laid out after every existing block, in reverse post-order.
        std::size_t placed = 0;
        for (const auto blockId : reversePostOrder(graph)) {
            if (blockId >= firstClone) {
                graph.layoutOrder.push_back(blockId);
                ++placed;
            }
        }
        if (placed != blocks.size()) {
            throw std::runtime_error("a cloned control flow block is unreachable");
        }
        return;
    }
    std::vector<BasicBlock> ordered;
    ordered.reserve(graph.blocks.size());
    for (std::uint32_t blockId = 0; blockId < firstClone; ++blockId) {
        ordered.push_back(graph.blocks[blockId]);
    }
    for (const auto blockId : reversePostOrder(graph)) {
        if (blockId >= firstClone) {
            ordered.push_back(graph.blocks[blockId]);
        }
    }
    if (ordered.size() != graph.blocks.size()) {
        throw std::runtime_error("a cloned control flow block is unreachable");
    }
    applyBlockOrder(graph, std::move(ordered));
}

bool privatizeOneSharedReturn(ControlFlowGraph& graph) {
    for (const auto& block : graph.blocks) {
        if (!block.successors.empty() || block.terminator.kind != TerminatorKind::Return || block.predecessors.size() < 2u || block.estimatedSpirvWords > CloneWordFloor) {
            continue;
        }
        for (const auto predecessor : block.predecessors) {
            const auto& from = graph.FindBlock(predecessor);
            if (from.terminator.kind != TerminatorKind::ConditionalBranch || findInnermostContainingLoop(graph, predecessor) != nullptr) {
                continue;
            }
            const auto shared = block.id;
            cloneBlocks(graph, {shared}, {{predecessor, shared}});
            return true;
        }
    }
    return false;
}

std::optional<ControlFlowGraph> privatizeMergeTail(const ControlFlowGraph& graph, std::uint32_t header, std::uint32_t merge, std::uint32_t& cost, const std::function<void(ControlFlowGraph&)>& recompute) {
    const auto& headerBlock = graph.FindBlock(header);
    if ((headerBlock.terminator.trueBlock != merge && headerBlock.terminator.falseBlock != merge) || graph.FindBlock(merge).predecessors.size() < 2u || !hasLinearPathToTerminal(graph, merge)) {
        return std::nullopt;
    }
    const auto tail = linearTail(graph, merge);
    if (!cloneRefusal(graph, header, tail).empty()) {
        return std::nullopt;
    }
    auto candidate = graph;
    cloneBlocks(candidate, tail, {{header, merge}});
    recompute(candidate);
    if (candidate.irreducible || findSelectionMerge(candidate, candidate.FindBlock(header)) == InvalidControlFlowId) {
        return std::nullopt;
    }
    cost = estimatedSpirvWords(graph, tail);
    return candidate;
}

std::vector<Edge> loopEdges(const ControlFlowGraph& graph) {
    std::vector<Edge> edges;
    for (const auto& loop : graph.naturalLoops) {
        edges.emplace_back(loop.headerBlock, loop.latchBlock);
    }
    std::sort(edges.begin(), edges.end());
    return edges;
}

std::string junctionRefusal(const ControlFlowGraph& graph, std::uint32_t header, const std::vector<std::uint32_t>& entered, const std::vector<Edge>& routed) {
    for (const auto blockId : entered) {
        if (graph.Dominates(blockId, header)) {
            return "the region wraps around to dominating block " + std::to_string(blockId);
        }
    }
    for (const auto& [source, target] : routed) {
        const auto kind = graph.FindBlock(source).terminator.kind;
        if (kind != TerminatorKind::Branch && kind != TerminatorKind::ConditionalBranch) {
            return "block " + std::to_string(source) + " ends in an indirect or unsupported branch";
        }
        if (graph.Dominates(target, source)) {
            return "the edge from block " + std::to_string(source) + " to block " + std::to_string(target) + " is a back edge";
        }
    }
    return {};
}

std::uint32_t appendRouteBlock(ControlFlowGraph& graph, std::uint32_t positionBlock) {
    const auto& position = graph.FindBlock(positionBlock);
    BasicBlock block;
    block.id = static_cast<std::uint32_t>(graph.blocks.size());
    block.startProgramCounter = position.startProgramCounter;
    block.endProgramCounter = position.startProgramCounter;
    block.instructionBegin = position.instructionBegin;
    block.instructionEnd = position.instructionBegin;
    graph.blocks.push_back(std::move(block));
    return graph.blocks.back().id;
}

void setBranch(BasicBlock& block, std::uint32_t target) {
    block.successors = {target};
    block.terminator.kind = TerminatorKind::Branch;
    block.terminator.condition = BranchCondition::Always;
    block.terminator.trueBlock = target;
}

std::uint32_t appendAssignmentBlock(ControlFlowGraph& graph, std::uint32_t positionBlock, std::uint32_t target, std::uint32_t variable, std::int32_t value) {
    const auto assignment = appendRouteBlock(graph, positionBlock);
    auto& block = graph.FindBlock(assignment);
    setBranch(block, target);
    block.terminator.gotoVariable = variable;
    block.terminator.gotoValue = value;
    return assignment;
}

std::uint32_t appendJunction(ControlFlowGraph& graph, const std::vector<std::uint32_t>& targets, std::uint32_t firstVariable) {
    auto next = targets.back();
    for (auto index = static_cast<std::uint32_t>(targets.size() - 1u); index-- > 0u;) {
        const auto dispatch = appendRouteBlock(graph, targets[index]);
        auto& block = graph.FindBlock(dispatch);
        block.successors = {targets[index], next};
        block.terminator.kind = TerminatorKind::ConditionalBranch;
        block.terminator.condition = BranchCondition::GotoVariable;
        block.terminator.trueBlock = targets[index];
        block.terminator.falseBlock = next;
        block.terminator.gotoVariable = firstVariable + index;
        next = dispatch;
    }
    const auto junction = appendRouteBlock(graph, targets.front());
    setBranch(graph.FindBlock(junction), next);
    return junction;
}

std::optional<ControlFlowGraph> routeThroughJunction(const ControlFlowGraph& graph, std::uint32_t header, std::uint32_t merge, const std::vector<std::uint32_t>& region, const std::vector<std::uint32_t>& entered, const std::vector<Edge>& entryEdges, SplitBudget& budget, const std::function<void(ControlFlowGraph&)>& recompute, std::string& refusal) {
    std::vector<Edge> routed = entryEdges;
    std::vector<std::uint32_t> sources = {header};
    for (const auto member : region) {
        if (!contains(entered, member)) {
            sources.push_back(member);
        }
    }
    for (const auto source : sources) {
        for (const auto successor : graph.FindBlock(source).successors) {
            if (successor == merge || contains(entered, successor)) {
                routed.emplace_back(source, successor);
            }
        }
    }
    refusal = junctionRefusal(graph, header, entered, routed);
    if (!refusal.empty()) {
        return std::nullopt;
    }

    const auto order = reversePostOrder(graph);
    const auto orderOf = [&](std::uint32_t blockId) {
        return std::distance(order.begin(), std::find(order.begin(), order.end(), blockId));
    };
    std::vector<std::uint32_t> targets;
    bool reachesMerge = false;
    for (const auto& edge : routed) {
        if (edge.second == merge) {
            reachesMerge = true;
        } else {
            addUnique(targets, edge.second);
        }
    }
    std::sort(targets.begin(), targets.end(), [&](std::uint32_t lhs, std::uint32_t rhs) { return orderOf(lhs) < orderOf(rhs); });
    if (reachesMerge) {
        targets.push_back(merge);
    }

    auto candidate = graph;
    const auto variableCount = static_cast<std::uint32_t>(targets.size() - 1u);
    const auto firstVariable = budget.nextGotoVariable;
    const auto junction = appendJunction(candidate, targets, firstVariable);
    for (const auto& [source, target] : routed) {
        const auto targetIndex = static_cast<std::uint32_t>(std::distance(targets.begin(), std::find(targets.begin(), targets.end(), target)));
        auto entry = junction;
        for (auto index = std::min(targetIndex + 1u, variableCount); index-- > 0u;) {
            entry = appendAssignmentBlock(candidate, target, entry, firstVariable + index, index == targetIndex ? 1 : 0);
        }
        redirectEdge(candidate.FindBlock(source), target, entry);
    }
    rebuildPredecessors(candidate);
    recompute(candidate);
    if (candidate.irreducible || candidate.unsupported) {
        refusal = "routing through a junction would make the graph irreducible";
        return std::nullopt;
    }
    if (loopEdges(candidate) != loopEdges(graph)) {
        refusal = "routing through a junction would change the natural loops";
        return std::nullopt;
    }
    budget.nextGotoVariable += variableCount;
    return candidate;
}

std::uint32_t nextGotoVariable(const ControlFlowGraph& graph) {
    std::uint32_t next = 0;
    for (const auto& block : graph.blocks) {
        if (block.terminator.gotoVariable != InvalidControlFlowId) {
            next = std::max(next, block.terminator.gotoVariable + 1u);
        }
    }
    return next;
}

bool hasSharedExitSelection(const ControlFlowGraph& graph, const NaturalLoop& loop) {
    for (const auto id : loop.blocks) {
        const auto& block = graph.FindBlock(id);
        if (block.terminator.kind != TerminatorKind::ConditionalBranch) continue;
        const auto globalMerge = graph.FindNearestCommonPostDominator(block.terminator.trueBlock, block.terminator.falseBlock);
        if (globalMerge != InvalidControlFlowId && isInsideLoopConstruct(graph, loop, globalMerge)) continue;
        for (const auto exit : loop.exitBlocks) {
            const auto& forwarder = graph.FindBlock(exit);
            if (forwarder.instructionBegin == forwarder.instructionEnd && forwarder.terminator.kind == TerminatorKind::Branch && forwarder.terminator.gotoVariable != InvalidControlFlowId && forwarder.terminator.trueBlock == loop.mergeBlock) continue;
            if (exit != loop.mergeBlock && graph.Dominates(block.id, exit) && reachesWithinIteration(graph, loop, block.terminator.trueBlock, exit) && reachesWithinIteration(graph, loop, block.terminator.falseBlock, exit)) return true;
        }
    }
    return false;
}

bool exitsLeaveConstruct(const ControlFlowGraph& graph, const NaturalLoop& loop) {
    std::vector<bool> visited(graph.blocks.size(), false);
    std::vector<std::uint32_t> pending;
    pending.insert(pending.end(), loop.exitBlocks.begin(), loop.exitBlocks.end());
    while (!pending.empty()) {
        const auto blockId = pending.back();
        pending.pop_back();
        if (blockId == loop.mergeBlock || visited[blockId]) {
            continue;
        }
        if (!graph.Dominates(loop.headerBlock, blockId)) {
            return true;
        }
        visited[blockId] = true;
        const auto& successors = graph.FindBlock(blockId).successors;
        pending.insert(pending.end(), successors.begin(), successors.end());
    }
    return false;
}

std::optional<ControlFlowGraph> routeLoopExits(const ControlFlowGraph& graph, const NaturalLoop& loop, const std::function<void(ControlFlowGraph&)>& recompute, std::string& refusal) {
    std::vector<Edge> exits;
    for (const auto member : loop.blocks) {
        const auto& block = graph.FindBlock(member);
        for (const auto successor : block.successors) {
            if (contains(loop.blocks, successor)) {
                continue;
            }
            if (block.terminator.kind != TerminatorKind::Branch && block.terminator.kind != TerminatorKind::ConditionalBranch) {
                refusal = "block " + std::to_string(member) + " ends in an indirect or unsupported branch";
                return std::nullopt;
            }
            exits.emplace_back(member, successor);
        }
    }

    const auto order = reversePostOrder(graph);
    const auto orderOf = [&](std::uint32_t blockId) {
        return std::distance(order.begin(), std::find(order.begin(), order.end(), blockId));
    };
    std::vector<std::uint32_t> targets;
    for (const auto& edge : exits) {
        addUnique(targets, edge.second);
    }
    std::sort(targets.begin(), targets.end(), [&](std::uint32_t lhs, std::uint32_t rhs) { return orderOf(lhs) < orderOf(rhs); });
    const auto latchExit = std::find_if(exits.begin(), exits.end(), [&](const Edge& edge) { return edge.first == loop.latchBlock; });
    const auto fallback = latchExit != exits.end() ? latchExit->second : targets.back();
    targets.erase(std::find(targets.begin(), targets.end(), fallback));
    targets.push_back(fallback);

    auto candidate = graph;
    const auto firstVariable = nextGotoVariable(graph);
    const auto variableCount = static_cast<std::uint32_t>(targets.size() - 1u);
    const auto junction = appendJunction(candidate, targets, firstVariable);

    auto entry = loop.headerBlock;
    for (auto index = variableCount; index-- > 0u;) {
        entry = appendAssignmentBlock(candidate, loop.headerBlock, entry, firstVariable + index, 0);
    }
    for (const auto predecessor : graph.FindBlock(loop.headerBlock).predecessors) {
        if (!contains(loop.blocks, predecessor)) {
            redirectEdge(candidate.FindBlock(predecessor), loop.headerBlock, entry);
        }
    }
    if (candidate.entryBlock == loop.headerBlock) {
        candidate.entryBlock = entry;
    }

    for (const auto& [source, target] : exits) {
        const auto index = static_cast<std::uint32_t>(std::distance(targets.begin(), std::find(targets.begin(), targets.end(), target)));
        const auto exit = index == variableCount ? junction : appendAssignmentBlock(candidate, target, junction, firstVariable + index, 1);
        redirectEdge(candidate.FindBlock(source), target, exit);
    }
    rebuildPredecessors(candidate);
    recompute(candidate);
    if (candidate.irreducible || candidate.unsupported) {
        refusal = "routing its exits through one block would make the graph irreducible";
        return std::nullopt;
    }
    if (loopEdges(candidate) != loopEdges(graph)) {
        refusal = "routing its exits through one block would change the natural loops";
        return std::nullopt;
    }
    const auto routed = std::find_if(candidate.naturalLoops.begin(), candidate.naturalLoops.end(), [&](const NaturalLoop& value) { return value.headerBlock == loop.headerBlock; });
    if (routed == candidate.naturalLoops.end() || routed->mergeBlock != junction) {
        refusal = "routing its exits through one block does not make that block its merge";
        return std::nullopt;
    }
    return candidate;
}

bool structurizeEnteredRegion(ControlFlowGraph& graph, std::uint32_t header, std::uint32_t merge, const std::vector<std::uint32_t>& region, SplitBudget& budget, const std::function<void(ControlFlowGraph&)>& recompute) {
    const auto entryEdges = externalEntryEdges(graph, header, region);
    if (entryEdges.empty()) {
        return false;
    }
    const auto entered = reachableWithin(graph, entryEdges, region);
    const auto enteredWords = estimatedSpirvWords(graph, entered);
    const auto structuralReason = cloneRefusal(graph, header, entered);
    auto cloneReason = structuralReason.empty() ? cloneCostRefusal(budget, enteredWords) : structuralReason;

    std::uint32_t tailWords = 0;
    if (auto privatized = privatizeMergeTail(graph, header, merge, tailWords, recompute); privatized && cloneCostRefusal(budget, tailWords).empty() && (!cloneReason.empty() || tailWords <= enteredWords)) {
        budget.remainingCloneWords -= tailWords;
        budget.remainingFallbackWords -= std::min(tailWords, budget.remainingFallbackWords);
        graph = std::move(*privatized);
        return true;
    }
    if (cloneReason.empty()) {
        budget.remainingCloneWords -= enteredWords;
        budget.remainingFallbackWords -= std::min(enteredWords, budget.remainingFallbackWords);
        cloneBlocks(graph, entered, entryEdges);
        return true;
    }
    std::string junctionReason;
    if (auto routed = routeThroughJunction(graph, header, merge, region, entered, entryEdges, budget, recompute, junctionReason)) {
        graph = std::move(*routed);
        return true;
    }
    if (structuralReason.empty() && enteredWords <= budget.remainingFallbackWords) {
        budget.remainingFallbackWords -= enteredWords;
        cloneBlocks(graph, entered, entryEdges);
        return true;
    }
    if (structuralReason.empty()) {
        cloneReason += " and the fallback budget of " + std::to_string(budget.remainingFallbackWords);
    }
    throw std::runtime_error("selection header block " + std::to_string(header) + " has externally entered region block " + std::to_string(entryEdges.front().second) + " (from block " + std::to_string(entryEdges.front().first) + "): " + cloneReason + "; " + junctionReason);
}

// XBOX-FORK: the scan over selection headers used to restart from the deepest header after every split, so a graph with
// thousands of splits re-evaluated (mostly unchanged) headers millions of times. `cursor` is the position (in the sorted
// header order) of the header that acted last; the next scan resumes there. When a resumed scan finds nothing, the caller
// rescans from 0, so the fixed point reached is the same kind of state: no header has an action left.
bool splitOneSelectionMerge(ControlFlowGraph& graph, SplitBudget& budget, const std::function<void(ControlFlowGraph&)>& recompute, bool routeLeavingExits, bool routeSharedExits, std::size_t& cursor, std::vector<std::uint32_t>& headerOrder, bool& headerOrderValid, std::vector<std::uint32_t>& cachedLoopHeaders, bool& simpleSplit, bool predsSynced) {
    // XBOX-FORK: the header order (deeper first, then lower id) is built from plain integer keys. It is rebuilt on every
    // call: block ids are renumbered by each split (moveBlockBefore), so an order cached across calls would name other blocks.
    std::vector<std::uint32_t> loopHeaders;
    loopHeaders.reserve(graph.naturalLoops.size());
    for (const auto& loop : graph.naturalLoops) {
        loopHeaders.push_back(loop.headerBlock);
    }
    sortUnique(loopHeaders);
    if (!headerOrderValid || graph.layoutOrder.empty() || loopHeaders != cachedLoopHeaders) {
        thread_local std::vector<std::uint64_t> headerKeys;
        headerKeys.clear();
        const bool hasDomDepth = graph.domTree.valid && graph.blocks.size() == graph.domTree.depth.size();
        for (const auto& block : graph.blocks) {
            if (block.terminator.kind == TerminatorKind::ConditionalBranch && !containsSorted(loopHeaders, block.id)) {
                const auto depth = hasDomDepth ? graph.domTree.depth[block.id] : static_cast<std::uint32_t>(block.dominators.size());
                headerKeys.push_back((static_cast<std::uint64_t>(~depth) << 32) | block.id);
            }
        }
        std::sort(headerKeys.begin(), headerKeys.end());
        headerOrder.clear();
        headerOrder.reserve(headerKeys.size());
        for (const auto key : headerKeys) headerOrder.push_back(static_cast<std::uint32_t>(key & 0xffffffffu));
        cachedLoopHeaders = loopHeaders;
        headerOrderValid = !graph.layoutOrder.empty();
    }
    const auto& selectionHeaders = headerOrder;
    StructProf::Scope pTop("sel:total");
    std::optional<bool> dominanceConstructsCache;
    const auto dominanceConstructsNow = [&] {
        if (!dominanceConstructsCache) {
            dominanceConstructsCache = (predsSynced && graph.domTree.valid && graph.blocks.size() == graph.domTree.tin.size()) ? true : constructsFollowDominance(graph);
        }
        return *dominanceConstructsCache;
    };

    for (std::size_t headerIndex = std::min(cursor, selectionHeaders.size()); headerIndex < selectionHeaders.size(); ++headerIndex) {
        const auto blockId = selectionHeaders[headerIndex];
        const auto& block = graph.FindBlock(blockId);
        if (block.terminator.kind != TerminatorKind::ConditionalBranch || containsSorted(loopHeaders, blockId)) {
            continue;
        }
        bool loopExit;
        { StructProf::Scope p0("sel:isStructuredLoopExit"); loopExit = isStructuredLoopExit(graph, block, routeLeavingExits); }
        if (loopExit) {
            continue;
        }

        std::uint32_t merge;
        { StructProf::Scope p1("sel:findSelectionMerge"); merge = findSelectionMerge(graph, block, routeLeavingExits, routeSharedExits); }
        if (merge == InvalidControlFlowId) {
            continue;
        }

        std::vector<std::uint32_t> region;
        { StructProf::Scope p2("sel:selectionRegion"); region = selectionRegion(graph, block, merge); }
        bool entered;
        { StructProf::Scope p3("sel:structurizeEnteredRegion"); entered = structurizeEnteredRegion(graph, blockId, merge, region, budget, recompute); }
        if (entered) {
            cursor = headerIndex;
            headerOrderValid = false;
            simpleSplit = false;
            return true;
        }

        if (merge != blockId && dominanceConstructsNow() && graph.Dominates(blockId, blockId) && !graph.Dominates(merge, blockId)) {
            const auto inConstruct = [&](std::uint32_t member) { return member != merge && graph.Dominates(blockId, member) && !graph.Dominates(merge, member); };
                const bool splitDone = splitSharedMergeBlock(graph, merge, inConstruct, mergeLeavesContainingLoop(graph, blockId, merge));
            if (splitDone) {
                cursor = headerIndex;
                simpleSplit = true;
                return true;
            }
            continue;
        }
        StructProf::Scope p4("sel:dominated+splitShared");
        const auto constructBlocks = dominatedBlocks(graph, blockId, merge);
        const auto forceSplit = mergeLeavesContainingLoop(graph, blockId, merge);
        const bool splitDone2 = splitSharedMergeBlock(graph, merge, constructBlocks, forceSplit);
        if (splitDone2) {
            cursor = headerIndex;
            simpleSplit = true;
            return true;
        }
    }
    return false;
}

std::vector<std::uint32_t> naturalLoopBody(const ControlFlowGraph& graph, std::uint32_t headerBlock, std::uint32_t latchBlock, bool& natural) {
    std::vector<std::uint32_t> body;
    std::vector<bool> inBody(graph.blocks.size(), false);
    std::vector<std::uint32_t> stack;
    natural = true;
    addUnique(body, headerBlock);
    addUnique(body, latchBlock);
    inBody[headerBlock] = true;
    inBody[latchBlock] = true;
    if (latchBlock != headerBlock) {
        stack.push_back(latchBlock);
    }

    while (!stack.empty()) {
        const auto blockId = stack.back();
        stack.pop_back();
        const auto& block = graph.FindBlock(blockId);
        for (const auto predecessor : block.predecessors) {
            if (!graph.Dominates(headerBlock, predecessor)) {
                natural = false;
            }
            if (!inBody[predecessor]) {
                inBody[predecessor] = true;
                body.push_back(predecessor);
                if (predecessor != headerBlock) {
                    stack.push_back(predecessor);
                }
            }
        }
    }

    sortUnique(body);
    return body;
}

struct TarjanState {
    const ControlFlowGraph* graph = nullptr;
    std::uint32_t nextIndex = 0;
    std::vector<std::uint32_t> index;
    std::vector<std::uint32_t> lowlink;
    std::vector<bool> onStack;
    std::vector<std::uint32_t> stack;
    std::vector<StronglyConnectedComponent> components;
};

void emitComponent(TarjanState& state, std::uint32_t blockId) {
    StronglyConnectedComponent component;
    for (;;) {
        const auto member = state.stack.back();
        state.stack.pop_back();
        state.onStack[member] = false;
        component.blocks.push_back(member);
        if (member == blockId) {
            break;
        }
    }
    sortUnique(component.blocks);

    bool cyclic = component.blocks.size() > 1u;
    for (const auto member : component.blocks) {
        const auto& memberBlock = state.graph->FindBlock(member);
        if (contains(memberBlock.successors, member)) {
            cyclic = true;
        }
        for (const auto predecessor : memberBlock.predecessors) {
            if (!containsSorted(component.blocks, predecessor)) {
                addUnique(component.entryBlocks, member);
            }
        }
    }
    sortUnique(component.entryBlocks);
    component.irreducible = cyclic && component.entryBlocks.size() > 1u;
    state.components.push_back(std::move(component));
}

void tarjanVisit(TarjanState& state, std::uint32_t root) {
    struct Frame {
        std::uint32_t blockId;
        const std::vector<std::uint32_t>* successors;
        std::size_t next;
    };
    std::vector<Frame> work;
    const auto enter = [&](std::uint32_t blockId) {
        state.index[blockId] = state.nextIndex;
        state.lowlink[blockId] = state.nextIndex;
        ++state.nextIndex;
        state.stack.push_back(blockId);
        state.onStack[blockId] = true;
        work.push_back({blockId, &state.graph->FindBlock(blockId).successors, 0u});
    };
    enter(root);
    while (!work.empty()) {
        auto& frame = work.back();
        const auto blockId = frame.blockId;
        if (frame.next < frame.successors->size()) {
            const auto successor = (*frame.successors)[frame.next++];
            if (state.index[successor] == InvalidControlFlowId) {
                enter(successor);
            } else if (state.onStack[successor]) {
                state.lowlink[blockId] = std::min(state.lowlink[blockId], state.index[successor]);
            }
            continue;
        }
        work.pop_back();
        if (!work.empty()) {
            auto& parent = state.lowlink[work.back().blockId];
            parent = std::min(parent, state.lowlink[blockId]);
        }
        if (state.lowlink[blockId] == state.index[blockId]) {
            emitComponent(state, blockId);
        }
    }
}

// XBOX-FORK: a retry hook so Recompile can fall back to flag structurization when the structured result fails IR verification.
bool forcedFlagStructurization() {
    static const bool forced = std::getenv("APS5_FORCE_STRUCTURIZER_FALLBACK") != nullptr;
    return forced || ForcedFlagStructurization();
}

std::vector<std::uint32_t> terminatorTargets(const Terminator& terminator) {
    if (terminator.kind == TerminatorKind::Branch) {
        return {terminator.trueBlock};
    }
    if (terminator.kind == TerminatorKind::ConditionalBranch) {
        return {terminator.trueBlock, terminator.falseBlock};
    }
    return {};
}

bool supportsFlagStructurization(const ControlFlowGraph& graph) {
    const auto count = graph.blocks.size();
    if (graph.unsupported || graph.entryBlock >= count) {
        return false;
    }
    for (std::uint32_t id = 0; id < count; ++id) {
        const auto& block = graph.blocks[id];
        const auto kind = block.terminator.kind;
        if (block.id != id || (kind != TerminatorKind::Branch && kind != TerminatorKind::ConditionalBranch && kind != TerminatorKind::Return)) {
            return false;
        }
        const auto targets = terminatorTargets(block.terminator);
        if (std::any_of(targets.begin(), targets.end(), [&](std::uint32_t target) { return target >= count; })) {
            return false;
        }
    }
    return true;
}

struct FlagEmission {
    ControlFlowGraph* graph = nullptr;
    std::uint32_t entryBlock = InvalidControlFlowId;
    std::uint32_t firstVariable = 0;
    std::uint32_t nextVariable = 0;
    std::vector<std::vector<std::uint32_t>> targets;
    std::vector<std::vector<std::uint32_t>> successors;
    std::vector<std::vector<std::uint32_t>> predecessors;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> openLoops;
    std::vector<std::uint32_t> order;
    std::vector<std::uint32_t> fallthroughs;
};

std::vector<std::vector<std::uint32_t>> orderedComponents(const FlagEmission& emission, const std::vector<std::uint32_t>& members) {
    const auto count = emission.successors.size();
    std::vector<bool> inside(count, false);
    for (const auto member : members) {
        inside[member] = true;
    }
    std::vector<std::uint32_t> index(count, InvalidControlFlowId);
    std::vector<std::uint32_t> lowlink(count, InvalidControlFlowId);
    std::vector<std::uint32_t> componentOf(count, InvalidControlFlowId);
    std::vector<bool> onStack(count, false);
    std::vector<std::uint32_t> stack;
    std::vector<std::pair<std::uint32_t, std::size_t>> work;
    std::vector<std::vector<std::uint32_t>> components;
    std::uint32_t nextIndex = 0;
    const auto visit = [&](std::uint32_t blockId) {
        index[blockId] = nextIndex;
        lowlink[blockId] = nextIndex;
        ++nextIndex;
        stack.push_back(blockId);
        onStack[blockId] = true;
        work.emplace_back(blockId, 0u);
    };
    for (const auto root : members) {
        if (index[root] != InvalidControlFlowId) {
            continue;
        }
        visit(root);
        while (!work.empty()) {
            const auto blockId = work.back().first;
            const auto& successors = emission.successors[blockId];
            if (work.back().second < successors.size()) {
                const auto successor = successors[work.back().second++];
                if (!inside[successor]) {
                    continue;
                }
                if (index[successor] == InvalidControlFlowId) {
                    visit(successor);
                } else if (onStack[successor]) {
                    lowlink[blockId] = std::min(lowlink[blockId], index[successor]);
                }
                continue;
            }
            work.pop_back();
            if (!work.empty()) {
                auto& parent = lowlink[work.back().first];
                parent = std::min(parent, lowlink[blockId]);
            }
            if (lowlink[blockId] != index[blockId]) {
                continue;
            }
            std::vector<std::uint32_t> component;
            for (;;) {
                const auto member = stack.back();
                stack.pop_back();
                onStack[member] = false;
                componentOf[member] = static_cast<std::uint32_t>(components.size());
                component.push_back(member);
                if (member == blockId) {
                    break;
                }
            }
            sortUnique(component);
            components.push_back(std::move(component));
        }
    }

    std::vector<std::uint32_t> incoming(components.size(), 0u);
    std::vector<std::vector<std::uint32_t>> following(components.size());
    for (const auto member : members) {
        for (const auto successor : emission.successors[member]) {
            if (inside[successor] && componentOf[successor] != componentOf[member]) {
                following[componentOf[member]].push_back(componentOf[successor]);
            }
        }
    }
    for (auto& next : following) {
        sortUnique(next);
        for (const auto component : next) {
            ++incoming[component];
        }
    }
    std::map<std::uint32_t, std::uint32_t> ready;
    for (std::uint32_t component = 0; component < components.size(); ++component) {
        if (incoming[component] == 0u) {
            ready.emplace(components[component].front(), component);
        }
    }
    std::vector<std::vector<std::uint32_t>> ordered;
    ordered.reserve(components.size());
    while (!ready.empty()) {
        const auto component = ready.begin()->second;
        ready.erase(ready.begin());
        for (const auto next : following[component]) {
            if (--incoming[next] == 0u) {
                ready.emplace(components[next].front(), next);
            }
        }
        ordered.push_back(std::move(components[component]));
    }
    return ordered;
}

std::uint32_t flagLoopHead(const FlagEmission& emission, const std::vector<std::uint32_t>& component) {
    for (const auto member : component) {
        const auto& predecessors = emission.predecessors[member];
        if (member == emission.entryBlock || std::any_of(predecessors.begin(), predecessors.end(), [&](std::uint32_t predecessor) { return !std::binary_search(component.begin(), component.end(), predecessor); })) {
            return member;
        }
    }
    return component.front();
}

std::uint32_t appendFlagBlock(FlagEmission& emission, std::uint32_t positionBlock) {
    const auto block = appendRouteBlock(*emission.graph, positionBlock);
    emission.order.push_back(block);
    return block;
}

std::uint32_t appendFlagAssignment(FlagEmission& emission, std::uint32_t positionBlock, std::uint32_t variable, std::int32_t value) {
    const auto assignment = appendAssignmentBlock(*emission.graph, positionBlock, positionBlock, variable, value);
    emission.order.push_back(assignment);
    return assignment;
}

void setDispatch(BasicBlock& block, std::uint32_t variable, std::uint32_t trueTarget, std::uint32_t falseTarget) {
    block.successors = {trueTarget, falseTarget};
    block.terminator.kind = TerminatorKind::ConditionalBranch;
    block.terminator.condition = BranchCondition::GotoVariable;
    block.terminator.trueBlock = trueTarget;
    block.terminator.falseBlock = falseTarget;
    block.terminator.gotoVariable = variable;
}

std::vector<bool> alwaysNextComponents(const FlagEmission& emission, const std::vector<std::vector<std::uint32_t>>& components, const std::vector<std::uint32_t>& firsts) {
    const auto count = static_cast<std::uint32_t>(components.size());
    std::vector<std::uint32_t> componentOf(emission.successors.size(), InvalidControlFlowId);
    for (std::uint32_t component = 0; component < count; ++component) {
        for (const auto member : components[component]) {
            componentOf[member] = component;
        }
    }
    std::vector<bool> next(count, true);
    std::vector<std::int64_t> skipped(count + 1u, 0);
    for (std::uint32_t component = 0; component < count; ++component) {
        for (const auto member : components[component]) {
            for (const auto successor : emission.successors[member]) {
                const auto target = componentOf[successor];
                if (target <= component) {
                    continue;
                }
                ++skipped[component + 1u];
                --skipped[target];
                if (successor != firsts[target]) {
                    next[target] = false;
                }
            }
        }
    }
    const auto entryComponent = componentOf[emission.entryBlock];
    std::int64_t crossing = 0;
    for (std::uint32_t component = 0; component < count; ++component) {
        crossing += skipped[component];
        if (crossing > 0 || component < entryComponent || (component == entryComponent && firsts[component] != emission.entryBlock)) {
            next[component] = false;
        }
    }
    return next;
}

void emitFlagBlock(FlagEmission& emission, std::uint32_t original, bool guarded) {
    auto& graph = *emission.graph;
    const auto reach = emission.firstVariable + original;
    const auto guard = guarded ? appendFlagBlock(emission, original) : InvalidControlFlowId;
    const auto clear = appendFlagAssignment(emission, original, reach, 0);
    emission.fallthroughs.push_back(clear);
    emission.order.push_back(original);

    std::vector<std::uint32_t> edges;
    std::vector<std::uint32_t> ends;
    for (const auto target : emission.targets[original]) {
        const auto edge = appendFlagAssignment(emission, original, emission.firstVariable + target, 1);
        edges.push_back(edge);
        const auto loop = std::find_if(emission.openLoops.begin(), emission.openLoops.end(), [&](const std::pair<std::uint32_t, std::uint32_t>& open) { return open.first == target; });
        if (loop == emission.openLoops.end()) {
            ends.push_back(edge);
            continue;
        }
        emission.fallthroughs.push_back(edge);
        ends.push_back(appendFlagAssignment(emission, original, loop->second, 1));
    }

    const bool conditional = graph.blocks[original].terminator.kind == TerminatorKind::ConditionalBranch;
    const auto join = conditional ? appendFlagBlock(emission, original) : InvalidControlFlowId;
    const auto merge = guarded ? appendFlagBlock(emission, original) : InvalidControlFlowId;
    const auto exit = conditional ? join : merge;
    for (const auto end : ends) {
        if (exit == InvalidControlFlowId) {
            emission.fallthroughs.push_back(end);
        } else {
            setBranch(graph.blocks[end], exit);
        }
    }
    if (conditional) {
        emission.fallthroughs.push_back(join);
    }
    if (guarded) {
        emission.fallthroughs.push_back(merge);
        setDispatch(graph.blocks[guard], reach, clear, merge);
        graph.blocks[guard].terminator.mergeBlock = merge;
    }

    auto& block = graph.blocks[original];
    block.successors = edges;
    block.terminator.mergeBlock = join;
    block.terminator.continueBlock = InvalidControlFlowId;
    block.terminator.loopHeader = false;
    if (!edges.empty()) {
        block.terminator.trueBlock = edges.front();
    }
    if (conditional) {
        block.terminator.falseBlock = edges.back();
    }
}

void emitFlagRegion(FlagEmission& emission, const std::vector<std::uint32_t>& members);

void emitFlagLoop(FlagEmission& emission, std::vector<std::uint32_t> component, std::uint32_t head, bool guarded) {
    auto& graph = *emission.graph;
    const auto again = emission.nextVariable++;
    const auto header = appendFlagBlock(emission, head);
    emission.fallthroughs.push_back(header);
    emission.fallthroughs.push_back(appendFlagAssignment(emission, head, again, 0));
    emission.openLoops.emplace_back(head, again);
    emitFlagBlock(emission, head, guarded);
    component.erase(std::find(component.begin(), component.end(), head));
    emitFlagRegion(emission, component);
    emission.openLoops.pop_back();
    const auto latch = appendFlagBlock(emission, head);
    const auto merge = appendFlagBlock(emission, head);
    emission.fallthroughs.push_back(merge);
    setDispatch(graph.blocks[latch], again, header, merge);
    auto& terminator = graph.blocks[header].terminator;
    terminator.loopHeader = true;
    terminator.mergeBlock = merge;
    terminator.continueBlock = latch;
}

void emitFlagRegion(FlagEmission& emission, const std::vector<std::uint32_t>& members) {
    const auto components = orderedComponents(emission, members);
    std::vector<bool> loops;
    std::vector<std::uint32_t> firsts;
    for (const auto& component : components) {
        const auto first = component.front();
        loops.push_back(component.size() != 1u || contains(emission.successors[first], first));
        firsts.push_back(loops.back() ? flagLoopHead(emission, component) : first);
    }
    const auto next = emission.openLoops.empty() ? alwaysNextComponents(emission, components, firsts) : std::vector<bool>(components.size(), false);
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (loops[index]) {
            emitFlagLoop(emission, components[index], firsts[index], !next[index]);
        } else {
            emitFlagBlock(emission, firsts[index], !next[index] || emission.graph->blocks[firsts[index]].terminator.kind == TerminatorKind::Return);
        }
    }
}

}

void Structurizer::Structurize(ControlFlowGraph& graph) const {
    const auto fast = StructBudget::fastPercent();
    if (fast >= 400u) {
        StructBudget::percent = 400u;
        structurizeCascade(graph);
        materializeAnalyses(graph);
        return;
    }
    const auto before = graph;
    StructBudget::percent = fast;
    try {
        structurizeCascade(graph);
        StructBudget::percent = 400u;
        materializeAnalyses(graph);
        return;
    } catch (const std::runtime_error&) {
        if (StructProf::enabled()) std::fprintf(stderr, "[struct] fast budget failed, rerunning with 400%%\n");
    }
    StructBudget::percent = 400u;
    graph = before;
    structurizeCascade(graph);
    materializeAnalyses(graph);
}

void Structurizer::structurizeCascade(ControlFlowGraph& graph) const {
    const auto original = graph;
    const bool flagsSupported = supportsFlagStructurization(original);
    if (flagsSupported && forcedFlagStructurization()) {
        structurizeWithFlags(graph);
        return;
    }
    try {
        StructBudget::skippedRelaxedReturnJoin = false;
        structurizeAttempts(graph, false);
    } catch (const std::runtime_error&) {
        const auto error = std::current_exception();
        try {
            if (!StructBudget::skippedRelaxedReturnJoin) {
                std::rethrow_exception(error);
            }
            graph = original;
            structurizeAttempts(graph, true);
        } catch (const std::runtime_error&) {
            if (!flagsSupported) {
                std::rethrow_exception(error);
            }
            graph = original;
            try {
                structurizeWithFlags(graph);
            } catch (const std::runtime_error&) {
                std::rethrow_exception(error);
            }
        }
    }
}

void Structurizer::structurizeWithFlags(ControlFlowGraph& graph) const {
    if (!supportsFlagStructurization(graph)) {
        throw std::runtime_error("CFG flag structurization supports only branch, conditional branch and return blocks");
    }
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    const auto entry = graph.entryBlock;
    FlagEmission emission;
    emission.graph = &graph;
    emission.entryBlock = entry;
    emission.firstVariable = nextGotoVariable(graph);
    emission.nextVariable = emission.firstVariable + count;
    emission.targets.resize(count);
    emission.successors.resize(count);
    emission.predecessors.resize(count);
    for (const auto& block : graph.blocks) {
        emission.targets[block.id] = terminatorTargets(block.terminator);
        emission.successors[block.id] = emission.targets[block.id];
        sortUnique(emission.successors[block.id]);
        for (const auto successor : emission.successors[block.id]) {
            emission.predecessors[successor].push_back(block.id);
        }
    }

    emitFlagRegion(emission, allBlockIds(count));
    const auto body = std::move(emission.order);
    emission.order.clear();
    for (auto variable = emission.firstVariable; variable < emission.nextVariable; ++variable) {
        emission.fallthroughs.push_back(appendFlagAssignment(emission, entry, variable, variable == emission.firstVariable + entry ? 1 : 0));
    }
    emission.order.insert(emission.order.end(), body.begin(), body.end());
    appendFlagBlock(emission, entry);

    const auto& order = emission.order;
    std::vector<bool> placed(graph.blocks.size(), false);
    for (const auto blockId : order) {
        if (blockId >= placed.size() || placed[blockId]) {
            throw std::runtime_error("CFG flag structurization placed a block twice");
        }
        placed[blockId] = true;
    }
    if (order.size() != graph.blocks.size()) {
        throw std::runtime_error("CFG flag structurization did not place every block");
    }
    std::vector<bool> fallthrough(graph.blocks.size(), false);
    for (const auto blockId : emission.fallthroughs) {
        fallthrough[blockId] = true;
    }
    for (std::size_t position = 0; position + 1u < order.size(); ++position) {
        if (fallthrough[order[position]]) {
            setBranch(graph.blocks[order[position]], order[position + 1u]);
        }
    }

    graph.entryBlock = order.front();
    std::vector<BasicBlock> blocks;
    blocks.reserve(order.size());
    for (const auto blockId : order) {
        blocks.push_back(std::move(graph.blocks[blockId]));
    }
    applyBlockOrder(graph, std::move(blocks));
    rebuildPredecessors(graph);
    recomputeAnalyses(graph);
    orderByDominance(graph);
    if (graph.irreducible || !hasStructuredConstructs(graph)) {
        throw std::runtime_error("CFG flag structurization did not produce structured constructs");
    }
}

void Structurizer::structurizeAttempts(ControlFlowGraph& graph, bool relaxReturnJoins) const {
    const auto original = graph;
    try {
        structurize(graph, false, false, false, relaxReturnJoins);
        if (!hasStructuredConstructs(graph)) throw std::runtime_error("CFG does not have structured constructs");
    } catch (const std::runtime_error& attemptError) {
        if (StructProf::enabled()) std::fprintf(stderr, "[struct] attempt failed: %.300s\n", attemptError.what());
        graph = original;
        try {
            structurize(graph, true, false, false, relaxReturnJoins);
            if (!hasStructuredConstructs(graph)) throw std::runtime_error("CFG does not have structured constructs");
        } catch (const std::runtime_error& attemptError) {
        if (StructProf::enabled()) std::fprintf(stderr, "[struct] attempt failed: %.300s\n", attemptError.what());
            const auto error = std::current_exception();
            graph = original;
            try {
                structurize(graph, false, true, false, relaxReturnJoins);
                if (hasStructuredConstructs(graph)) return;
            } catch (const std::runtime_error& attemptError) {
        if (StructProf::enabled()) std::fprintf(stderr, "[struct] attempt failed: %.300s\n", attemptError.what());
            }
            graph = original;
            try {
                structurize(graph, false, true, true, relaxReturnJoins);
            } catch (const std::runtime_error& attemptError) {
        if (StructProf::enabled()) std::fprintf(stderr, "[struct] attempt failed: %.300s\n", attemptError.what());
                std::rethrow_exception(error);
            }
            if (!hasStructuredConstructs(graph)) {
                std::rethrow_exception(error);
            }
        }
    }
}

void Structurizer::privatizeSharedReturns(ControlFlowGraph& graph) const {
    const auto budget = std::max<std::size_t>(16u, graph.blocks.size() * 4u);
    for (std::size_t clones = 0; privatizeOneSharedReturn(graph); ++clones) {
        if (clones == budget) {
            throw std::runtime_error("CFG shared return privatization exceeded budget");
        }
        rebuildPredecessors(graph);
        recomputeAnalyses(graph);
    }
}

void Structurizer::structurize(ControlFlowGraph& graph, bool privatizeReturns, bool routeLeavingExits, bool routeSharedExits, bool relaxReturnJoins) const {
    StructProf::Scope prof("structurize(attempt)");
    if (StructProf::enabled()) std::fprintf(stderr, "[struct] attempt privatize=%d leave=%d shared=%d relax=%d blocks=%zu\n", privatizeReturns, routeLeavingExits, routeSharedExits, relaxReturnJoins, graph.blocks.size());
    const auto startBlocks = graph.blocks.size();
    recomputeAnalyses(graph);
    verifyReducibility(graph);
    for (const auto& loop : graph.naturalLoops) {
        if (loop.exitBlocks.empty()) {
            throw std::runtime_error("loop at block " + std::to_string(loop.headerBlock) + " has no structured merge/continue");
        }
    }
    if (privatizeReturns) {
        privatizeSharedReturns(graph);
    }
    canonicalizeNaturalLoops(graph, routeLeavingExits, routeSharedExits);
    splitSharedMergeBlocks(graph, routeLeavingExits, routeSharedExits, relaxReturnJoins);
    isolateSemanticLoopHeaders(graph);
    orderByDominance(graph);
    clearStructuredTerminators(graph);

    std::map<std::uint32_t, std::uint32_t> mergeHeaders;
    const auto reserveMergeBlock = [&](std::uint32_t header, std::uint32_t merge) {
        const auto [it, inserted] = mergeHeaders.emplace(merge, header);
        if (!inserted && it->second != header) {
            throw std::runtime_error("duplicate structured merge block " + std::to_string(merge) + " for header " + std::to_string(header) + " (already used by header " + std::to_string(it->second) + ")");
        }
    };

    for (const auto& loop : graph.naturalLoops) {
        auto& header = graph.FindBlock(loop.headerBlock);
        if (loop.mergeBlock == InvalidControlFlowId || loop.continueBlock == InvalidControlFlowId) {
            throw std::runtime_error("loop at block " + std::to_string(loop.headerBlock) + " has no structured merge/continue");
        }
        if (header.instructionBegin != header.instructionEnd || header.terminator.kind != TerminatorKind::Branch) {
            throw std::runtime_error("loop header " + std::to_string(loop.headerBlock) + " is not a dedicated empty control block");
        }
        reserveMergeBlock(loop.headerBlock, loop.mergeBlock);
        header.terminator.loopHeader = true;
        header.terminator.mergeBlock = loop.mergeBlock;
        header.terminator.continueBlock = loop.continueBlock;
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> exitTails;
    for (auto& block : graph.blocks) {
        if (block.terminator.kind != TerminatorKind::ConditionalBranch || block.terminator.loopHeader) {
            continue;
        }
        if (isStructuredLoopExit(graph, block, routeLeavingExits)) {
            if (const auto merge = exitTailSelectionMerge(graph, block, routeLeavingExits); merge != InvalidControlFlowId) {
                exitTails.emplace_back(block.id, merge);
            }
            continue;
        }

        const auto merge = findSelectionMerge(graph, block, routeLeavingExits, routeSharedExits);
        if (merge == InvalidControlFlowId) {
            throw std::runtime_error("conditional block " + std::to_string(block.id) + " has no structured merge");
        }

        reserveMergeBlock(block.id, merge);
        block.terminator.mergeBlock = merge;
    }
    for (const auto& [header, merge] : exitTails) {
        if (mergeHeaders.contains(merge)) {
            continue;
        }
        reserveMergeBlock(header, merge);
        graph.FindBlock(header).terminator.mergeBlock = merge;
    }
    if (StructProf::enabled()) std::fprintf(stderr, "[struct] ok start=%zu end=%zu\n", startBlocks, graph.blocks.size());
}

void Structurizer::computeDominatorTree(ControlFlowGraph& graph) const {
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    if (inPlaceIntersections() && dominatorTrees() && graph.entryBlock < count &&
        assignTreeDominators(
            graph.blocks,
            {graph.entryBlock},
            [&](std::uint32_t id) -> const std::vector<std::uint32_t>& { return graph.blocks[id].predecessors; },
            [&](std::uint32_t id) -> const std::vector<std::uint32_t>& { return graph.blocks[id].successors; },
            graph.domTree)) {
        return;
    }
    graph.domTree.valid = false;
    const auto order = analysisOrder(graph, false);
    if (inPlaceIntersections()) {
        std::vector<BlockSet> sets;
        sets.reserve(count);
        for (const auto& block : graph.blocks) sets.push_back(block.id == graph.entryBlock ? BlockSet::only(count, block.id) : BlockSet::all(count));
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto id : order) {
                auto& block = graph.blocks[id];
                if (block.id == graph.entryBlock) continue;
                BlockSet next = block.predecessors.empty() ? BlockSet::only(count, block.id) : sets[block.predecessors.front()];
                if (!block.predecessors.empty()) {
                    for (std::size_t i = 1; i < block.predecessors.size(); ++i) next.intersect(sets[block.predecessors[i]]);
                    next.add(block.id);
                }
                changed = replaceSet(sets[id], std::move(next)) || changed;
            }
        }
        for (std::uint32_t id = 0; id < count; ++id) graph.blocks[id].dominators = sets[id].ids();
        return;
    }

    const auto all = allBlockIds(count);
    for (auto& block : graph.blocks) block.dominators = block.id == graph.entryBlock ? std::vector<std::uint32_t>{block.id} : all;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto id : order) {
            auto& block = graph.blocks[id];
            if (block.id == graph.entryBlock) continue;
            std::vector<std::uint32_t> next;
            if (block.predecessors.empty()) {
                next = {block.id};
            } else {
                next = graph.blocks[block.predecessors.front()].dominators;
                for (std::size_t i = 1; i < block.predecessors.size(); ++i) intersectAnalysisSet(next, graph.blocks[block.predecessors[i]].dominators);
                addSortedUnique(next, block.id);
            }
            if (next != block.dominators) {
                replaceAnalysisSet(block.dominators, std::move(next));
                changed = true;
            }
        }
    }
}

void Structurizer::detectNaturalLoops(ControlFlowGraph& graph) const {
    graph.naturalLoops.clear();
    for (auto& edge : graph.backEdges) {
        bool natural = true;
        auto body = naturalLoopBody(graph, edge.targetBlock, edge.sourceBlock, natural);
        edge.natural = natural;

        NaturalLoop loop;
        loop.headerBlock = edge.targetBlock;
        loop.latchBlock = edge.sourceBlock;
        loop.continueBlock = edge.sourceBlock;
        loop.blocks = std::move(body);

        for (const auto blockId : loop.blocks) {
            const auto& block = graph.FindBlock(blockId);
            for (const auto successor : block.successors) {
                if (!containsSorted(loop.blocks, successor)) {
                    addUnique(loop.exitBlocks, successor);
                }
            }
        }
        sortUnique(loop.exitBlocks);

        if (!loop.exitBlocks.empty()) {
            std::uint32_t merge = loop.exitBlocks.front();
            for (std::size_t i = 1; i < loop.exitBlocks.size() && merge != InvalidControlFlowId; ++i) {
                merge = graph.FindNearestCommonPostDominator(merge, loop.exitBlocks[i]);
            }
            loop.mergeBlock = merge;
        }

        graph.naturalLoops.push_back(std::move(loop));
    }
}

void Structurizer::computePostDominators(ControlFlowGraph& graph) const {
    const auto count = static_cast<std::uint32_t>(graph.blocks.size());
    if (inPlaceIntersections() && dominatorTrees()) {
        std::vector<std::uint32_t> exits;
        for (const auto& block : graph.blocks) {
            if (block.successors.empty()) exits.push_back(block.id);
        }
        if (assignTreeDominators(
                graph.blocks,
                exits,
                [&](std::uint32_t id) -> const std::vector<std::uint32_t>& { return graph.blocks[id].successors; },
                [&](std::uint32_t id) -> const std::vector<std::uint32_t>& { return graph.blocks[id].predecessors; },
                graph.pdomTree,
                &BasicBlock::immediatePostDominator)) {
            return;
        }
    }
    graph.pdomTree.valid = false;
    for (auto& block : graph.blocks) block.immediatePostDominator = InvalidControlFlowId;
    const auto order = analysisOrder(graph, true);
    if (inPlaceIntersections()) {
        std::vector<BlockSet> sets;
        sets.reserve(count);
        for (const auto& block : graph.blocks) sets.push_back(block.successors.empty() ? BlockSet::only(count, block.id) : BlockSet::all(count));
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto id : order) {
                auto& block = graph.blocks[id];
                BlockSet next = block.successors.empty() ? BlockSet::only(count, block.id) : sets[block.successors.front()];
                if (!block.successors.empty()) {
                    for (std::size_t i = 1; i < block.successors.size(); ++i) next.intersect(sets[block.successors[i]]);
                    next.add(block.id);
                }
                changed = replaceSet(sets[id], std::move(next)) || changed;
            }
        }
        for (std::uint32_t id = 0; id < count; ++id) graph.blocks[id].postDominators = sets[id].ids();
        return;
    }

    const auto all = allBlockIds(count);
    for (auto& block : graph.blocks) block.postDominators = block.successors.empty() ? std::vector<std::uint32_t>{block.id} : all;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto id : order) {
            auto& block = graph.blocks[id];
            std::vector<std::uint32_t> next;
            if (block.successors.empty()) {
                next = {block.id};
            } else {
                next = graph.blocks[block.successors.front()].postDominators;
                for (std::size_t i = 1; i < block.successors.size(); ++i) intersectAnalysisSet(next, graph.blocks[block.successors[i]].postDominators);
                addSortedUnique(next, block.id);
            }
            if (next != block.postDominators) {
                replaceAnalysisSet(block.postDominators, std::move(next));
                changed = true;
            }
        }
    }
}

void Structurizer::computeBackEdges(ControlFlowGraph& graph) const {
    graph.backEdges.clear();
    for (const auto& block : graph.blocks) {
        for (const auto successor : block.successors) {
            if (graph.Dominates(successor, block.id)) {
                graph.backEdges.push_back({block.id, successor, true});
            }
        }
    }
}

void Structurizer::computeStronglyConnectedComponents(ControlFlowGraph& graph, bool buildComponents) const {
    if (!buildComponents) {
        const auto count = static_cast<std::uint32_t>(graph.blocks.size());
        thread_local std::vector<std::uint32_t> index;
        thread_local std::vector<std::uint32_t> lowlink;
        thread_local std::vector<std::uint8_t> onStack;
        thread_local std::vector<std::uint32_t> memberMark;
        thread_local std::vector<std::uint32_t> stack;
        thread_local std::vector<std::uint32_t> sccMembers;
        index.assign(count, InvalidControlFlowId);
        lowlink.assign(count, InvalidControlFlowId);
        onStack.assign(count, 0u);
        if (memberMark.size() < count) memberMark.assign(count, 0u);
        stack.clear();
        stack.reserve(count);
        std::uint32_t nextIndex = 0u;
        thread_local std::uint32_t mark = 0u;
        if (mark > std::numeric_limits<std::uint32_t>::max() - count - 2u) {
            memberMark.assign(count, 0u);
            mark = 0u;
        }
        struct Frame {
            std::uint32_t blockId;
            const std::vector<std::uint32_t>* successors;
            std::size_t next;
        };
        thread_local std::vector<Frame> work;
        work.clear();
        graph.irreducible = false;
        const auto enter = [&](std::uint32_t blockId) {
            index[blockId] = nextIndex;
            lowlink[blockId] = nextIndex;
            ++nextIndex;
            stack.push_back(blockId);
            onStack[blockId] = 1u;
            work.push_back({blockId, &graph.blocks[blockId].successors, 0u});
        };
        for (std::uint32_t root = 0; root < count; ++root) {
            if (index[root] != InvalidControlFlowId) continue;
            enter(root);
            while (!work.empty()) {
                auto& frame = work.back();
                const auto blockId = frame.blockId;
                if (frame.next < frame.successors->size()) {
                    const auto successor = (*frame.successors)[frame.next++];
                    if (index[successor] == InvalidControlFlowId) {
                        enter(successor);
                    } else if (onStack[successor]) {
                        lowlink[blockId] = std::min(lowlink[blockId], index[successor]);
                    }
                    continue;
                }
                work.pop_back();
                if (!work.empty()) {
                    auto& parent = lowlink[work.back().blockId];
                    parent = std::min(parent, lowlink[blockId]);
                }
                if (lowlink[blockId] == index[blockId]) {
                    if (stack.back() == blockId) {
                        stack.pop_back();
                        onStack[blockId] = 0u;
                        continue;
                    }
                    sccMembers.clear();
                    ++mark;
                    for (;;) {
                        const auto member = stack.back();
                        stack.pop_back();
                        onStack[member] = 0u;
                        memberMark[member] = mark;
                        sccMembers.push_back(member);
                        if (member == blockId) break;
                    }
                    std::uint32_t entryCount = 0u;
                    std::uint32_t minEntry = InvalidControlFlowId;
                    for (const auto member : sccMembers) {
                        bool isEntry = false;
                        for (const auto predecessor : graph.blocks[member].predecessors) {
                            if (memberMark[predecessor] != mark) {
                                isEntry = true;
                                break;
                            }
                        }
                        if (isEntry) {
                            ++entryCount;
                            minEntry = std::min(minEntry, member);
                        }
                    }
                    if (entryCount > 1u) {
                        graph.irreducible = true;
                        graph.failureKind = FailureKind::IrreducibleControlFlow;
                        graph.failureBlock = minEntry;
                        graph.unsupportedReason = "irreducible CFG: cyclic component has multiple entries";
                        return;
                    }
                }
            }
        }
        return;
    }

    TarjanState state;
    state.graph = &graph;
    state.index.assign(graph.blocks.size(), InvalidControlFlowId);
    state.lowlink.assign(graph.blocks.size(), InvalidControlFlowId);
    state.onStack.assign(graph.blocks.size(), false);
    state.stack.reserve(graph.blocks.size());
    state.components.reserve(graph.blocks.size());

    for (const auto& block : graph.blocks) {
        if (state.index[block.id] == InvalidControlFlowId) {
            tarjanVisit(state, block.id);
        }
    }

    graph.components = std::move(state.components);
    graph.irreducible = false;
    for (const auto& component : graph.components) {
        if (component.irreducible) {
            graph.irreducible = true;
            graph.failureKind = FailureKind::IrreducibleControlFlow;
            graph.failureBlock = component.entryBlocks.empty() ? component.blocks.front() : component.entryBlocks.front();
            graph.unsupportedReason = "irreducible CFG: cyclic component has multiple entries";
            break;
        }
    }
}

void Structurizer::recomputeAnalyses(ControlFlowGraph& graph) const {
    StructProf::Scope prof("recomputeAnalyses");
    { StructProf::Scope p1("ra:dominators"); computeDominatorTree(graph); }
    { StructProf::Scope p2("ra:postDominators"); computePostDominators(graph); }
    { StructProf::Scope p3("ra:backEdges"); computeBackEdges(graph); }
    { StructProf::Scope p4("ra:naturalLoops"); detectNaturalLoops(graph); }
    { StructProf::Scope p5("ra:scc"); computeStronglyConnectedComponents(graph, false); }
}

void Structurizer::materializeAnalyses(ControlFlowGraph& graph) const {
    materializeTreeSets(graph.blocks, graph.domTree, &BasicBlock::dominators);
    materializeTreeSets(graph.blocks, graph.pdomTree, &BasicBlock::postDominators);
    computeStronglyConnectedComponents(graph, true);
}

void Structurizer::canonicalizeNaturalLoops(ControlFlowGraph& graph, bool routeLeavingExits, bool routeSharedExits) const {
    StructProf::Scope prof("canonicalizeNaturalLoops");
    const auto rewriteBudget = graph.blocks.size() * 2u + 16u;
    for (std::size_t rewrite = 0; rewrite < rewriteBudget; ++rewrite) {
        bool changed = false;
        for (const auto& loop : graph.naturalLoops) {
            std::vector<std::uint32_t> latches;
            for (const auto& edge : graph.backEdges) {
                if (edge.targetBlock == loop.headerBlock) {
                    addUnique(latches, edge.sourceBlock);
                }
            }
            const bool latchInInnerLoop = routeSharedExits && latches.size() == 1u && std::any_of(graph.naturalLoops.begin(), graph.naturalLoops.end(), [&](const NaturalLoop& inner) {
                return inner.headerBlock != loop.headerBlock && contains(loop.blocks, inner.headerBlock) && contains(inner.blocks, latches.front());
            });
            const auto& header = graph.FindBlock(loop.headerBlock);
            const bool semanticSelfLoop = latches.size() == 1u && latches.front() == header.id && header.instructionBegin != header.instructionEnd;
            const bool semanticLatch = latches.size() == 1u && graph.FindBlock(latches.front()).instructionBegin != graph.FindBlock(latches.front()).instructionEnd && graph.FindBlock(latches.front()).predecessors.size() == 1u;
            if (latches.size() <= 1u && !latchInInnerLoop && !semanticSelfLoop && !semanticLatch) {
                continue;
            }

            const auto continueBlock = appendSyntheticBranchBlock(graph, loop.headerBlock);
            for (const auto latch : latches) {
                auto& block = graph.FindBlock(latch);
                replaceValue(block.successors, loop.headerBlock, continueBlock);
                replaceTerminatorTarget(block.terminator, loop.headerBlock, continueBlock);
            }
            rebuildPredecessors(graph);
            recomputeAnalyses(graph);
            changed = true;
            break;
        }
        if (changed) {
            continue;
        }

        for (const auto& loop : graph.naturalLoops) {
            const auto& header = graph.FindBlock(loop.headerBlock);
            const auto isLoopControlTarget = [&](std::uint32_t target) {
                return target == loop.mergeBlock || target == loop.continueBlock;
            };
            if (header.terminator.kind != TerminatorKind::ConditionalBranch || isLoopControlTarget(header.terminator.trueBlock) || isLoopControlTarget(header.terminator.falseBlock) || !contains(loop.blocks, header.terminator.trueBlock) || !contains(loop.blocks, header.terminator.falseBlock)) {
                continue;
            }

            if (!isolateSemanticLoopHeader(graph, loop.headerBlock, [this](ControlFlowGraph& innerGraph) { recomputeAnalyses(innerGraph); })) {
                throw std::runtime_error("failed to isolate semantic loop header " + std::to_string(loop.headerBlock));
            }
            changed = true;
            break;
        }
        if (changed) {
            continue;
        }

        const NaturalLoop* unmerged = nullptr;
        for (const auto& loop : graph.naturalLoops) {
            if (loop.exitBlocks.size() > 1u && (loop.mergeBlock == InvalidControlFlowId || (routeLeavingExits && (exitsLeaveConstruct(graph, loop) || (routeSharedExits && hasSharedExitSelection(graph, loop))))) && (unmerged == nullptr || loop.blocks.size() < unmerged->blocks.size())) {
                unmerged = &loop;
            }
        }
        if (unmerged == nullptr) {
            return;
        }
        std::string refusal;
        auto routed = routeLoopExits(graph, *unmerged, [this](ControlFlowGraph& candidate) { recomputeAnalyses(candidate); }, refusal);
        if (!routed) {
            const auto exits = unmerged->mergeBlock == InvalidControlFlowId ? std::string("with no common post-dominator") : "that leave the loop before its merge block " + std::to_string(unmerged->mergeBlock);
            throw std::runtime_error("loop at block " + std::to_string(unmerged->headerBlock) + " has exits " + exits + ": " + refusal);
        }
        graph = std::move(*routed);
    }

    throw std::runtime_error("CFG loop canonicalization exceeded rewrite budget");
}

void Structurizer::splitSharedMergeBlocks(ControlFlowGraph& graph, bool routeLeavingExits, bool routeSharedExits, bool relaxReturnJoins) const {
    StructProf::Scope prof("splitSharedMergeBlocks");
    const auto originalBlockCount = static_cast<std::uint32_t>(graph.blocks.size());
    const auto splitBudget = std::max<std::uint32_t>(StructBudget::percent < 400u ? 64u : 16u, static_cast<std::uint32_t>(static_cast<std::uint64_t>(originalBlockCount) * StructBudget::percent / 100u));
    const auto growthBudget = std::max<std::uint32_t>(SplitGrowthFloor, originalBlockCount * SplitGrowthFactor);
    const auto programWords = estimatedSpirvWords(graph, allBlockIds(originalBlockCount));
    SplitBudget budget{programWords, std::max(CloneWordLimit, programWords / CloneBudgetDivisor), std::max(CloneWordLimit, programWords), nextGotoVariable(graph)};
    static const bool useCursor = std::getenv("XBOX_NO_SPLIT_CURSOR") == nullptr;
    // XBOX-FORK: stable block ids while splitting (the layout order is kept separately and applied once at the end).
    static const bool stableIds = std::getenv("XBOX_NO_STABLE_IDS") == nullptr;
    if (stableIds && graph.layoutOrder.empty()) {
        graph.layoutOrder.resize(graph.blocks.size());
        std::iota(graph.layoutOrder.begin(), graph.layoutOrder.end(), 0u);
    }
    struct LayoutGuard {
        ControlFlowGraph& graph;
        ~LayoutGuard() { try { materializeLayout(graph); } catch (...) {} }
    } layoutGuard{graph};
    std::size_t cursor = 0;
    std::size_t loopCursor = 0;
    std::vector<std::uint32_t> headerOrder;
    std::vector<std::uint32_t> cachedLoopHeaders;
    bool headerOrderValid = false;
    bool predsSynced = false;
    for (std::uint32_t splits = 0; splits < splitBudget; ++splits) {
        StructProf::Scope profSplit("splitOne*(search+apply)");
        // XBOX-FORK: both scans resume at the item that acted last. When neither finds anything the scans restart from 0
        // once (a completeness pass); only when that also finds nothing is the graph at the fixed point.
        bool simpleSplit = false;
        bool loopMerge;
        { StructProf::Scope pl("split:splitOneLoopMerge"); loopMerge = splitOneLoopMerge(graph, loopCursor); }
        if (loopMerge) {
            simpleSplit = true;
            headerOrderValid = false;
        }
        bool selection = false;
        if (!loopMerge) {
            selection = splitOneSelectionMerge(graph, budget, [this](ControlFlowGraph& candidate) { recomputeAnalyses(candidate); }, routeLeavingExits, routeSharedExits, cursor, headerOrder, headerOrderValid, cachedLoopHeaders, simpleSplit, predsSynced);
        }
        if (!loopMerge && !selection) {
            if (useCursor && (cursor != 0u || loopCursor != 0u)) {
                cursor = 0;
                loopCursor = 0;
                continue;
            }
            if (!splitOneReturnJoin(graph, relaxReturnJoins)) {
                const bool wasStable = !graph.layoutOrder.empty();
                { StructProf::Scope pf("split:materializeLayout"); materializeLayout(graph); }
                if (wasStable) recomputeAnalyses(graph);
                if (StructProf::enabled()) std::fprintf(stderr, "[struct] splits=%u orig=%u\n", splits, originalBlockCount);
                return;
            }
            headerOrderValid = false;
        }
        if (!useCursor) {
            cursor = 0;
            loopCursor = 0;
        }
        if (graph.blocks.size() > growthBudget) {
            throw std::runtime_error("CFG shared merge splitting exceeded growth budget: originalBlocks=" + std::to_string(originalBlockCount) + " currentBlocks=" + std::to_string(graph.blocks.size()) + " growthBudget=" + std::to_string(growthBudget));
        }
        if (!simpleSplit || !predsSynced) {
            rebuildPredecessors(graph);
            predsSynced = true;
        }
        recomputeAnalyses(graph);
        verifyReducibility(graph);
    }

    throw std::runtime_error("CFG shared merge splitting exceeded budget: originalBlocks=" + std::to_string(originalBlockCount) + " currentBlocks=" + std::to_string(graph.blocks.size()) + " splitBudget=" + std::to_string(splitBudget));
}

void Structurizer::isolateSemanticLoopHeaders(ControlFlowGraph& graph) const {
    const auto isolationBudget = graph.naturalLoops.size() + 1u;
    for (std::size_t isolation = 0; isolation < isolationBudget; ++isolation) {
        const auto loop = std::find_if(graph.naturalLoops.begin(), graph.naturalLoops.end(), [&](const NaturalLoop& value) {
            const auto& header = graph.FindBlock(value.headerBlock);
            return header.instructionBegin != header.instructionEnd;
        });
        if (loop == graph.naturalLoops.end()) {
            return;
        }

        if (!isolateSemanticLoopHeader(graph, loop->headerBlock, [this](ControlFlowGraph& innerGraph) { recomputeAnalyses(innerGraph); })) {
            throw std::runtime_error("failed to isolate semantic loop header " + std::to_string(loop->headerBlock));
        }
    }

    throw std::runtime_error("CFG semantic loop-header isolation exceeded rewrite budget");
}

void Structurizer::orderByDominance(ControlFlowGraph& graph) const {
    const bool ordered = graph.domTree.valid && graph.blocks.size() == graph.domTree.parent.size()
        ? std::all_of(graph.blocks.begin(), graph.blocks.end(), [&](const BasicBlock& block) {
            return graph.domTree.parent[block.id] <= block.id;
        })
        : std::all_of(graph.blocks.begin(), graph.blocks.end(), [](const BasicBlock& block) {
            return std::all_of(block.dominators.begin(), block.dominators.end(), [&](std::uint32_t dominator) { return dominator <= block.id; });
        });
    if (ordered) {
        return;
    }
    const auto order = reversePostOrder(graph);
    if (order.size() != graph.blocks.size()) {
        throw std::runtime_error("control flow graph has unreachable blocks");
    }
    std::vector<BasicBlock> blocks;
    blocks.reserve(order.size());
    for (const auto blockId : order) {
        blocks.push_back(graph.blocks[blockId]);
    }
    applyBlockOrder(graph, std::move(blocks));
    recomputeAnalyses(graph);
}

void Structurizer::clearStructuredTerminators(ControlFlowGraph& graph) const {
    for (auto& block : graph.blocks) {
        block.terminator.mergeBlock = InvalidControlFlowId;
        block.terminator.continueBlock = InvalidControlFlowId;
        block.terminator.loopHeader = false;
    }
}

void Structurizer::verifyReducibility(const ControlFlowGraph& graph) const {
    StructProf::Scope prof("verifyReducibility");
    if (graph.unsupported || graph.irreducible) {
        throw std::runtime_error(graph.unsupportedReason.empty() ? std::string("unsupported CFG") : graph.unsupportedReason);
    }
}

bool Structurizer::hasStructuredConstructs(const ControlFlowGraph& graph) const {
    std::vector<const BasicBlock*> loopHeaders;
    for (const auto& block : graph.blocks) {
        if (block.terminator.loopHeader) loopHeaders.push_back(&block);
    }
    const auto innermostLoop = [&](const ControlFlowGraph& analysis, std::uint32_t blockId) {
        const BasicBlock* innermost = nullptr;
        for (const auto* header : loopHeaders) {
            if (analysis.Dominates(header->id, blockId) && !analysis.Dominates(header->terminator.mergeBlock, blockId) && (innermost == nullptr || analysis.Dominates(innermost->id, header->id))) innermost = header;
        }
        return innermost;
    };
    for (const auto& block : graph.blocks) {
        const auto& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::ConditionalBranch || terminator.loopHeader || terminator.mergeBlock != InvalidControlFlowId || terminator.trueBlock == terminator.falseBlock) continue;
        const auto* loop = innermostLoop(graph, block.id);
        if (loop == nullptr || (terminator.trueBlock != loop->terminator.mergeBlock && terminator.trueBlock != loop->terminator.continueBlock && terminator.falseBlock != loop->terminator.mergeBlock && terminator.falseBlock != loop->terminator.continueBlock)) return false;
    }
    auto structural = graph;
    for (auto& block : structural.blocks) {
        if (block.terminator.mergeBlock != InvalidControlFlowId) {
            block.successors.push_back(block.terminator.mergeBlock);
        }
        if (block.terminator.loopHeader) {
            block.successors.push_back(block.terminator.continueBlock);
        }
    }
    rebuildPredecessors(structural);
    computeDominatorTree(structural);
    computePostDominators(structural);
    const auto inRegion = [&](const BasicBlock& header, std::uint32_t blockId) {
        return structural.Dominates(header.id, blockId) && !structural.Dominates(header.terminator.mergeBlock, blockId);
    };
    const auto inContinue = [&](const BasicBlock& header, std::uint32_t blockId) {
        return structural.Dominates(header.terminator.continueBlock, blockId) && structural.PostDominates(header.terminator.continueBlock, blockId);
    };
    const auto inConstruct = [&](const BasicBlock& header, std::uint32_t blockId) {
        return inRegion(header, blockId) && !(header.terminator.loopHeader && inContinue(header, blockId));
    };
    const auto exitsLoop = [](const BasicBlock* loop, std::uint32_t target) {
        return loop != nullptr && (target == loop->terminator.mergeBlock || target == loop->terminator.continueBlock);
    };
    const bool walkConstructs = constructsFollowDominance(structural);
    for (const auto& block : graph.blocks) {
        const auto& terminator = block.terminator;
        for (const auto successor : block.successors) {
            const auto& target = graph.FindBlock(successor);
            if (structural.Dominates(successor, block.id) && (!target.terminator.loopHeader || target.terminator.continueBlock != block.id)) {
                return false;
            }
        }
        if (terminator.kind == TerminatorKind::ConditionalBranch && !terminator.loopHeader && terminator.mergeBlock == InvalidControlFlowId && terminator.trueBlock != terminator.falseBlock) {
            const auto* loop = innermostLoop(structural, block.id);
            if (!exitsLoop(loop, terminator.trueBlock) && !exitsLoop(loop, terminator.falseBlock)) {
                return false;
            }
        }
        if (terminator.mergeBlock == InvalidControlFlowId) {
            continue;
        }
        if (terminator.mergeBlock == block.id || !structural.Dominates(block.id, terminator.mergeBlock)) {
            return false;
        }
        if (terminator.loopHeader && (terminator.continueBlock == terminator.mergeBlock || !structural.Dominates(block.id, terminator.continueBlock))) {
            return false;
        }
        const auto* loop = terminator.loopHeader ? &block : innermostLoop(structural, block.id);
        const auto memberFits = [&](const BasicBlock& member) {
            if (inConstruct(block, member.id)) {
                if (member.id != block.id && member.terminator.mergeBlock != InvalidControlFlowId && !inConstruct(block, member.terminator.mergeBlock)) {
                    return false;
                }
                if (member.id != block.id && !std::all_of(member.predecessors.begin(), member.predecessors.end(), [&](std::uint32_t predecessor) { return inConstruct(block, predecessor); })) {
                    return false;
                }
                if (!std::all_of(member.successors.begin(), member.successors.end(), [&](std::uint32_t successor) { return inConstruct(block, successor) || successor == terminator.mergeBlock || exitsLoop(loop, successor); })) {
                    return false;
                }
            } else if (terminator.loopHeader && inContinue(block, member.id)) {
                if (!std::all_of(member.successors.begin(), member.successors.end(), [&](std::uint32_t successor) { return inContinue(block, successor) || successor == block.id || successor == terminator.mergeBlock; })) {
                    return false;
                }
            }
            return true;
        };
        if (walkConstructs) {
            auto members = dominatedBlocks(structural, block.id, terminator.mergeBlock);
            if (terminator.loopHeader) {
                const auto continueMembers = dominatedBlocks(structural, terminator.continueBlock);
                members.insert(members.end(), continueMembers.begin(), continueMembers.end());
                sortUnique(members);
            }
            if (!std::all_of(members.begin(), members.end(), [&](std::uint32_t memberId) { return memberFits(graph.FindBlock(memberId)); })) {
                return false;
            }
        } else if (!std::all_of(graph.blocks.begin(), graph.blocks.end(), memberFits)) {
            return false;
        }
        if (terminator.loopHeader) {
            const auto& predecessors = graph.FindBlock(terminator.continueBlock).predecessors;
            if (!std::all_of(predecessors.begin(), predecessors.end(), [&](std::uint32_t predecessor) { return inConstruct(block, predecessor) || inContinue(block, predecessor); })) {
                return false;
            }
        }
    }
    return true;
}

}
