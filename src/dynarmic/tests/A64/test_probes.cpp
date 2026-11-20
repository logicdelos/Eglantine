// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Instruction probes: host calls the JIT emits in front of chosen guest instructions, and the
// guarantee that a probe installed by a probe is seen by the very next entry into that code,
// however the installing instruction leaves its block.

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "A64/testenv.h"
#include "native/testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
using namespace Dynarmic::A64;

namespace {

constexpr u64 base = 0x1000;
constexpr u32 NOP = 0xd503201f;
constexpr u32 B_HERE = 0x14000000;
constexpr u32 RET = 0xd65f03c0;
constexpr u32 BR_X30 = 0xd61f03c0;
constexpr u32 ADD_X1_X1_1 = 0x91000421;
constexpr u32 MOV_X0_5 = 0xd28000a0;
constexpr u32 MOV_X2_7 = 0xd28000e2;

using Hit = std::array<u64, 3>;

/// Every branch the translator asked the probe table about: pc, kind, target or register.
std::vector<std::array<u64, 3>> g_branch_queries;

/// Probe lookup with a fixed insertion order, mutable between runs (single threaded here).
class ProbeTable final : public InstructionProbes {
public:
    struct Item {
        VAddr pc;
        InstructionProbe probe;
    };
    std::vector<Item> items;

    std::size_t Find(VAddr pc, InstructionProbeSet& out) const override {
        for (const Item& item : items) {
            if (item.pc == pc && out.count < out.probes.size()) {
                out.probes[out.count++] = item.probe;
            }
        }
        return out.count;
    }

    bool FindBranch(VAddr pc, BranchKind kind, std::uint64_t target_or_reg, InstructionProbe& out) const override {
        if (!audit_branches) {
            return false;
        }
        // pc, kind, target or register. A conditional branch is recorded, not probed: the census
        // fails coverage on a conditional entry instead, since it cannot tell if it was taken.
        g_branch_queries.push_back({pc, static_cast<u64>(kind), target_or_reg});
        if (kind == BranchKind::Conditional) {
            return false;
        }
        out.fn = &OnAudit;
        out.operands[0] = kind == BranchKind::Direct ? InstructionProbe::Operand{InstructionProbe::Arg::Imm, target_or_reg}
                                                     : InstructionProbe::Operand{InstructionProbe::Arg::XReg, target_or_reg};
        out.operands[1] = {InstructionProbe::Arg::Imm, pc};
        out.operands[2] = {InstructionProbe::Arg::Imm, kind == BranchKind::Direct ? 1u : kind == BranchKind::Return ? 3u : 2u};
        return true;
    }

    void Add(VAddr pc, InstructionProbe probe) {
        items.push_back({pc, probe});
    }

    bool audit_branches = false;
    static void OnAudit(u64 target, u64 pc, u64 kind);
};

ProbeTable* g_table = nullptr;
A64::Jit* g_jit = nullptr;
std::array<std::vector<Hit>, 4> g_hits;
std::vector<Hit> g_audit;
std::vector<std::size_t> g_order;

void ProbeTable::OnAudit(u64 target, u64 pc, u64 kind) {
    g_audit.push_back({target, pc, kind});
}

template<std::size_t Id>
void Record(u64 a, u64 b, u64 c) {
    g_hits[Id].push_back({a, b, c});
    g_order.push_back(Id);
}

void OnStop(u64, u64, u64) {
    g_jit->HaltExecution();
}

/// Like a census role that installs another probe: add one at `target` and invalidate it.
void InstallAtLr(u64 lr, u64 sp, u64) {
    g_hits[0].push_back({lr, sp, 0});
    InstructionProbe next;
    next.fn = &Record<1>;
    next.operands = {InstructionProbe::Operand{InstructionProbe::Arg::X0, 0},
                     InstructionProbe::Operand{InstructionProbe::Arg::SP, 0},
                     InstructionProbe::Operand{InstructionProbe::Arg::X30, 0}};
    next.end_block_after = true;
    g_table->Add(lr, next);
    g_jit->InvalidateCacheRange(lr, 4);
}

void InstallAtTarget(u64 target, u64, u64) {
    g_hits[0].push_back({target, 0, 0});
    InstructionProbe next;
    next.fn = &Record<1>;
    next.operands = {InstructionProbe::Operand{InstructionProbe::Arg::X0, 0},
                     InstructionProbe::Operand{InstructionProbe::Arg::SP, 0},
                     InstructionProbe::Operand{InstructionProbe::Arg::X30, 0}};
    next.end_block_after = true;
    g_table->Add(target, next);
    g_jit->InvalidateCacheRange(target, 4);
}

InstructionProbe Probe(void (*fn)(u64, u64, u64), InstructionProbe::Arg a, InstructionProbe::Arg b, InstructionProbe::Arg c,
                       bool end_block_after) {
    InstructionProbe probe;
    probe.fn = fn;
    probe.operands = {InstructionProbe::Operand{a, 0}, InstructionProbe::Operand{b, 0}, InstructionProbe::Operand{c, 0}};
    probe.end_block_after = end_block_after;
    return probe;
}

/// The shared environment, plus an expected-exception mode: with it off, an exception stays fatal.
struct ProbeEnv : A64TestEnv {
    bool expect_exceptions = false;
    std::vector<std::pair<u64, A64::Exception>> exceptions;

