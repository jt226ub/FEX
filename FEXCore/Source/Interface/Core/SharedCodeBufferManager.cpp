// SPDX-License-Identifier: MIT
#include "Interface/Core/LookupCache.h"
#include "Interface/Core/SharedCodeBufferManager.h"

#include <FEXCore/fextl/memory.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>

#ifndef _WIN32
#include <FEXCore/Utils/PrctlUtils.h>
#endif
#ifdef __APPLE__
#include <cerrno>
#include <sys/mman.h>
#endif

namespace FEXCore::CPU {
static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;
// We don't want to move above 128MB atm because that means we will have to encode longer jumps
static constexpr size_t MAX_CODE_SIZE = 1024 * 1024 * 128;
#ifdef __APPLE__
static_assert(MAX_CODE_SIZE == MaxCodeBufferSize);
#endif

#ifdef __APPLE__
CodeBuffer::CodeBuffer(size_t Size, bool ShouldBeNamed, bool SimulateMapFailure)
  : AllocatedSize(Size) {
  if (SimulateMapFailure) {
    errno = ENOMEM;
    Ptr = static_cast<uint8_t*>(MAP_FAILED);
  } else {
    Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
  }
#else
CodeBuffer::CodeBuffer(size_t Size, bool ShouldBeNamed)
  : AllocatedSize(Size) {
  Ptr = static_cast<uint8_t*>(FEXCore::Allocator::VirtualAlloc(Size, true));
#endif
#ifdef __APPLE__
  // mmap reports failure as MAP_FAILED, not null. A buffer that cannot be made is
  // reported to SharedCodeBufferManager::AllocateNew, which decides whether that is fatal.
  if (Ptr == MAP_FAILED || !Ptr) {
    Ptr = nullptr;
    Error = errno;
    ErrorWhat = "allocate the executable (MAP_JIT) memory";
    return;
  }
#endif
  LOGMAN_THROW_A_FMT(!!Ptr, "Couldn't allocate code buffer");

#ifndef __APPLE__
  // Protect the last page of the allocated buffer to trigger SIGSEGV on write access
  uintptr_t LastPageAddr = AlignDown(reinterpret_cast<uintptr_t>(Ptr) + Size - 1, FEXCore::Utils::FEX_PAGE_SIZE);
  if (!FEXCore::Allocator::VirtualProtect(reinterpret_cast<void*>(LastPageAddr), FEXCore::Utils::FEX_PAGE_SIZE,
                                          FEXCore::Allocator::ProtectOptions::None)) {
    LogMan::Msg::EFmt("Failed to mprotect last page of code buffer.");
  }
#else
  // macOS refuses to change the protection of a page inside a MAP_JIT mapping (mprotect
  // and mach_vm_protect both fail) and refuses MAP_JIT together with MAP_FIXED, so the
  // guard cannot be made the Linux way. It can be made by replacing the last host page of
  // this, the program's own, mapping with a MAP_FIXED mapping of ordinary PROT_NONE
  // memory: the rest stays MAP_JIT, and a write past the usable end then faults whatever
  // the thread's JIT write setting is (JITWriteProbe's "Guard:" cases, as built and
  // hardened). VirtualFree's single munmap releases both. The guard is a whole host
  // page, so it cannot be stepped over by any single store.
  void* const Guard = Ptr + Size - FEXCore::Utils::FEX_HOST_PAGE_SIZE;
  if (::mmap(Guard, FEXCore::Utils::FEX_HOST_PAGE_SIZE, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != Guard) {
    Error = errno;
    ErrorWhat = "place the guard page after it";
    FEXCore::Allocator::VirtualFree(Ptr, AllocatedSize);
    Ptr = nullptr;
    return;
  }
#endif

  if (ShouldBeNamed) {
    FEXCore::Allocator::VirtualName("FEXMemJIT", Ptr, Size);
  }

  // Huge-pages reduce the amount of iTLB misses dramatically when it works.
  FEXCore::Allocator::VirtualTHPControl(Ptr, Size, FEXCore::Allocator::THPControl::Enable);

  LookupCache = fextl::make_unique<GuestToHostMap>();

  CodeBufferEnd = Ptr + UsableSize();
  CodeBufferOffset = Ptr;
}

CodeBuffer::~CodeBuffer() {
#ifdef __APPLE__
  if (!Ptr) {
    return;
  }
#endif
  FEXCore::Allocator::VirtualFree(Ptr, AllocatedSize);
}

SharedCodeBufferManager::SharedCodeBufferManager() {
  FEX_CONFIG_OPT(GlobalJITNaming, GLOBALJITNAMING);
  FEX_CONFIG_OPT(LibraryJITNaming, LIBRARYJITNAMING);
  FEX_CONFIG_OPT(BlockJITNaming, BLOCKJITNAMING);

  // Only name the JIT buffers if perf JIT naming is disabled.
  // `perf top` prefers VMA names over the JIT symbols file for some reason.
  // Breaks memory tracking when naming is enabled, but it's a debug feature so it isn't expected to be enabled by default.
  NameJITBuffers = !(GlobalJITNaming || LibraryJITNaming || BlockJITNaming);
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::AllocateNew(size_t Size, bool AllowFailure) {
#ifndef __APPLE__
  (void)AllowFailure;
#endif
#ifndef _WIN32
  // MDWE (Memory-Deny-Write-Execute) is a new Linux 6.3 feature.
  // It's equivalent to systemd's `MemoryDenyWriteExecute` but implemented entirely in the kernel.
  //
  // MDWE prevents applications from creating RWX memory mappings.
  // This prevents FEX from doing anything JIT related, as FEX uses RWX for JIT memory mappings.
  //
  // A potential workaround to make FEX work with MDWE is to call mprotect every time we need to write or modify code.
  // Alternatively, FEX could use a memory mirror where one half is mapped as RW and the other is RX.
  //
  // Once MDWE is enabled with the prctl, the feature is sealed and it can /NOT/ be turned off.
  //
  // Status of MDWE is queried through prctl using `PR_GET_MDWE`:
  // -1: The kernel doesn't support MDWE
  // 0: MDWE is supported but disabled
  // >0: MDWE is enabled, hence prohibiting RWX mappings
  int MDWE = ::prctl(PR_GET_MDWE, 0, 0, 0, 0);
  if (MDWE != -1 && MDWE != 0) {
    LogMan::Msg::EFmt("MDWE was set to 0x{:x} which means FEX can't allocate executable memory", MDWE);
  }
#endif

#ifdef __APPLE__
  const bool SimulateMapFailure = FailuresForTesting > 0;
  if (SimulateMapFailure) {
    --FailuresForTesting;
  }
  auto Buffer = fextl::make_shared<CodeBuffer>(Size, NameJITBuffers, SimulateMapFailure);
  if (!Buffer->GetBufferBase()) {
    if (AllowFailure) {
      LogMan::Msg::EFmt("Couldn't make a {} MiB code buffer: couldn't {} (errno {})", Size / 1024 / 1024, Buffer->ErrorWhat, Buffer->Error);
      return nullptr;
    }
    // Mid-translation there is no way back to the embedder: stop, saying why.
    ERROR_AND_DIE_FMT("Couldn't make a {} MiB code buffer: couldn't {} (errno {})", Size / 1024 / 1024, Buffer->ErrorWhat, Buffer->Error);
  }
#else
  auto Buffer = fextl::make_shared<CodeBuffer>(Size, NameJITBuffers);
#endif

  Latest = Buffer;

  OnCodeBufferAllocated(Buffer);

  return Buffer;
}

#ifdef __APPLE__
bool SharedCodeBufferManager::TryAllocateFirst() {
  return Latest || AllocateNew(INITIAL_CODE_SIZE, true);
}
#endif

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::GetLatest() {
  if (!Latest) {
    AllocateNew(INITIAL_CODE_SIZE);
  }
  return Latest;
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::StartLargerCodeBuffer() {
  if (!Latest) {
    // Allocate initial CodeBuffer and return it
    return GetLatest();
  }

  auto NewCodeBufferSize = GetLatest()->TotalAllocationSize();
  NewCodeBufferSize = std::min<size_t>(NewCodeBufferSize * 2, MAX_CODE_SIZE);
  return AllocateNew(NewCodeBufferSize);
}

fextl::shared_ptr<CodeBuffer> SharedCodeBufferManager::StartMaximalCodeBuffer() {
  return AllocateNew(MAX_CODE_SIZE);
}
} // namespace FEXCore::CPU
