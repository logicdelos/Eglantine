// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace Dynarmic {
namespace A64 {

/// What Jit::PrecompileBlock did.
enum class PrecompileResult : std::uint8_t {
    Compiled,        ///< The block was translated and is now in the code cache.
    AlreadyCompiled, ///< The block was in the code cache; nothing was done.
    CodeMismatch,    ///< The guest code is not what the caller expected (or unreadable); nothing was compiled.
    NoSpace,         ///< Code space could not be obtained without waiting or evicting; nothing was emitted.
    Busy,            ///< The Jit is executing; nothing was done.
    Unsupported,     ///< This backend cannot compile ahead.
};

/// Bounds speculative reads and scratch space only; demand translation has no such limit.
inline constexpr std::uint32_t MaxPrecompileWords = 1024;

/// Reports demand-compiled, non-single-step blocks with a readable, contiguous instruction span
/// of 1..MaxPrecompileWords words. Called on the compiling thread after emission; must not throw,
/// block or call back into the Jit.
using BlockCompiledCallback = void (*)(void* user, std::uint64_t location, std::uint64_t guest_hash,
                                       std::uint32_t word_count);

class Jit final {
public:
    explicit Jit(UserConfig conf);
    ~Jit();

    /**
     * Runs the emulated CPU.
     * Cannot be recursively called.
     */
    HaltReason Run();

    /**
     * Step the emulated CPU for one instruction.
     * Cannot be recursively called.
     */
    HaltReason Step();

    /**
     * Clears the code cache of all compiled code.
     * Can be called at any time. Halts execution if called within a callback.
     */
    void ClearCache();

    /**
     * Invalidate the code cache at a range of addresses.
     * @param start_address The starting address of the range to invalidate.
     * @param length The length (in bytes) of the range to invalidate.
     */
    void InvalidateCacheRange(std::uint64_t start_address, std::size_t length);

    /**
     * Reset CPU state to state at startup. Does not clear code cache.
     * Cannot be called from a callback.
     */
    void Reset();

    /**
     * Stops execution in Jit::Run.
     */
    void HaltExecution(HaltReason hr = HaltReason::UserDefined1);

    /**
     * Clears a halt reason from flags.
     * Warning: Only use this if you're sure this won't introduce races.
     */
    void ClearHalt(HaltReason hr = HaltReason::UserDefined1);

    /// Read Stack Pointer
    std::uint64_t GetSP() const;
    /// Modify Stack Pointer
    void SetSP(std::uint64_t value);

    /// Read Program Counter
    std::uint64_t GetPC() const;
    /// Modify Program Counter
    void SetPC(std::uint64_t value);

    /// Read general-purpose register.
    std::uint64_t GetRegister(std::size_t index) const;
    /// Modify general-purpose register.
    void SetRegister(size_t index, std::uint64_t value);

    /// Read all general-purpose registers.
    std::array<std::uint64_t, 31> GetRegisters() const;
    /// Modify all general-purpose registers.
    void SetRegisters(const std::array<std::uint64_t, 31>& value);

    /// Read floating point and SIMD register.
    Vector GetVector(std::size_t index) const;
    /// Modify floating point and SIMD register.
    void SetVector(std::size_t index, Vector value);

    /// Read all floating point and SIMD registers.
    std::array<Vector, 32> GetVectors() const;
    /// Modify all floating point and SIMD registers.
    void SetVectors(const std::array<Vector, 32>& value);

    /// View FPCR.
    std::uint32_t GetFpcr() const;
    /// Modify FPCR.
    void SetFpcr(std::uint32_t value);

    /// View FPSR.
    std::uint32_t GetFpsr() const;
    /// Modify FPSR.
    void SetFpsr(std::uint32_t value);

    /// View PSTATE
    std::uint32_t GetPstate() const;
    /// Modify PSTATE
    void SetPstate(std::uint32_t value);

    /// Clears exclusive state for this core.
    void ClearExclusiveState();

    /**
     * Returns true if Jit::Run was called but hasn't returned yet.
     * i.e.: We're in a callback.
     */
    bool IsExecuting() const;

    /**
     * Compiles the block at `location` (the unique value of an A64 location descriptor: pc, FPCR
     * bits and the single-step flag) before the guest reaches it. Must be called on the thread that
     * calls Run, while Run is not executing; returns Busy otherwise. On a miss, the recorded
     * `word_count` instruction words are read and hashed before translation. Only a matching
     * snapshot is given to the translator, without rereading guest memory. Invalid spans, missing
     * words and stale hashes give CodeMismatch. Pending cache invalidations are applied first and
     * invalidate a precompiled block like any other. Never clears the code cache to make room
     * (NoSpace), and does not count in g_jit_stats or call g_jit_phase_hook: it is not time the
     * guest waited for.
     */
    PrecompileResult PrecompileBlock(std::uint64_t location, std::uint64_t expected_guest_hash,
                                     std::uint32_t word_count);

    /// Sets the callback described at BlockCompiledCallback (null: none). Call it before Run, from
    /// the thread that calls Run. Only the x86-64 backend compiles ahead and reports blocks; the
    /// others return Unsupported from PrecompileBlock and never call the callback.
    void SetBlockCompiledCallback(BlockCompiledCallback callback, void* user);

    /// @brief Disassemble the instructions following the current pc and return
    /// the resulting instructions as a vector of their string representations.
    std::string Disassemble() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace A64
}  // namespace Dynarmic