    void ExceptionRaised(u64 pc, A64::Exception exception) override {
        if (!expect_exceptions) {
            UNREACHABLE();
        }
        exceptions.emplace_back(pc, exception);
        g_jit->HaltExecution();
    }
};

struct Harness {
    ProbeEnv env;
    ProbeTable table;
    A64::Jit jit;

    static A64::UserConfig MakeConfig(A64TestEnv& env, ProbeTable& table, bool cycles) {
        A64::UserConfig conf{};
        conf.callbacks = &env;
        conf.probes = &table;
        conf.enable_cycle_counting = cycles;
        conf.wall_clock_cntpct = !cycles;
        return conf;
    }

    Harness(std::vector<u32> code, bool cycles) : env(), table(), jit(MakeConfig(env, table, cycles)) {
        env.code_mem_start_address = base;
        env.code_mem = std::move(code);
        g_table = &table;
        g_jit = &jit;
        for (auto& hits : g_hits) {
            hits.clear();
        }
        g_audit.clear();
        g_order.clear();
        g_branch_queries.clear();
    }

    /// Run from `pc` until the stop probe halts (or the ticks run out), servicing invalidation
    /// halts the way the emulator's run loop does.
    void RunFrom(u64 pc) {
        jit.SetPC(pc);
        env.ticks_left = 1000;
        for (int i = 0; i < 64; ++i) {
            HaltReason hr{};
            CheckedRun([&]() { hr = jit.Run(); });
            if (Has(hr, HaltReason::UserDefined1) || !Has(hr, HaltReason::CacheInvalidation)) {
                return;
            }
        }
        FAIL("run did not settle");
    }

    void AddStopAt(u64 pc) {
        table.Add(pc, Probe(&OnStop, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, false));
    }
};

/// The stop probe sits on a B_HERE, a branch to itself, and a host call cannot halt before its block
/// ends. The block's terminal then links back to the same block. That link checks the halt flag only
/// without cycle counting; with cycle counting it checks only the cycles left, so the loop runs, and
/// audits the branch on every pass, until the whole budget is spent (the same dispatcher contract
/// the emulator relies on when another thread halts a core: the halt is seen at the next
/// dispatch or when the ticks run out). Past the `prefix` entries the test expects, the audit must
/// therefore hold exactly one entry for the branch without cycle counting, and with it nothing but
/// repeats of that entry, ending only when the budget is gone.
void RequireStopBranchAudits(const Harness& h, std::size_t prefix, const Hit& stop_branch, bool cycles) {
    REQUIRE(g_audit.size() > prefix);
    for (std::size_t i = prefix; i < g_audit.size(); ++i) {
        REQUIRE(g_audit[i] == stop_branch);
    }
    if (cycles) {
        REQUIRE(h.env.ticks_left == 0);
    } else {
        REQUIRE(g_audit.size() == prefix + 1);
    }
}

}  // namespace

