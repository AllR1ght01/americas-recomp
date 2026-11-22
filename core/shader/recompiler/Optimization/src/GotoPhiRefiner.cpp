#include "Optimization/GotoPhiRefiner.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

namespace {

constexpr std::size_t MaxRefinementDepth = 64;
constexpr std::size_t MaxRefinementRounds = 32;
constexpr std::size_t None = static_cast<std::size_t>(-1);

struct Fact {
    const IrValue* condition = nullptr;
    bool value = false;
};

struct Memo {
    IrValue* result = nullptr;
    std::vector<std::size_t> relied;
    bool done = false;
};

IrValue* Reuse(const Memo& memo, std::set<std::size_t>& relied, IrValue* pending) {
    if (!memo.done) return pending;
    relied.insert(memo.relied.begin(), memo.relied.end());
    return memo.result;
}

std::vector<IrBlock*> ReversePostOrder(IrBlock& entry) {
    std::vector<IrBlock*> order;
    std::unordered_map<const IrBlock*, bool> visited;
    std::vector<std::pair<IrBlock*, std::size_t>> stack{{&entry, 0u}};
    visited[&entry] = true;
    while (!stack.empty()) {
        auto& [block, next] = stack.back();
        if (next < block->Successors().size()) {
            IrBlock* successor = block->Successors()[next++];
            if (visited.try_emplace(successor, true).second) stack.emplace_back(successor, 0u);
            continue;
        }
        order.push_back(block);
        stack.pop_back();
    }
    return {order.rbegin(), order.rend()};
}

IrValue* PhiIncoming(const IrValue& phi, const IrBlock* predecessor) {
    for (std::size_t index = 0; index < phi.ArgumentCount(); index++) {
        if (phi.PhiBlock(index) == predecessor) return phi.Argument(index);
    }
    return nullptr;
}

class Refinement {
public:
    explicit Refinement(IrProgram& program) : m_program(program) {}

