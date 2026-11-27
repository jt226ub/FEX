// SPDX-License-Identifier: MIT
/*
$info$
category: code buffer ~ Thread shared code buffer management
tags: backend|shared
$end_info$
*/
#pragma once

#include <FEXCore/fextl/memory.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <cstddef>
#include <cstdint>

namespace FEXCore {
struct GuestToHostMap;
}

namespace FEXCore::CPU {
#ifdef __APPLE__
// The largest code buffer (MAX_CODE_SIZE in SharedCodeBufferManager.cpp), and the most
// host code one block may take: half of it, so a block within the limit always fits a
// fresh largest buffer. The translator gives up on blocks that need more, the code cache
// rejects entries that are larger, and allocating more than this from a code buffer
// fails instead of replacing buffers for ever.
inline constexpr size_t MaxCodeBufferSize = 128 * 1024 * 1024;
inline constexpr size_t MaxBlockHostSize = MaxCodeBufferSize / 2;
#endif

struct CodeBuffer {
  fextl::unique_ptr<GuestToHostMap> LookupCache;

#ifdef __APPLE__
  // SimulateMapFailure (tests): behave as if mmap had failed with ENOMEM.
  CodeBuffer(size_t Size, bool ShouldBeNamed, bool SimulateMapFailure = false);
#else
  CodeBuffer(size_t Size, bool ShouldBeNamed);
#endif
  CodeBuffer(const CodeBuffer&) = delete;
  CodeBuffer& operator=(const CodeBuffer&) = delete;
  CodeBuffer(CodeBuffer&& oth) = delete;
  CodeBuffer& operator=(CodeBuffer&&) = delete;

  ~CodeBuffer();

  // Atomically allocate a fixed size buffer out of the current allocated codebuffer.
  // Lockless because it's just a linear allocator.
  struct CodeBufferAllocation {
    const uint8_t* BufferBase;
    uint8_t* BufferAllocationOffset;
  };

  CodeBufferAllocation AtomicAllocateBuffer(size_t Size) {
    Size = FEXCore::AlignUp(Size, 16);
    LOGMAN_THROW_A_FMT(reinterpret_cast<uintptr_t>(CodeBufferOffset.load()) % 16 == 0, "Buffer needs to always be 16B aligned!");

    auto ExpectedOffset = CodeBufferOffset.load(std::memory_order_relaxed);
    auto DesiredOffset = ExpectedOffset + Size;

    if (DesiredOffset > CodeBufferEnd) {
      // Couldn't fit.
      return {};
    }

    while (!CodeBufferOffset.compare_exchange_strong(ExpectedOffset, DesiredOffset)) {
      DesiredOffset = ExpectedOffset + Size;

      if (DesiredOffset > CodeBufferEnd) {
        // Couldn't fit.
        return {};
      }
    }

    // Managed to fit.
    return {
      .BufferBase = Ptr,
      .BufferAllocationOffset = ExpectedOffset,
    };
  }

  // Returns the total number of bytes available for storing code
  size_t UsableSize() const {
    return AllocatedSize - GuardSize;
  }

  // Returns whether Address is in the inaccessible page after the usable part.
  bool IsAddressInGuard(uintptr_t Address) const {
    const auto Guard = reinterpret_cast<uintptr_t>(Ptr) + UsableSize();
    return Address >= Guard && Address < Guard + GuardSize;
  }

  // Returns the full size of the buffer, including the guard page.
  size_t TotalAllocationSize() const {
    return AllocatedSize;
  }

  // Returns the num of bytes currently allocated from the allocator.
  size_t AllocatedSpaceUsed() const {
    return CodeBufferOffset - Ptr;
  }

  // Trivially reset the allocator.
  void Reset() {
    CodeBufferOffset = Ptr;
  }

  // Returns the base of the buffer.
  uint8_t* GetBufferBase() const {
    return Ptr;
  }

private:
#ifdef __APPLE__
  // The guard must be a whole host page there: protection only works on 16K pages.
  static constexpr size_t GuardSize = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
#else
  static constexpr size_t GuardSize = FEXCore::Utils::FEX_PAGE_SIZE;
#endif
  uint8_t* Ptr;
  uint8_t* CodeBufferEnd;
  size_t AllocatedSize; // including guard page; see UsableSize()

  // Code buffer allocation information.
  std::atomic<uint8_t*> CodeBufferOffset {};

#ifdef __APPLE__
public:
  // Tests: as if Used bytes had been allocated already.
  void SetUsedForTesting(size_t Used) {
    CodeBufferOffset = Ptr + Used;
  }

  // Set when the buffer could not be made (GetBufferBase() is then null): what failed
  // and its errno.
  const char* ErrorWhat {};
  int Error {};
#endif
};

/**
 * A manager that coordinates access to the CodeBuffer used for compiling new code across threads.
 *
 * The CodeBuffer is managed as a partially persistent data structure:
 * - Exactly one CodeBuffer is now designated as "active", which means data can be appended to it
 * - Lossy modifications to the active CodeBuffer will not invalidate any data in use by other threads (which is what enables save CodeBuffer sharing across threads)
 * - Instead, such lossy modifications trigger a new "version" of the data in the modifying thread. Old versions of the CodeBuffer persist as read-only data for use by the other threads.
 * - The other threads can update their version of the CodeBuffer. This will decrease the reference count and eventually trigger deallocation of the old version
 */
class SharedCodeBufferManager {
public:
  SharedCodeBufferManager();
  virtual ~SharedCodeBufferManager() = default;

  // Get the CodeBuffer that was most recently allocated.
  // This is the only CodeBuffer that data may be written to.
  fextl::shared_ptr<CodeBuffer> GetLatest();

  // Allocate a new CodeBuffer with geometric growth up to an internal maximum.
  // Subsequent calls to GetLatest will point to the returned buffer.
  fextl::shared_ptr<CodeBuffer> StartLargerCodeBuffer();

  // Allocate a new CodeBuffer with maximum internal size.
  // Subsequent calls to GetLatest will point to the returned buffer.
  fextl::shared_ptr<CodeBuffer> StartMaximalCodeBuffer();

  virtual void OnCodeBufferAllocated(const std::shared_ptr<CodeBuffer>&) {};

#ifdef __APPLE__
  // Makes the first code buffer if there is none yet, reporting failure instead of
  // stopping the process, so the embedder can be told when a thread is created. Later
  // buffers are made mid-translation, where a failure still stops the process.
  bool TryAllocateFirst();
  // Tests: the next Count code buffer allocations fail, as if mmap had failed.
  int FailuresForTesting {};
#endif

private:
  fextl::shared_ptr<CodeBuffer> Latest;

  fextl::shared_ptr<CodeBuffer> AllocateNew(size_t Size, bool AllowFailure = false);

  bool NameJITBuffers {true};
};
} // namespace FEXCore::CPU