TEST_CASE("probes: a probe installed at the return site is seen by the return that follows", "[a64][probe]") {
    const bool cycles = GENERATE(true, false);
    const bool bare_ret = GENERATE(true, false);  // RET (return stack) or BR X30 (fast dispatch)
    const bool on_branch = GENERATE(true, false);  // probe on the branch itself or on the first instruction
    INFO("cycles=" << cycles << " ret=" << bare_ret << " on_branch=" << on_branch);

    // 0: BL f   4: return site   8: stop   16: f   20: return
    Harness h({0x94000004, MOV_X2_7, B_HERE, NOP, ADD_X1_X1_1, bare_ret ? RET : BR_X30}, cycles);
    h.AddStopAt(base + 8);

    // Compile everything with no census probes, so there are blocks, links, return-stack and
    // fast-dispatch entries that know nothing about the new probe.
    h.RunFrom(base);
    REQUIRE(h.jit.GetRegister(2) == 7);

    const u64 f_probe_pc = base + (on_branch ? 20 : 16);
    h.table.Add(f_probe_pc, Probe(&InstallAtLr, InstructionProbe::Arg::X30, InstructionProbe::Arg::SP, InstructionProbe::Arg::Imm, true));
    h.jit.InvalidateCacheRange(f_probe_pc, 4);
    h.jit.SetRegister(0, 0x1234);
    h.jit.SetRegister(1, 0);
    h.jit.SetRegister(2, 0);
    h.jit.SetRegister(3, 0x77);
    h.jit.SetSP(0x8000);

    h.RunFrom(base);

    REQUIRE(g_hits[0].size() == 1);
    REQUIRE(g_hits[0][0][0] == base + 4);
    REQUIRE(g_hits[0][0][1] == 0x8000);
    REQUIRE(g_hits[1].size() == 1);  // the probe installed by the probe fired, exactly once
    REQUIRE(g_hits[1][0][0] == 0x1234);
    REQUIRE(g_hits[1][0][1] == 0x8000);
    REQUIRE(g_hits[1][0][2] == base + 4);
    REQUIRE(h.jit.GetRegister(1) == 1);
    REQUIRE(h.jit.GetRegister(2) == 7);
    REQUIRE(h.jit.GetRegister(3) == 0x77);
}

TEST_CASE("probes: a probe installed at a direct branch target is seen by the branch", "[a64][probe]") {
    const bool cycles = GENERATE(true, false);
    const int kind = GENERATE(0, 1, 2, 3);  // B, BL, CBZ, B.EQ
    INFO("cycles=" << cycles << " kind=" << kind);

    std::vector<u32> code;
    u64 branch_pc = base;
    u64 target_pc = base + 16;
    switch (kind) {
    case 0:  // 0: B 16
        code = {0x14000004, NOP, NOP, NOP, MOV_X0_5, B_HERE};
        break;
    case 1:  // 0: BL 16
        code = {0x94000004, NOP, NOP, NOP, MOV_X0_5, B_HERE};
        break;
    case 2:  // 0: CBZ X1, 16 (X1 is zero)
        code = {0xb4000081, NOP, NOP, NOP, MOV_X0_5, B_HERE};
        break;
    default:  // 0: CMP X1,#0   4: B.EQ 16
        code = {0xf100003f, 0x54000060, NOP, NOP, MOV_X0_5, B_HERE};
        branch_pc = base + 4;
        break;
    }
    Harness h(code, cycles);
    h.AddStopAt(base + 20);

    h.RunFrom(base);
    REQUIRE(h.jit.GetRegister(0) == 5);

    // The branch installs a probe at its own target and asks for invalidation.
    InstructionProbe install = Probe(&InstallAtTarget, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, true);
    install.operands[0] = {InstructionProbe::Arg::Imm, target_pc};
    h.table.Add(branch_pc, install);
    h.jit.InvalidateCacheRange(branch_pc, 4);
    h.jit.SetRegister(0, 0x99);
    h.jit.SetRegister(1, 0);

    h.RunFrom(base);

    REQUIRE(g_hits[0].size() == 1);
    REQUIRE(g_hits[1].size() == 1);  // MOV X0,#5 had not run yet when the target probe fired
    REQUIRE(g_hits[1][0][0] == 0x99);
    REQUIRE(h.jit.GetRegister(0) == 5);
}

TEST_CASE("probes: every probe at one address fires once, in order", "[a64][probe]") {
    const bool cycles = GENERATE(true, false);
    Harness h({NOP, ADD_X1_X1_1, B_HERE}, cycles);
    h.AddStopAt(base + 8);
    h.table.Add(base + 4, Probe(&Record<0>, InstructionProbe::Arg::X1, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, false));
    h.table.Add(base + 4, Probe(&Record<1>, InstructionProbe::Arg::X1, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, true));
    h.table.Add(base + 4, Probe(&Record<2>, InstructionProbe::Arg::X1, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, false));

    h.jit.SetRegister(1, 41);
    h.RunFrom(base);

    REQUIRE(g_hits[0].size() == 1);
    REQUIRE(g_hits[1].size() == 1);
    REQUIRE(g_hits[2].size() == 1);
    REQUIRE(g_order == std::vector<std::size_t>{0, 1, 2});
    REQUIRE(g_hits[0][0][0] == 41);  // read live, before the ADD
    REQUIRE(h.jit.GetRegister(1) == 42);
}

