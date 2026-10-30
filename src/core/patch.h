#pragma once
#include <cstdint>

namespace swrots {

// Redirects the function at `address` to `target` with a 5-byte JMP.
void PatchJump(uint32_t address, const void* target);
// The same for a function replaced by a C/C++ function (any calling convention, returning to its caller): while
// netplay's deterministic scheduler runs, through a gate that cleans the stack the port's code used (core/gates.h).
void PatchHook(uint32_t address, const void* target);
// Replaces an instruction of `length` bytes with CALL target + NOPs.
void PatchCall(uint32_t address, const void* target, uint32_t length);
void PatchNop(uint32_t address, uint32_t length);
void PatchBytes(uint32_t address, const void* bytes, uint32_t length);

// Allocates executable memory for runtime-generated stubs.
uint8_t* AllocStub(uint32_t size);

} // namespace swrots
