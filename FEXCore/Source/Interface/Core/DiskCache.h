// SPDX-License-Identifier: MIT
#pragma once
#include "Interface/Core/JIT/Relocations.h"
#include "Interface/Core/Frontend.h"
#include "Interface/Core/CPUBackend.h"

#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/File.h>
#include <FEXCore/Utils/WorkQueueThread.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_set.h>
#include <FEXCore/fextl/robin_map.h>
#include <FEXCore/fextl/map.h>
#include <FEXCore/fextl/vector.h>
#include <stdint.h>
#include <mutex>
#include <optional>
#include <span>
#include <xxhash.h>

namespace FEXCore {

namespace Context {
  class ContextImpl;
}

namespace DiskCache {

  namespace MesaFOZ {

#define FOSSILIZE_BLOB_HASH_LENGTH 40 /* SHA1 hexadecimal string length */

    struct __attribute__((packed)) foz_payload_key {
      uint8_t bytes[FOSSILIZE_BLOB_HASH_LENGTH];
    };

    struct __attribute__((packed)) foz_payload_header {
      uint32_t payload_size;
      uint32_t format;
      uint32_t crc;
      uint32_t uncompressed_size;
    };

    struct mesa_index_db_file_entry;

  } // namespace MesaFOZ

  class IndexedDB;

  struct MemoryLRUKey {
    uint64_t LookupKey;
    XXH128_hash_t GuestHash;
    uint64_t GuestFootprint;
    uint32_t Size;
  };

  struct IndexEntry {
    IndexedDB* DB;
    uint64_t Offset;
    uint32_t Size;
    uint32_t GuestSize;
    XXH128_hash_t GuestHash;
    fextl::shared_ptr<fextl::vector<uint8_t>> MemoryBlob;
    fextl::vector<uint32_t> GuestExtents;
    std::optional<fextl::list<MemoryLRUKey>::iterator> LRUEntry;
  };

  struct IndexCacheHead {
    struct IndexEntry MainEntry;
    uint64_t MainEntryFootprint;
    fextl::unique_ptr<fextl::multimap<uint64_t, IndexEntry>> MoreEntries; // sorted by guest footprint
  };

  struct __attribute__((packed)) BlobFixedHeader {
    uint32_t GuestSize;
    uint32_t HostSize;
    uint32_t EntryPointCount;
    uint32_t SmallRelocCount;
    uint32_t ThunkRelocCount;
    XXH128_hash_t GuestHash;
#ifdef __APPLE__
    // macOS port: see DiskCacheMacOS.cpp, SealEntry. Covers this header (with the field
    // zeroed), the entry up to its guest code copy and the guest extents, seeded with
    // the lookup key.
    XXH128_hash_t IntegrityHash;
#endif
  };

  // packed struct for types 0, 2 and 3. type 1 is bigger and separate below
  struct __attribute__((packed)) BlobSmallRelocation {
    uint32_t Offset;
    uint8_t Type;
    union {
      struct __attribute__((packed)) {
        uint32_t Symbol;
      } Named;
      struct __attribute__((packed)) {
        uint64_t GuestRIP;
      } RIPLiteral;
      struct __attribute__((packed)) {
        uint8_t RegisterIndex;
        uint64_t GuestRIP;
      } RIPMove;
      struct __attribute__((packed)) {
        uint8_t RegisterIndex;
        uint8_t ValueSize;
        uint32_t SiteOffset;
      } PatchableData;
    };
  };

  // type 1, implicit
  struct __attribute__((packed)) BlobThunkRelocation {
    uint32_t Offset;
    uint8_t RegisterIndex;
    uint8_t SymbolHash[32]; // sha256sum in the real RelocNamedThunkMove
  };

  struct CodeHitData {
    fextl::vector<uint8_t> Blob;
    std::span<uint8_t> HostCode;
    std::span<const uint64_t> GuestPages;
    std::span<uint64_t> EntryPointRIPs;
    std::span<const uint32_t> EntryPointHostOffsets;

    // the spans above point to memory owned by the Blob vec, so it's important this can't be copied
    CodeHitData() = default;
    CodeHitData(CodeHitData&&) = default;
    CodeHitData& operator=(CodeHitData&&) = default;
    CodeHitData(const CodeHitData&) = delete;
    CodeHitData& operator=(const CodeHitData&) = delete;
  };

  using Index = fextl::robin_map<uint64_t, IndexCacheHead>;

  class FOZFile {
  public:
    bool Open(const fextl::string& CacheFileName, bool ReadOnly);
    bool Lock(uint32_t TimeoutMS) {
      if (!FD) {
        return false;
      }
      return FD->Lock(TimeoutMS);
    }
    bool Unlock() {
      if (!FD) {
        return false;
      }
      return FD->Unlock();
    }
    File::File::FileHandleType GetHandle() {
      return FD ? FD->GetHandle() : (File::File::FileHandleType)-1;
    }
    ssize_t Size();
    bool ReadAll(fextl::vector<uint8_t>& Out); // from first blob
#ifdef __APPLE__
    // Opens Leaf inside the directory DirFD without following a symbolic link, creating
    // it private to the user when read-write. Refused is set when the file exists but may
    // not be trusted (see DiskCacheMacOS.cpp).
    bool OpenAt(int DirFD, const fextl::string& Leaf, bool ReadOnly, bool& Refused);
    bool FlushToDisk();
    bool Truncate(uint64_t Size);
#endif
    bool ReadBlob(uint64_t Offset, std::span<uint8_t> OutBlob);
    bool WriteBlob(const MesaFOZ::foz_payload_key& Key, std::span<const std::span<const uint8_t>> BlobChunks, uint64_t& OutBlobOffset);