TEST_CASE("probes: single stepping a probed instruction fires it once", "[a64][probe]") {
    Harness h({ADD_X1_X1_1, RET}, true);
    h.table.Add(base, Probe(&Record<0>, InstructionProbe::Arg::X1, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, true));
    h.table.Add(base + 4, Probe(&Record<1>, InstructionProbe::Arg::X30, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, true));
    h.jit.SetPC(base);
    h.jit.SetRegister(1, 9);
    h.jit.SetRegister(30, base + 0x40);
    h.env.ticks_left = 100;

    CheckedRun([&]() { h.jit.Step(); });
    REQUIRE(h.jit.GetPC() == base + 4);
    REQUIRE(h.jit.GetRegister(1) == 10);
    CheckedRun([&]() { h.jit.Step(); });
    REQUIRE(h.jit.GetPC() == base + 0x40);
    REQUIRE(g_hits[0].size() == 1);
    REQUIRE(g_hits[0][0][0] == 9);
    REQUIRE(g_hits[1].size() == 1);
    REQUIRE(g_hits[1][0][0] == base + 0x40);
}

TEST_CASE("probes: clearing the cache and running again fires the probe again", "[a64][probe]") {
    const bool cycles = GENERATE(true, false);
    Harness h({NOP, MOV_X0_5, B_HERE}, cycles);
    h.AddStopAt(base + 8);
    h.table.Add(base + 4, Probe(&Record<0>, InstructionProbe::Arg::X0, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, false));

    h.RunFrom(base);
    REQUIRE(g_hits[0].size() == 1);
    h.RunFrom(base);  // cached code: still exactly one more
    REQUIRE(g_hits[0].size() == 2);
    h.jit.ClearCache();
    h.RunFrom(base);  // retranslated from the table
    REQUIRE(g_hits[0].size() == 3);
}

TEST_CASE("probes: a second core keeps stale code until its own cache is invalidated", "[a64][probe]") {
    // Cores have separate JITs and caches: installing a probe on one core does nothing for the
    // other until that core is told as well, which is what the emulator's rendezvous waits for.
    Harness a({NOP, MOV_X0_5, B_HERE}, true);
    A64TestEnv env_b;
    env_b.code_mem_start_address = base;
    env_b.code_mem = a.env.code_mem;
    A64::UserConfig conf_b = Harness::MakeConfig(env_b, a.table, true);
    A64::Jit jit_b{conf_b};

    jit_b.SetPC(base);
    env_b.ticks_left = 20;
    CheckedRun([&]() { jit_b.Run(); });  // compiled on the second core, no probe yet

    a.table.Add(base + 4, Probe(&Record<0>, InstructionProbe::Arg::X0, InstructionProbe::Arg::Imm, InstructionProbe::Arg::Imm, false));
    a.jit.InvalidateCacheRange(base + 4, 4);

    jit_b.SetPC(base);
    env_b.ticks_left = 20;
    CheckedRun([&]() { jit_b.Run(); });
    REQUIRE(g_hits[0].empty());  // stale on the core that was not told

    jit_b.InvalidateCacheRange(base + 4, 4);
    jit_b.SetPC(base);
    env_b.ticks_left = 20;
    CheckedRun([&]() { jit_b.Run(); });
    REQUIRE(g_hits[0].size() == 1);
}

TEST_CASE("probes: branch audit sees direct and indirect branches with their targets", "[a64][probe]") {
    const bool cycles = GENERATE(true, false);
    // 0: MOV X9,#24   4: BLR X9   8: BL 24   12: stop   24: RET
    Harness h({0xd2820309, 0xd63f0120, 0x94000004, B_HERE, NOP, NOP, RET}, cycles);
    h.table.audit_branches = true;
    h.AddStopAt(base + 12);

    h.RunFrom(base);

    // The RETs are audited too (kind 3, target = X30), and so is the B_HERE the stop probe sits on.
    REQUIRE(g_audit.size() > 4);
    REQUIRE(g_audit[0] == Hit{base + 24, base + 4, 2});   // BLR: register value read when it runs
    REQUIRE(g_audit[1] == Hit{base + 8, base + 24, 3});   // RET back to the BL, X30 = base + 8
    REQUIRE(g_audit[2] == Hit{base + 24, base + 8, 1});   // BL: target known when translating
    REQUIRE(g_audit[3] == Hit{base + 12, base + 24, 3});  // RET back to the stop, X30 = base + 12
    RequireStopBranchAudits(h, 4, Hit{base + 12, base + 12, 1}, cycles);  // B_HERE under the stop probe
}

