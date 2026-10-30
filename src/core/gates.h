#pragma once
// Netplay: gates for the game's calls into the port (kernel exports, XDK replacements). The port's code runs on
// the game's stack and leaves its values below it when it returns: return addresses, the PC's heap and handle
// values, which differ from PC to PC. The game reads some of them back (its uninitialised locals: a constant
// buffer half filled, a temporary vector's unused lane) and keeps them in its memory, where they make two PCs'
// states differ. A gate calls the port's function, then zeroes the stack it dirtied below the game's: the game
// only ever sees zeros there. Used while the deterministic scheduler runs (kernel/sched.h).
//
// Each gate is at a fixed address in core.dll (itself at a fixed base), chosen by its slot: the same on every PC.

#include <cstdint>

namespace swrots {

constexpr uint32_t kGateKernelSlots = 400;  // kernel exports, by ordinal
constexpr uint32_t kGateSdkFirst = 400;     // XDK replacements, by symbol index
constexpr uint32_t kGateHookFirst = 3072;  // the port's own hooks of the game's functions (PatchHook)
constexpr uint32_t kGateSlots = 4096;

// The gate of `slot`, which calls `target` (any calling convention: every register and the flags pass through, as
// does the stack; the target sees its arguments where it expects them). Null when the slot is out of range.
const void* HostCallGate(uint32_t slot, const void* target);

// A string the port hands to the game (which may keep its address), at a fixed place in core.dll: the same text gets
// the same address on every PC, given the same order of first uses (netplay). The same text, the same pointer.
const char* GameString(const char* text);
const wchar_t* GameText(const wchar_t* text);

// Whether `address` is code (a kernel export may be a variable: those get no gate).
bool IsCodeAddress(const void* address);

} // namespace swrots