  private:
#ifdef __APPLE__
    // Generous: several processes share one database and a lock holder may be descheduled
    // on a busy machine. A timeout only means this process runs without the cache (or
    // drops an entry), which a warm run then has to compile.
    static constexpr uint32_t OPEN_LOCK_TIMEOUT_MS = 5000;
#else
    static constexpr uint32_t OPEN_LOCK_TIMEOUT_MS = 100;
#endif

    fextl::string FileName;
    fextl::unique_ptr<File::File> FD;
    bool ReadOnly = false;
  };

  class IndexedDB {
  public:
    bool Open(const fextl::string& CacheDBName, bool ReadOnly);
    void PopulateIndex(Index& CacheIndex, bool& FoundMetadata);
    bool ReadCacheBlob(uint64_t Offset, std::span<uint8_t> OutBlob);
    bool StoreCacheBlob(const MesaFOZ::foz_payload_key& UniqueKey, uint64_t LookupKey, std::span<const uint8_t> Blob,
                        MesaFOZ::mesa_index_db_file_entry& IndexEntry, std::span<const uint8_t> IndexBlob);
    bool Full() const {
      return MaxSizeReached;
    }
#ifdef __APPLE__
    // macOS port (DiskCacheMacOS.cpp). The index is read once, at open, into IndexData;
    // only a table of (lookup key, record offset) is built, and records are parsed when
    // their key is first looked up.
    bool OpenAt(int DirFD, const fextl::string& Leaf, bool ReadOnly, uint64_t MaxFileSize);
    bool LoadIndex(uint64_t MaxFileSize, bool& FoundMetadata);
    void ResolveKey(uint64_t LookupKey, Index& CacheIndex);
    bool LockForOpen();
    void UnlockAfterOpen();
    bool Reset();
    bool FlushToDisk();
    bool WithinSizeLimit(uint64_t BlobSize, uint64_t IndexRecordSize);
    bool RepairIndexTail();
    void NoteAppended() {
      IndexKnownEnd = IndexFOZ.Size();
    }
    bool IsReadOnly() const {
      return ReadOnly;
    }
    fextl::string Name;
    uint64_t MetadataOffset = 0;
    uint32_t MetadataSize = 0;
    uint64_t DataRecords = 0;

  private:
    struct LazySlot {
      uint64_t Key;
      uint32_t Offset; // into IndexData; EmptySlot or ResolvedSlot otherwise
    };
    static constexpr uint32_t EmptySlot = ~0U;
    static constexpr uint32_t ResolvedSlot = ~0U - 1;
    fextl::vector<uint8_t> IndexData;
    fextl::vector<LazySlot> LazyTable;
    uint64_t CacheFOZSizeAtOpen = 0;
    uint64_t IndexKnownEnd = 0;
    int DirFD = -1;
    bool ParseRecord(uint32_t Offset, Index& CacheIndex);

  public:
#endif

  private:
    // stores run on the Writer, so returning quick isn't as important
#ifdef __APPLE__
    static constexpr uint32_t STORE_LOCK_TIMEOUT_MS = 5000; // see OPEN_LOCK_TIMEOUT_MS
#else
    static constexpr uint32_t STORE_LOCK_TIMEOUT_MS = 1000;
#endif
    static constexpr uint64_t BIG_MAPPING_SIZE = 1ULL << 33;

    FOZFile CacheFOZ;
    uint8_t* CacheFileMapping = nullptr;
    std::atomic<uint64_t> CacheFileSize;
    FOZFile IndexFOZ;
    bool ReadOnly = false;
    bool MaxSizeReached = false;

    FEX_CONFIG_OPT(MaxFileSize, DISKCACHEMAXFILESIZE);
  };

  class DiskCache {
  public:
    void Init(FEXCore::Context::ContextImpl* CTX);

    std::optional<CodeHitData> Lookup(Core::InternalThreadState* Thread, std::optional<ExecutableFileSectionInfo> Region, uint64_t GuestRIP,
                                      std::optional<uint64_t>& GuestCodeKey);
    void Validate(uint64_t GuestCodeKey, const CodeHitData& Hit, const CPU::CPUBackend::CompiledCode& CompiledCode,
                  std::optional<ExecutableFileSectionInfo> Region);
    bool Store(Core::InternalThreadState* Thread, std::optional<ExecutableFileSectionInfo> Region, uint64_t GuestRIP, uint64_t GuestCodeKey,
               std::span<const uint8_t> GuestCode, const CPU::CPUBackend::CompiledCode& CompiledCode,
               std::span<const FEXCore::CPU::Relocation> Relocations, const Frontend::Decoder::DecodedBlockInformation* DecodedBlockInfo);