    std::uint32_t Run() {
        if (m_program.BlockOrder().empty()) return 0;
        m_order = ReversePostOrder(*m_program.BlockOrder().front());
        for (std::size_t position = 0; position < m_order.size(); ++position) m_index[m_order[position]] = position;
        ComputeDominators();
        const auto facts = CollectFacts();
        std::map<std::vector<std::pair<const IrValue*, bool>>, std::size_t> factSets;
        std::uint32_t replaced = 0;
        for (std::size_t position = 0; position < m_order.size(); ++position) {
            if (facts[position].empty()) continue;
            std::vector<std::pair<const IrValue*, bool>> key;
            for (const auto& fact : facts[position]) key.emplace_back(fact.condition, fact.value);
            const auto factSet = factSets.try_emplace(std::move(key), factSets.size()).first->second;
            for (IrValue* inst : m_order[position]->Instructions()) {
                if (inst->IsPhi()) continue;
                for (std::size_t operand = 0; operand < inst->ArgumentCount(); ++operand) {
                    IrValue* argument = inst->Argument(operand);
                    if (argument == nullptr) continue;
                    IrValue* value = argument->Resolve();
                    if (!value->IsPhi() || value->Type() == IrType::Bool) continue;
                    if (IrValue* refined = Refined(value, position, facts[position], factSet); refined != nullptr && refined != value) {
                        inst->ReplaceArgument(operand, refined);
                        ++replaced;
                    }
                }
            }
        }
        return replaced;
    }

private:
    void ComputeDominators() {
        m_dominator.assign(m_order.size(), None);
        m_dominator[0] = 0;
        const auto intersect = [&](std::size_t left, std::size_t right) {
            while (left != right) {
                while (left > right) left = m_dominator[left];
                while (right > left) right = m_dominator[right];
            }
            return left;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t position = 1; position < m_order.size(); ++position) {
                std::size_t candidate = None;
                for (const IrBlock* predecessor : m_order[position]->Predecessors()) {
                    const auto found = m_index.find(predecessor);
                    if (found == m_index.end() || m_dominator[found->second] == None) continue;
                    candidate = candidate == None ? found->second : intersect(candidate, found->second);
                }
                if (candidate != m_dominator[position]) {
                    m_dominator[position] = candidate;
                    changed = true;
                }
            }
        }
        std::vector<std::vector<std::size_t>> children(m_order.size());
        for (std::size_t position = 1; position < m_order.size(); ++position) {
            if (m_dominator[position] != None) children[m_dominator[position]].push_back(position);
        }
        m_enter.assign(m_order.size(), 0);
        m_leave.assign(m_order.size(), 0);
        std::size_t clock = 0;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{0u, 0u}};
        m_enter[0] = clock++;
        while (!stack.empty()) {
            auto& [block, next] = stack.back();
            if (next < children[block].size()) {
                const auto child = children[block][next++];
                m_enter[child] = clock++;
                stack.emplace_back(child, 0u);
                continue;
            }
            m_leave[block] = clock++;
            stack.pop_back();
        }
    }

    bool Dominates(std::size_t dominator, std::size_t block) const {
        return m_enter[dominator] <= m_enter[block] && m_leave[block] <= m_leave[dominator];
    }

    const std::vector<bool>& ReachableAvoiding(std::size_t from, std::size_t barrier) {
        const auto key = std::make_pair(from, barrier);
        if (const auto found = m_reachable.find(key); found != m_reachable.end()) return found->second;
        std::vector<bool> reached(m_order.size(), false);
        std::vector<std::size_t> pending{from};
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            for (const IrBlock* successor : m_order[current]->Successors()) {
                const auto found = m_index.find(successor);
                if (found == m_index.end() || found->second == barrier || found->second == from || reached[found->second]) continue;
                reached[found->second] = true;
                pending.push_back(found->second);
            }
        }
        return m_reachable.emplace(key, std::move(reached)).first->second;
    }

    std::vector<std::vector<Fact>> CollectFacts() {
        struct GuardEdge {
            std::size_t branch = 0;
            std::size_t edge = 0;
            const IrValue* condition = nullptr;
            bool value = false;
        };
        std::vector<std::vector<Fact>> facts(m_order.size());
        const auto& programOrder = m_program.BlockOrder();
        const auto& info = m_program.Metadata().blockInfo;
        if (programOrder.size() != info.size()) return facts;
        std::unordered_map<std::uint32_t, IrBlock*> byId;
        for (std::size_t index = 0; index < programOrder.size(); ++index) byId.emplace(info[index].id, programOrder[index]);
        const auto hasFact = [&](std::size_t position, const IrValue* condition, bool value) {
            for (const auto& fact : facts[position]) {
                if (fact.condition == condition && fact.value == value) return true;
            }
            return false;
        };
        const auto addFact = [&](std::size_t position, const IrValue* condition, bool value) {
            if (hasFact(position, condition, value) || hasFact(position, condition, !value)) return false;
            facts[position].push_back(Fact{condition, value});
            return true;
        };
        std::vector<GuardEdge> guards;
        for (std::size_t index = 0; index < programOrder.size(); ++index) {
            const auto& terminator = info[index].terminator;
            if (terminator.kind != TerminatorKind::ConditionalBranch || info[index].condition == nullptr || terminator.trueBlock == terminator.falseBlock) continue;
            const IrValue* condition = info[index].condition->Resolve();
            if (!condition->IsPhi() || condition->Type() != IrType::Bool) continue;
            const auto branch = m_index.find(programOrder[index]);
            if (branch == m_index.end()) continue;
            const auto trueIt = byId.find(terminator.trueBlock);
            const auto falseIt = byId.find(terminator.falseBlock);
            if (trueIt != byId.end() && falseIt != byId.end()) {
                m_branches.emplace(programOrder[index], BranchInfo{condition, trueIt->second, falseIt->second});
            }
            for (const auto& [target, value] : {std::pair{terminator.trueBlock, true}, std::pair{terminator.falseBlock, false}}) {
                const auto block = byId.find(target);
                if (block == byId.end() || block->second->Predecessors().size() != 1u || block->second->Predecessors().front() != programOrder[index]) continue;
                const auto edge = m_index.find(block->second);
                if (edge == m_index.end()) continue;
                guards.push_back(GuardEdge{branch->second, edge->second, condition, value});
                for (std::size_t position = 0; position < m_order.size(); ++position) {
                    if (Dominates(edge->second, position)) addFact(position, condition, value);
                }
            }
        }
        const auto impliesFact = [&](const auto& self, const IrValue* phi, bool expected, const IrValue* targetCond, bool targetVal, std::vector<std::pair<const IrValue*, bool>>& visiting, std::uint32_t& budget) -> bool {
            if (phi == targetCond && expected == targetVal) return true;
            if (budget == 0u || phi == nullptr || !phi->IsPhi() || phi->Type() != IrType::Bool || phi->ArgumentCount() == 0u) return false;
            const auto pair = std::make_pair(phi, expected);
            for (const auto& active : visiting) {
                if (active == pair) return true;
            }
            const IrBlock* joinBlock = phi->Parent();
            const IrBlock* targetParent = targetCond->Parent();
            if (joinBlock == nullptr || targetParent == nullptr || targetParent == joinBlock) return false;
            const auto targetBlock = m_index.find(targetParent);
            if (targetBlock == m_index.end()) return false;
            --budget;
            visiting.push_back(pair);
            bool all = true;
            for (std::size_t operand = 0; all && operand < phi->ArgumentCount(); ++operand) {
                const IrBlock* predecessor = phi->PhiBlock(operand);
                const IrValue* incoming = phi->Argument(operand)->Resolve();
                if (incoming->HasImmediate() && incoming->ImmediateBool() != expected) continue;
                const auto pred = m_index.find(predecessor);
                if (pred == m_index.end()) {
                    all = false;
                    break;
                }
                if (!incoming->HasImmediate() && hasFact(pred->second, incoming, !expected)) continue;
                if (hasFact(pred->second, targetCond, targetVal)) continue;
                bool proven = false;
                if (!incoming->HasImmediate() && incoming->IsPhi() && incoming->Type() == IrType::Bool) {
                    const IrBlock* inParent = incoming->Parent();
                    const auto inBlock = inParent != nullptr ? m_index.find(inParent) : m_index.end();
                    if (inBlock != m_index.end() &&
                        (inBlock->second == pred->second || !ReachableAvoiding(inBlock->second, pred->second)[targetBlock->second]) &&
                        self(self, incoming, expected, targetCond, targetVal, visiting, budget)) {
                        proven = true;
                    }
                }
                if (!proven) {
                    for (const auto& active : visiting) {
                        if (!hasFact(pred->second, active.first, active.second)) continue;
                        const IrBlock* factParent = active.first->Parent();
                        const auto factBlock = factParent != nullptr ? m_index.find(factParent) : m_index.end();
                        if (factBlock == m_index.end() || factBlock->second == targetBlock->second || Dominates(factBlock->second, targetBlock->second)) continue;
                        if (factBlock->second != pred->second && ReachableAvoiding(factBlock->second, pred->second)[targetBlock->second]) continue;
                        proven = true;
                        break;
                    }
                }
                if (!proven) {
                    const auto& predFacts = facts[pred->second];
                    for (std::size_t fi = predFacts.size(); fi-- > 0u;) {
                        const auto& predFact = predFacts[fi];
                        const IrBlock* factParent = predFact.condition->Parent();
                        const auto factBlock = factParent != nullptr ? m_index.find(factParent) : m_index.end();
                        if (factBlock == m_index.end() || factBlock->second == targetBlock->second || Dominates(factBlock->second, targetBlock->second)) continue;
                        if (factBlock->second != pred->second && ReachableAvoiding(factBlock->second, pred->second)[targetBlock->second]) continue;
                        if (self(self, predFact.condition, predFact.value, targetCond, targetVal, visiting, budget)) {
                            proven = true;
                            break;
                        }
                    }
                }
                if (!proven) all = false;
            }
            visiting.pop_back();
            return all;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& guard : guards) {
                const IrBlock* joinBlock = guard.condition->Parent();
                if (joinBlock == nullptr) continue;
                const auto join = m_index.find(joinBlock);
                if (join == m_index.end() || !Dominates(join->second, guard.branch)) continue;
                std::vector<std::pair<const IrValue*, bool>> candidates;
                std::vector<const IrValue*> worklist{guard.condition};
                std::set<const IrValue*> visitedPhis{guard.condition};
                const auto addCandidate = [&](const IrValue* cond, bool val) {
                    if (cond == guard.condition || cond->Parent() == joinBlock) return;
                    if (hasFact(guard.edge, cond, val) || hasFact(guard.edge, cond, !val)) return;
                    for (const auto& existing : candidates) {
                        if (existing.first == cond && existing.second == val) return;
                    }
                    candidates.emplace_back(cond, val);
                };
                while (!worklist.empty() && visitedPhis.size() <= 64u) {
                    const IrValue* cur = worklist.back();
                    worklist.pop_back();
                    for (std::size_t operand = 0; operand < cur->ArgumentCount(); ++operand) {
                        const IrBlock* predecessor = cur->PhiBlock(operand);
                        const IrValue* incoming = cur->Argument(operand)->Resolve();
                        if (incoming->HasImmediate() && incoming->ImmediateBool() != guard.value) continue;
                        const auto pred = m_index.find(predecessor);
                        if (pred == m_index.end()) continue;
                        if (!incoming->HasImmediate() && hasFact(pred->second, incoming, !guard.value)) continue;
                        for (const auto& fact : facts[pred->second]) addCandidate(fact.condition, fact.value);
                        if (!incoming->HasImmediate() && incoming->IsPhi() && incoming->Type() == IrType::Bool) {
                            addCandidate(incoming, guard.value);
                            if (visitedPhis.insert(incoming).second) worklist.push_back(incoming);
                        }
                    }
                }
                for (const auto& [cond, val] : candidates) {
                    const auto condBlock = m_index.find(cond->Parent());
                    if (condBlock == m_index.end() || Dominates(guard.edge, condBlock->second)) continue;
                    if (join->second != guard.branch && ReachableAvoiding(join->second, guard.branch)[condBlock->second]) continue;
                    std::vector<std::pair<const IrValue*, bool>> visiting;
                    std::uint32_t budget = 1024u;
                    if (!impliesFact(impliesFact, guard.condition, guard.value, cond, val, visiting, budget)) continue;
                    for (std::size_t position = 0; position < m_order.size(); ++position) {
                        if (Dominates(guard.edge, position) && addFact(position, cond, val)) {
                            changed = true;
                        }
                    }
                }
            }
        }
        return facts;
    }

    IrValue* Refined(IrValue* value, std::size_t use, const std::vector<Fact>& facts, std::size_t factSet) {
        std::set<std::size_t> relied;
        IrValue* refined = RefineValue(value, facts, factSet, relied, 0u);
        if (refined == nullptr || refined == value) return nullptr;
        refined = refined->Resolve();
        if (refined->Type() != value->Type()) return nullptr;
        if (!refined->HasImmediate() && refined->Parent() != nullptr) {
            const auto defined = m_index.find(refined->Parent());
            if (defined == m_index.end() || defined->second == use || !Dominates(defined->second, use)) return nullptr;
            relied.insert(defined->second);
        }
        const auto junction = m_index.find(value->Parent());
        if (junction == m_index.end()) return nullptr;
        for (const auto block : relied) {
            if (block != junction->second && ReachableAvoiding(block, junction->second)[use]) return nullptr;
        }
        return refined;
    }

    IrValue* RefineValue(IrValue* value, const std::vector<Fact>& facts, std::size_t factSet, std::set<std::size_t>& relied, std::size_t depth) {
        value = value->Resolve();
        if (!value->IsPhi() || depth > MaxRefinementDepth) return value;
        const auto [entry, inserted] = m_refined.try_emplace({factSet, value});
        if (!inserted) return Reuse(entry->second, relied, value);
        IrValue* result = value;
        std::set<std::size_t> own;
        if (const auto block = m_index.find(value->Parent()); block != m_index.end()) {
            for (const auto& fact : facts) {
                if (fact.condition == value || fact.condition->Parent() != value->Parent()) continue;
                std::set<std::size_t> attempt;
                if (IrValue* restricted = Restrict(*value, *fact.condition, fact.value, facts, factSet, attempt, depth + 1u)) {
                    attempt.insert(block->second);
                    result = RefineValue(restricted, facts, factSet, attempt, depth + 1u);
                    own = std::move(attempt);
                    break;
                }
            }
            if (result == value) {
                for (const auto& fact : facts) {
                    if (fact.condition == value || fact.condition->Parent() == value->Parent() || fact.condition->Parent() == nullptr) continue;
                    const auto condBlock = m_index.find(fact.condition->Parent());
                    if (condBlock == m_index.end() || !Dominates(condBlock->second, block->second)) continue;
                    std::set<std::size_t> attempt;
                    IrValue* single = nullptr;
                    bool excludedAny = false;
                    bool ok = true;
                    for (std::size_t index = 0; index < value->ArgumentCount(); ++index) {
                        const IrBlock* predecessor = value->PhiBlock(index);
                        const auto br = m_branches.find(predecessor);
                        if (br != m_branches.end() && br->second.condition == fact.condition) {
                            const IrBlock* wrongTarget = fact.value ? br->second.falseBlock : br->second.trueBlock;
                            const IrBlock* rightTarget = fact.value ? br->second.trueBlock : br->second.falseBlock;
                            if (wrongTarget == value->Parent() && rightTarget != value->Parent()) {
                                excludedAny = true;
                                continue;
                            }
                        }
                        IrValue* kept = RefineValue(value->Argument(index), facts, factSet, attempt, depth + 1u);
                        if (kept == nullptr) {
                            ok = false;
                            break;
                        }
                        kept = kept->Resolve();
                        if (single == nullptr) {
                            single = kept;
                        } else if (single != kept) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok && excludedAny && single != nullptr) {
                        attempt.insert(block->second);
                        attempt.insert(condBlock->second);
                        result = RefineValue(single, facts, factSet, attempt, depth + 1u);
                        own = std::move(attempt);
                        break;
                    }
                }
            }
        }
        entry->second = Memo{result, {own.begin(), own.end()}, true};
        relied.insert(own.begin(), own.end());
        return result;
    }

    IrValue* Restrict(const IrValue& phi, const IrValue& guardPhi, bool expected, const std::vector<Fact>& facts, std::size_t factSet, std::set<std::size_t>& relied, std::size_t depth) {
        if (depth > MaxRefinementDepth) return nullptr;
        const auto [entry, inserted] = m_restricted.try_emplace({factSet, &phi, &guardPhi, expected});
        if (!inserted) return Reuse(entry->second, relied, nullptr);
        std::set<std::size_t> own;
        IrValue* single = RestrictArms(phi, guardPhi, expected, facts, factSet, own, depth);
        entry->second = Memo{single, single != nullptr ? std::vector<std::size_t>(own.begin(), own.end()) : std::vector<std::size_t>{}, true};
        if (single != nullptr) relied.insert(own.begin(), own.end());
        return single;
    }

    IrValue* RestrictArms(const IrValue& phi, const IrValue& guardPhi, bool expected, const std::vector<Fact>& facts, std::size_t factSet, std::set<std::size_t>& relied, std::size_t depth) {
        IrValue* single = nullptr;
        for (std::size_t index = 0; index < phi.ArgumentCount(); ++index) {
            const IrBlock* predecessor = phi.PhiBlock(index);
            IrValue* guard = PhiIncoming(guardPhi, predecessor);
            if (guard == nullptr) return nullptr;
            guard = guard->Resolve();
            IrValue* arm = phi.Argument(index)->Resolve();
            IrValue* kept = nullptr;
            if (guard->HasImmediate()) {
                if (guard->ImmediateBool() != expected) continue;
                kept = RefineValue(arm, facts, factSet, relied, depth + 1u);
            } else if (guard->IsPhi() && arm->IsPhi() && guard->Parent() == arm->Parent()) {
                const auto block = m_index.find(arm->Parent());
                if (block == m_index.end()) return nullptr;
                kept = Restrict(*arm, *guard, expected, facts, factSet, relied, depth + 1u);
                if (kept == nullptr) return nullptr;
                kept = kept->Resolve();
                if (!kept->HasImmediate() && kept->Parent() != nullptr) {
                    const auto defined = m_index.find(kept->Parent());
                    if (defined == m_index.end() || defined->second == block->second || !Dominates(defined->second, block->second)) return nullptr;
                }
                relied.insert(block->second);
            } else {
                kept = RefineValue(arm, facts, factSet, relied, depth + 1u);
            }
            if (kept == nullptr) return nullptr;
            kept = kept->Resolve();
            if (single == nullptr) {
                single = kept;
            } else if (single != kept) {
                return nullptr;
            }
        }
        return single;
    }

    struct BranchInfo {
        const IrValue* condition = nullptr;
        const IrBlock* trueBlock = nullptr;
        const IrBlock* falseBlock = nullptr;
    };

    IrProgram& m_program;
    std::vector<IrBlock*> m_order;
    std::unordered_map<const IrBlock*, std::size_t> m_index;
    std::unordered_map<const IrBlock*, BranchInfo> m_branches;
    std::vector<std::size_t> m_dominator;
    std::vector<std::size_t> m_enter;
    std::vector<std::size_t> m_leave;
    std::map<std::pair<std::size_t, std::size_t>, std::vector<bool>> m_reachable;
    std::map<std::pair<std::size_t, const IrValue*>, Memo> m_refined;
    std::map<std::tuple<std::size_t, const IrValue*, const IrValue*, bool>, Memo> m_restricted;
};

}

GotoPhiRefinementStats GotoPhiRefiner::Refine(IrProgram& program) const {
    GotoPhiRefinementStats stats;
    for (std::size_t round = 0; round < MaxRefinementRounds; ++round) {
        const std::uint32_t replaced = Refinement(program).Run();
        if (replaced == 0u) break;
        stats.replacedOperands += replaced;
    }
    return stats;
}

}