TEST_CASE("probes: conditional branches and returns are reported to the branch audit", "[a64][probe]") {
    {
        // B.cond EQ, +8 bytes. Translated before it runs, so the query is recorded either way.
        const u32 branch_if_equal = 0x54000000u | (2u << 5);
        Harness h({branch_if_equal, NOP, NOP}, true);
        h.table.audit_branches = true;
        h.AddStopAt(base + 4);
        h.RunFrom(base);
        bool saw_conditional = false;
        for (const std::array<u64, 3>& query : g_branch_queries) {
            if (query[1] == static_cast<u64>(BranchKind::Conditional) && query[0] == base && query[2] == base + 8) {
                saw_conditional = true;
            }
        }
        REQUIRE(saw_conditional);
    }
    {
        Harness h({RET, NOP}, true);
        h.table.audit_branches = true;
        h.AddStopAt(base + 4);
        h.RunFrom(base);
        bool saw_return = false;
        for (const std::array<u64, 3>& query : g_branch_queries) {
            if (query[1] == static_cast<u64>(BranchKind::Return) && query[0] == base && query[2] == 30) {
                saw_return = true;
            }
        }
        REQUIRE(saw_return);
    }
}

TEST_CASE("probes: authenticated branches and returns reach the branch audit with their targets", "[a64][probe]") {
    const bool cycles = GENERATE(true, false);
    struct Case {
        const char* name;
        u32 instruction;
        u64 kind;  // as the harness reports it: 2 indirect, 3 return
        u64 reg;   // the register the translator is told holds the target
        int target_register;
    };
    const std::array<Case, 6> cases{{
        {"BRAAZ X5", 0xD61F08BF, 2, 5, 5},
        {"BLRAAZ X5", 0xD63F08BF, 2, 5, 5},
        {"BRAA X5, X6", 0xD71F08A6, 2, 5, 5},
        {"BLRAA X5, X6", 0xD73F08A6, 2, 5, 5},
        {"RETAA", 0xD65F0BFF, 3, 30, 30},
        {"RETAB", 0xD65F0FFF, 3, 30, 30},
    }};
    for (const Case& c : cases) {
        INFO(c.name << " cycles=" << cycles);
        // 0: the branch   4: skipped   8: stop
        Harness h({c.instruction, NOP, B_HERE}, cycles);
        h.table.audit_branches = true;
        h.AddStopAt(base + 8);
        h.jit.SetRegister(static_cast<std::size_t>(c.target_register), base + 8);
        h.RunFrom(base);

        bool queried = false;
        for (const std::array<u64, 3>& query : g_branch_queries) {
            queried = queried || (query[0] == base && query[1] == static_cast<u64>(c.kind == 3 ? BranchKind::Return : BranchKind::Indirect) &&
                                  query[2] == c.reg);
        }
        REQUIRE(queried);
        // The probe reads the register when the branch runs: the entry the census must account for.
        REQUIRE(g_audit.size() >= 2);
        REQUIRE(g_audit[0] == Hit{base + 8, base, c.kind});
        RequireStopBranchAudits(h, 1, Hit{base + 8, base + 8, 1}, cycles);  // B_HERE under the stop probe
    }
}

TEST_CASE("probes: a no-modifier authenticated branch with a modifier register is not audited", "[a64][probe]") {
    // Z=0 (no modifier register) with the modifier field set to X6 is unallocated: the visitor raises
    // UnallocatedEncoding and the branch never runs. The environment is told to expect exactly that.
    Harness h({0xD61F08A6, NOP}, true);
    h.env.expect_exceptions = true;
    h.table.audit_branches = true;
    h.RunFrom(base);

    REQUIRE(h.env.exceptions.size() == 1);
    REQUIRE(h.env.exceptions[0].first == base);
    REQUIRE(h.env.exceptions[0].second == A64::Exception::UnallocatedEncoding);
    REQUIRE(g_audit.empty());
    for (const std::array<u64, 3>& query : g_branch_queries) {
        REQUIRE(query[1] != static_cast<u64>(BranchKind::Indirect));
    }
}

TEST_CASE("probes: branch audit survives a missing callee probe", "[a64][probe]") {
    Harness h({0x94000002, B_HERE, RET}, true);  // BL +8; stop; RET
    h.table.audit_branches = true;
    h.AddStopAt(base + 4);
    h.RunFrom(base);
    REQUIRE_FALSE(g_audit.empty());
    CHECK(g_audit.front() == Hit{base + 8, base, 1});
    CHECK(g_hits[0].empty());
}