    bool IsWritingDiskCache() const {
      return WritingDiskCache;
    }
    bool IsReadingDiskCache() const {
      return ReadingDiskCache;
    }
    bool IsValidating() const {
      return Validation;
    }

    // Waits for the writer thread to finish every queued store, then flushes the files.
    bool Flush();
    uint64_t GetRejectedCount() const {
      return RejectedCount.load(std::memory_order_relaxed);
    }
    uint64_t GetQueuedCount() const {
      return QueuedCount.load(std::memory_order_relaxed);
    }
#ifdef __APPLE__
    void LockBeforeFork();
    void UnlockAfterFork(bool Child);
#endif

  private:
    bool OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly);
    uint64_t MakeLookupKey(Core::InternalThreadState* Thread, const uint64_t ModuleOffset, bool Writable, bool MonoBackpatcher);
    IndexEntry* LookupLocked(const uint64_t LookupKey, const XXH128_hash_t& GuestHash, const uint64_t GuestFootprint);

    bool ReadingDiskCache {};
    bool WritingDiskCache {};
    FEXCore::Context::ContextImpl* CTX;
    XXH128_hash_t BucketHash;
    fextl::vector<fextl::unique_ptr<IndexedDB>> ROCacheDBs;
    fextl::unique_ptr<IndexedDB> RWCacheDB;
    Index Index;
    std::mutex IndexLock;
    bool FoundMetadata = false;
    std::atomic<uint64_t> RejectedCount {};
    std::atomic<uint64_t> QueuedCount {};
    struct FlushWorkItem;
#ifdef __APPLE__
    // macOS port, DiskCacheMacOS.cpp.
    void InitMacOS(const fextl::vector<uint8_t>& BucketBytes, size_t MachineDataSize, uint64_t ProcessBucketHash);
    bool OpenCacheDBMacOS(const fextl::string& Leaf, bool ReadOnly);
    bool CheckMetadata(IndexedDB& DB);
    void ResolveLazyLocked(uint64_t LookupKey);
    bool ValidateHit(const BlobFixedHeader& Header, std::span<const uint8_t> Body, uint64_t LookupKey, uint64_t GuestSizeAvailable,
                     std::span<const uint32_t> GuestExtents);
    bool MayUseCache(uint64_t GuestRIP, uint64_t GuestSize, std::span<const uint64_t> EntryPoints);
    bool FlushToDiskMacOS(uint64_t QueuedBeforeFlush);
    uint8_t CodeGenStateFlags() const;
    fextl::vector<uint8_t> ExpectedMetadata;
    int BaseDirFD = -1;
    int BucketDirFD = -1;
    std::mutex FlushMutex;
    uint64_t FlushedQueuedCount = 0;
    bool ForkedChild = false;
    bool ForkLocksHeld = false;
#endif
    struct CacheStoreWorkItem;

    struct PruneMemoryLRUWorkItem;
    std::atomic<uint64_t> MemoryLRUCurrentSize {};
    std::mutex MemoryLRULock;
    fextl::list<MemoryLRUKey> MemoryLRU;

    // the Writer holds references to all this stuff above and needs to be last
    fextl::unique_ptr<WorkQueueThread> Writer;

    FEX_CONFIG_OPT(EnableDiskCache, DISKCACHE);
    FEX_CONFIG_OPT(Validation, DISKCACHEVALIDATION);
    FEX_CONFIG_OPT(MapDiskCacheFiles, DISKCACHEFILEMAPPING);
    FEX_CONFIG_OPT(RelocationFilter, DISKCACHERELOCATIONFILTER);
    FEX_CONFIG_OPT(AnonCaching, DISKCACHEANONCACHING);
    FEX_CONFIG_OPT(BasePathOverride, DISKCACHEPATH);
    FEX_CONFIG_OPT(RODBNames, DISKCACHERODBNAMES);
    FEX_CONFIG_OPT(MemoryLRUMaxSize, DISKCACHEMEMORYSIZE);

    uint64_t MemoryLRUEvictThreshold = MemoryLRUMaxSize / 25;
  };

  static constexpr uint16_t AnonPrefixGuestBytes = 64;
  // The current version of the diskcache.
  // This must be changed any time codegen changes occur!
  // Be aware of the impact of changing this frequently!
  static constexpr uint16_t FormatVersion = 49;

  static constexpr uint32_t LOOKUP_KEY_MAX_BUCKET_DEPTH = 500;

#ifdef __APPLE__
  namespace MacOS {
    // Seals an entry: the integrity hash over [Blob, Blob + Size) with the hash field zeroed,
    // followed by the guest extents, seeded with the lookup key.
    XXH128_hash_t EntryHash(const uint8_t* Blob, size_t Size, std::span<const uint32_t> GuestExtents, uint64_t LookupKey);
    bool KeepBucket(std::string_view CacheBase, std::string_view Name);
  } // namespace MacOS
#endif

} // namespace DiskCache

} // namespace FEXCore
