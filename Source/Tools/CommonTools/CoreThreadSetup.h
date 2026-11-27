// SPDX-License-Identifier: MIT
//
// SUPERSEDED by the fexmac library (Source/Embedding, fexmac.h), which owns this duty now.
// No tool on this branch uses this header. It is kept only so that tools on branches
// written before the library existed still build when merged; port them to fexmac.h
// and delete this file.
//
// Per-thread setup the translator core expects its embedder to do. On Linux this lives
// in FEX's process emulation layer; tools that embed the core directly use this.
#pragma once

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <cstdint>
#include <sys/mman.h>

namespace FEX::CoreThreadSetup {

// The core keeps a private stack of return addresses so that guest `call` and `ret`
// pair up as host calls and returns. The embedder allocates it, with an inaccessible
// guard page on each side. Guard pages are a whole host page, which is larger than a
// guest page on Apple silicon.
inline bool AllocateCallRetStack(FEXCore::Core::InternalThreadState* Thread) {
  constexpr size_t Guard = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
  constexpr size_t Size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  void* Alloc = ::mmap(nullptr, Size + 2 * Guard, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Alloc == MAP_FAILED) {
    return false;
  }
  auto Base = reinterpret_cast<uint64_t>(Alloc) + Guard;
  if (::mprotect(reinterpret_cast<void*>(Base), Size, PROT_READ | PROT_WRITE) != 0) {
    return false;
  }
  Thread->CallRetStackBase = reinterpret_cast<void*>(Base);
  // Start a quarter of the way in, as the Linux layer does, leaving room both ways.
  Thread->CurrentFrame->State.callret_sp = Base + Size / 4;
  return true;
}

// A descriptor table with every possible entry present and zeroed, so a guest that loads
// an arbitrary selector reads a null descriptor instead of memory past the table.
inline FEXCore::Core::CPUState::gdt_segment* FullDescriptorTable() {
  static FEXCore::Core::CPUState::gdt_segment Table[8192] {};
  return &Table[0];
}

} // namespace FEX::CoreThreadSetup
