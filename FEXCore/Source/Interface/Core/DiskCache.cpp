// SPDX-License-Identifier: MIT

#define XXH_STATIC_LINKING_ONLY

#include "Interface/Core/Frontend.h"
#include "Interface/Context/Context.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/DiskCache.h>
#include <FEXCore/Core/DiskCacheFileMapper.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/File.h>
#include <FEXCore/Utils/FileUtils.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Profiler.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXHeaderUtils/Filesystem.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <condition_variable>

#ifdef __APPLE__
#include <git_version.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace FEXCore {

namespace DiskCache {

  namespace MesaFOZ {

    enum { FOSSILIZE_COMPRESSION_NONE = 1, FOSSILIZE_COMPRESSION_DEFLATE = 2 };

    enum { FOSSILIZE_FORMAT_VERSION = 6, FOSSILIZE_FORMAT_MIN_COMPAT_VERSION = 5 };

#define FOZ_REF_MAGIC_SIZE 16

    static const uint8_t stream_reference_magic_and_version[FOZ_REF_MAGIC_SIZE] = {
      0x81, 'F', 'O', 'S', 'S', 'I', 'L', 'I', 'Z', 'E', 'D', 'B', 0, 0, 0, FOSSILIZE_FORMAT_VERSION, /* 4 bytes to use for versioning. */
    };

    struct __attribute__((packed)) mesa_index_db_file_entry {
      uint64_t hash;
      uint32_t size;
      uint64_t last_access_time;
      uint64_t cache_db_file_offset;
    };

  } // namespace MesaFOZ

  struct __attribute__((packed)) IndexExtraBlobHeader {
    XXH128_hash_t GuestHash; // StoreCacheBlob will assume it's here, care if moving
    uint64_t GuestFootprint;
    uint32_t GuestSize;
    uint32_t GuestExtentsCount;
  };

  static FileMapperFunc FileMapper = nullptr;

  bool FOZFile::Open(const fextl::string& FOZFileName, bool ReadOnly) {
    FileName = FOZFileName;
    this->ReadOnly = ReadOnly;

    File::FileModes Modes = File::FileModes::READ;
    if (!ReadOnly) {
      Modes = Modes | File::FileModes::WRITE | File::FileModes::CREATE;
    }
#ifdef __APPLE__
    // A cache file holds code that will be executed. Refuse anything but a plain file
    // (in particular a symbolic link planted in the cache directory).
    {
      struct stat Link;
      if (lstat(FileName.c_str(), &Link) == 0 && !S_ISREG(Link.st_mode)) {
        LogMan::Msg::EFmt("DiskCache: {} is not a regular file; not using it", FileName);
        return false;
      }
    }
#endif
    FD = fextl::make_unique<File::File>(FileName.c_str(), Modes, false);
    if (!FD->IsValid()) {
      FD.reset();
      return false;
    }
#ifdef __APPLE__
    {
      // Only the user may be able to write a cache file. A new file is created with the
      // default permissions and narrowed here; its directory is already private.
      struct stat Info;
      const bool Ok = fstat(FD->GetHandle(), &Info) == 0 && S_ISREG(Info.st_mode) && Info.st_uid == geteuid();
      if (Ok && !ReadOnly) {
        fchmod(FD->GetHandle(), S_IRUSR | S_IWUSR);
      }
      if (!Ok || (ReadOnly && (Info.st_mode & (S_IWGRP | S_IWOTH)))) {
        LogMan::Msg::EFmt("DiskCache: {} is not owned by this user or is writable by others; not using it", FileName);
        FD.reset();
        return false;
      }
    }
#endif

    bool Valid = false;
    bool TookLock = false;
    ssize_t Size = FD->Size();

    if (Size < FOZ_REF_MAGIC_SIZE && !ReadOnly) {
      if (!FD->Lock(OPEN_LOCK_TIMEOUT_MS)) {
        FD.reset();
        return false;
      }
      TookLock = true;
      // check size again in case someone else made it while we waited
      Size = FD->Size();
    }

    if (Size == 0 && !ReadOnly) {
      Valid = FD->PWrite(MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE, 0) == FOZ_REF_MAGIC_SIZE;
    } else {
      uint8_t magic[FOZ_REF_MAGIC_SIZE];
      if (FD->PRead(magic, FOZ_REF_MAGIC_SIZE, 0) == FOZ_REF_MAGIC_SIZE &&
          memcmp(magic, MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE - 1) == 0) {
        int version = magic[FOZ_REF_MAGIC_SIZE - 1];
        Valid = version <= MesaFOZ::FOSSILIZE_FORMAT_VERSION && version >= MesaFOZ::FOSSILIZE_FORMAT_MIN_COMPAT_VERSION;
      }
    }

    if (TookLock) {
      FD->Unlock();
    }

    if (!Valid) {
      FD.reset();
    }
    return Valid;
  }

  ssize_t FOZFile::Size() {
    return FD ? FD->Size() : -1;
  }

  bool FOZFile::ReadAll(fextl::vector<uint8_t>& Out) {
    ssize_t FileSize = Size();
    if (FileSize < FOZ_REF_MAGIC_SIZE) {
      return false;
    }
    Out.resize((size_t)FileSize - FOZ_REF_MAGIC_SIZE);
    return FD->PRead(Out.data(), Out.size(), FOZ_REF_MAGIC_SIZE) == (ssize_t)Out.size();
  }

#ifdef __APPLE__
  bool FOZFile::FlushToDisk() {
    if (!FD) {
      return false;
    }
    // F_FULLFSYNC is the macOS way to push data through the drive's own cache; plain
    // fsync only reaches the drive.
    if (fcntl(FD->GetHandle(), F_FULLFSYNC) == 0) {
      return true;
    }
    return fsync(FD->GetHandle()) == 0;
  }

  bool FOZFile::TruncateTornTail() {
    // Records are appended as [key][payload header][payload] under the file lock. A
    // process that dies mid-append leaves a partial last record, and every record
    // appended after it would then be unreachable. Walk the framing and cut the file at
    // the end of the last whole record. The caller holds the lock, so no record is
    // being written right now.
    ssize_t FileSize = Size();
    if (FileSize < FOZ_REF_MAGIC_SIZE) {
      return false;
    }
    fextl::vector<uint8_t> Data;
    if (!ReadAll(Data)) {
      return false;
    }
    // Offsets below are file offsets; Data starts after the magic.
    uint64_t Offset = FOZ_REF_MAGIC_SIZE;
    while (Offset + sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header) <= (uint64_t)FileSize) {
      MesaFOZ::foz_payload_header Header;
      memcpy(&Header, Data.data() + Offset - FOZ_REF_MAGIC_SIZE + sizeof(MesaFOZ::foz_payload_key), sizeof(Header));
      const uint64_t RecordEnd = Offset + sizeof(MesaFOZ::foz_payload_key) + sizeof(Header) + Header.payload_size;
      if (RecordEnd > (uint64_t)FileSize) {
        break;
      }
      Offset = RecordEnd;
    }
    if (Offset == (uint64_t)FileSize) {
      return true;
    }
    LogMan::Msg::IFmt("DiskCache: dropping {} bytes of a partly written record at the end of {}", FileSize - Offset, FileName);
    return ftruncate(FD->GetHandle(), Offset) == 0;
  }
#endif

  bool FOZFile::ReadBlob(uint64_t Offset, std::span<uint8_t> OutBlob) {
    if (FD->PRead(OutBlob.data(), OutBlob.size(), Offset) != (ssize_t)OutBlob.size()) {
      return false;
    }
    return true;
  }

  bool FOZFile::WriteBlob(const MesaFOZ::foz_payload_key& Key, std::span<const std::span<const uint8_t>> BlobChunks, uint64_t& OutBlobOffset) {
    ssize_t FileSize = FD->Size();
    if (FileSize < 0) {
      return false;
    }
    uint64_t WriteOffset = (uint64_t)FileSize;

    if (FD->PWrite(Key.bytes, sizeof(Key.bytes), WriteOffset) != sizeof(Key.bytes)) {
      return false;
    }
    WriteOffset += sizeof(Key.bytes);

    uint64_t TotalBlobSize = 0;
    for (const std::span<const uint8_t>& Chunk : BlobChunks) {
      TotalBlobSize += Chunk.size();
    }

    MesaFOZ::foz_payload_header ScratchHeader {.payload_size = (uint32_t)TotalBlobSize,
                                               .format = MesaFOZ::FOSSILIZE_COMPRESSION_NONE,
                                               .crc = 0, // todo? maybe
                                               .uncompressed_size = (uint32_t)TotalBlobSize};

    if (FD->PWrite(&ScratchHeader, sizeof(ScratchHeader), WriteOffset) != sizeof(ScratchHeader)) {
      return false;
    }
    WriteOffset += sizeof(ScratchHeader);

    OutBlobOffset = WriteOffset;

    for (const std::span<const uint8_t>& Chunk : BlobChunks) {
      if (Chunk.size() == 0) {
        continue;
      }
      if (FD->PWrite(Chunk.data(), Chunk.size(), WriteOffset) != (ssize_t)Chunk.size()) {
        return false;
      }
      WriteOffset += Chunk.size();
    }

    return true;
  }

  bool IndexedDB::Open(const fextl::string& CacheDBName, bool ReadOnly) {
    if (!CacheFOZ.Open(CacheDBName + ".foz", ReadOnly)) {
      return false;
    }
    if (!IndexFOZ.Open(CacheDBName + "_idx.foz", ReadOnly)) {
      return false;
    }

    File::File::FileHandleType CacheFileHandle = CacheFOZ.GetHandle();
    if (FileMapper && CacheFileHandle != (File::File::FileHandleType)-1) {
      CacheFileMapping = reinterpret_cast<uint8_t*>(FileMapper(CacheFileHandle, ReadOnly ? CacheFOZ.Size() : BIG_MAPPING_SIZE));
      CacheFileSize = CacheFOZ.Size();
    }

    this->ReadOnly = ReadOnly;
    return true;
  }

  void IndexedDB::PopulateIndex(Index& CacheIndex, bool& FoundMetadata) {
    fextl::vector<uint8_t> Data;
    if (!IndexFOZ.ReadAll(Data)) {
      return;
    }

    ssize_t CacheFOZSize = CacheFOZ.Size();
    if (CacheFOZSize < 0) {
      return;
    }

    const uint8_t* IndexDataStart = Data.data();
    const size_t IndexDataSize = Data.size();
    size_t ReadOffset = 0;
    while (ReadOffset + sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header) <= IndexDataSize) {
      const auto* FOZKey = reinterpret_cast<const MesaFOZ::foz_payload_key*>(IndexDataStart + ReadOffset);
      ReadOffset += sizeof(MesaFOZ::foz_payload_key);
      const auto* FOZHeader = reinterpret_cast<const MesaFOZ::foz_payload_header*>(IndexDataStart + ReadOffset);
      ReadOffset += sizeof(MesaFOZ::foz_payload_header);

      uint64_t IndexBlobSize = sizeof(MesaFOZ::mesa_index_db_file_entry) + sizeof(IndexExtraBlobHeader);

      if (FOZHeader->payload_size < IndexBlobSize || ReadOffset + FOZHeader->payload_size > IndexDataSize) {
        break;
      }
      const auto* IndexBlobCommon = reinterpret_cast<const MesaFOZ::mesa_index_db_file_entry*>(IndexDataStart + ReadOffset);
      ReadOffset += sizeof(MesaFOZ::mesa_index_db_file_entry);
      const auto* IndexBlobExtra = reinterpret_cast<const IndexExtraBlobHeader*>(IndexDataStart + ReadOffset);
      ReadOffset += sizeof(IndexExtraBlobHeader);

      // skip corrupt (carefully) so we don't have to figure that out in the hot path later
      if (IndexBlobCommon->cache_db_file_offset > (uint64_t)CacheFOZSize ||
          IndexBlobCommon->size > (uint64_t)CacheFOZSize - IndexBlobCommon->cache_db_file_offset) {
        continue;
      }
      if (IndexBlobExtra->GuestSize + sizeof(BlobFixedHeader) > IndexBlobCommon->size) {
        continue;
      }
      IndexBlobSize += IndexBlobExtra->GuestExtentsCount * sizeof(uint32_t);
      if (FOZHeader->payload_size != IndexBlobSize) {
        break;
      }
      const auto* GuestExtents = reinterpret_cast<const uint32_t*>(IndexDataStart + ReadOffset);
      ReadOffset += IndexBlobExtra->GuestExtentsCount * sizeof(uint32_t);
      if (FOZKey->bytes[39] != 0xFF) {
        IndexEntry NewEntry {this, IndexBlobCommon->cache_db_file_offset, IndexBlobCommon->size, IndexBlobExtra->GuestSize,
                             IndexBlobExtra->GuestHash};
        NewEntry.GuestExtents.resize(IndexBlobExtra->GuestExtentsCount);
        memcpy(NewEntry.GuestExtents.data(), GuestExtents, IndexBlobExtra->GuestExtentsCount * sizeof(uint32_t));
        bool ExtentsValid = true;
        for (uint32_t i = 0; i < NewEntry.GuestExtents.size(); i += 2) {
          if ((uint64_t)NewEntry.GuestExtents[i] + NewEntry.GuestExtents[i + 1] > IndexBlobExtra->GuestSize) {
            ExtentsValid = false;
            break;
          }
        }
        if (!ExtentsValid) {
          continue;
        }
        auto It = CacheIndex.find(IndexBlobCommon->hash);
        if (It == CacheIndex.end()) {
          CacheIndex.emplace(IndexBlobCommon->hash, IndexCacheHead {std::move(NewEntry), IndexBlobExtra->GuestFootprint, nullptr});
        } else {
          if (!It->second.MoreEntries.get()) {
            It->second.MoreEntries = fextl::make_unique<fextl::multimap<uint64_t, IndexEntry>>();
          }
          It->second.MoreEntries->insert({IndexBlobExtra->GuestFootprint, std::move(NewEntry)});
        }
      } else {
        FoundMetadata = true;
#ifdef __APPLE__
        if (!MetadataSize) {
          MetadataOffset = IndexBlobCommon->cache_db_file_offset;
          MetadataSize = IndexBlobCommon->size;
        }
#endif
      }
    }
    // could truncate/delete index if we don't end up perfectly at end here
  }

#ifdef __APPLE__
  bool IndexedDB::FlushToDisk() {
    if (ReadOnly) {
      return true;
    }
    const bool CacheOk = CacheFOZ.FlushToDisk();
    return IndexFOZ.FlushToDisk() && CacheOk;
  }

  bool IndexedDB::LockForOpen() {
    if (!CacheFOZ.Lock(STORE_LOCK_TIMEOUT_MS)) {
      return false;
    }
    if (!IndexFOZ.Lock(STORE_LOCK_TIMEOUT_MS)) {
      CacheFOZ.Unlock();
      return false;
    }
    return true;
  }

  void IndexedDB::UnlockAfterOpen() {
    CacheFOZ.Unlock();
    IndexFOZ.Unlock();
  }

  bool IndexedDB::RepairIndex() {
    return !ReadOnly && IndexFOZ.TruncateTornTail();
  }

  bool IndexedDB::HasEntries() {
    return IndexFOZ.Size() > FOZ_REF_MAGIC_SIZE;
  }

  bool IndexedDB::Reset() {
    if (ReadOnly) {
      return false;
    }
    MetadataOffset = 0;
    MetadataSize = 0;
    const bool Ok = ftruncate(CacheFOZ.GetHandle(), FOZ_REF_MAGIC_SIZE) == 0 && ftruncate(IndexFOZ.GetHandle(), FOZ_REF_MAGIC_SIZE) == 0;
    CacheFileSize = CacheFOZ.Size();
    return Ok;
  }
#endif

  bool IndexedDB::ReadCacheBlob(uint64_t Offset, std::span<uint8_t> OutBlob) {
    if (CacheFileMapping && (ReadOnly || Offset + OutBlob.size() <= BIG_MAPPING_SIZE)) {
      if (Offset + OutBlob.size() > CacheFileSize) {
        return false;
      }
      // todo could reduce copies by having a private mapping for relocs, etc
      memcpy(OutBlob.data(), CacheFileMapping + Offset, OutBlob.size());
      return true;
    } else {
      return CacheFOZ.ReadBlob(Offset, OutBlob);
    }
  }

  bool IndexedDB::StoreCacheBlob(const MesaFOZ::foz_payload_key& UniqueKey, uint64_t LookupKey, std::span<const uint8_t> Blob,
                                 MesaFOZ::mesa_index_db_file_entry& IndexEntry, std::span<const uint8_t> IndexBlob) {
    if (ReadOnly) {
      // shouldn't happen
      return false;
    }

    if (MaxSizeReached || CacheFileSize + Blob.size() >= MaxFileSize) {
      MaxSizeReached = true;
      return false;
    }

    if (!CacheFOZ.Lock(STORE_LOCK_TIMEOUT_MS) || !IndexFOZ.Lock(STORE_LOCK_TIMEOUT_MS)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    // write cache side first so we get offset for index
    std::span<const uint8_t> BlobChunks[] = {Blob};
    uint64_t BlobOffset = 0;
    if (!CacheFOZ.WriteBlob(UniqueKey, BlobChunks, BlobOffset)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    IndexEntry = {.hash = LookupKey,
                  .size = (uint32_t)Blob.size(),
                  .last_access_time = 0, // todo..
                  .cache_db_file_offset = BlobOffset};

    std::span<const uint8_t> IndexBlobChunks[] = {
      {(const uint8_t*)&IndexEntry, sizeof(IndexEntry)},
      IndexBlob,
    };
    uint64_t UnusedIndexBlobOffset = 0;
    if (!IndexFOZ.WriteBlob(UniqueKey, IndexBlobChunks, UnusedIndexBlobOffset)) {
      CacheFOZ.Unlock();
      IndexFOZ.Unlock();
      return false;
    }

    CacheFOZ.Unlock();
    IndexFOZ.Unlock();

    // publish new file size for memory-mapped reads
    if (CacheFileMapping) {
      CacheFileSize = BlobOffset + Blob.size();
    }

    return true;
  }

  bool DiskCache::OpenCacheDB(const fextl::string& CacheDBName, bool ReadOnly) {
    fextl::unique_ptr<IndexedDB> CurDB;

    if (!ReadOnly && RWCacheDB) {
      // rw already opened, just support one
      return false;
    }

    CurDB = fextl::make_unique<IndexedDB>();
    if (!CurDB) {
      return false;
    }

    if (!CurDB->Open(CacheDBName, ReadOnly)) {
#ifdef __APPLE__
      LogMan::Msg::IFmt("DiskCache: could not open {}; translating without it", CacheDBName);
#endif
      CurDB.reset();
      return false;
    }

#ifdef __APPLE__
    // macOS port: a database is only used if its metadata blob is exactly the bucket
    // description of this process (format version, port and build identity, host
    // features, code-generation options). The directory and file names are hashes of the
    // same data, so this only fails for a damaged file or one copied under the wrong
    // name; either way none of its entries may be loaded.
    //
    // A read-write database is shared with other processes, which append to it under
    // the file locks. Holding both locks while reading it means no record is half
    // written while it is checked.
    {
      if (!ReadOnly && !CurDB->LockForOpen()) {
        LogMan::Msg::IFmt("DiskCache: {} is busy; translating without it", CacheDBName);
        return false;
      }
      if (!ReadOnly) {
        CurDB->RepairIndex();
      }
      ::FEXCore::DiskCache::Index Local;
      bool LocalFoundMetadata = false;
      CurDB->PopulateIndex(Local, LocalFoundMetadata);
      if (!CheckMetadata(*CurDB, CacheDBName)) {
        RejectedCount.fetch_add(1, std::memory_order_relaxed);
        if (ReadOnly || !CurDB->Reset()) {
          if (!ReadOnly) {
            CurDB->UnlockAfterOpen();
          }
          return false;
        }
        // The cache directory is disposable: start this database again rather than
        // translating without a cache from now on.
        LogMan::Msg::IFmt("DiskCache: emptied {} and starting it again", CacheDBName);
        Local.clear();
        LocalFoundMetadata = false;
      }
      if (!ReadOnly) {
        CurDB->UnlockAfterOpen();
      }
      FoundMetadata |= LocalFoundMetadata;
      for (auto It = Local.begin(); It != Local.end(); ++It) {
        auto& Head = It->second;
        auto Existing = Index.find(It->first);
        if (Existing == Index.end()) {
          Index.emplace(It->first, std::move(Head));
          continue;
        }
        auto& Target = Existing->second;
        if (!Target.MoreEntries) {
          Target.MoreEntries = fextl::make_unique<fextl::multimap<uint64_t, IndexEntry>>();
        }
        Target.MoreEntries->insert({Head.MainEntryFootprint, std::move(Head.MainEntry)});
        if (Head.MoreEntries) {
          for (auto& Entry : *Head.MoreEntries) {
            Target.MoreEntries->insert({Entry.first, std::move(Entry.second)});
          }
        }
      }
    }
#else
    CurDB->PopulateIndex(Index, FoundMetadata);
#endif

    if (ReadOnly) {
      ROCacheDBs.push_back(std::move(CurDB));
    } else {
      RWCacheDB = std::move(CurDB);
    }

    return true;
  }

#ifdef __APPLE__
  bool DiskCache::CheckMetadata(IndexedDB& DB, const fextl::string& Name) {
    if (DB.MetadataSize == 0) {
      // A database without a description is acceptable only while it holds nothing,
      // which is the state of a new file before the description is appended.
      if (!DB.HasEntries()) {
        return true;
      }
      LogMan::Msg::EFmt("DiskCache: {} has entries but no description; not using it", Name);
      return false;
    }
    if (DB.MetadataSize != ExpectedMetadata.size()) {
      LogMan::Msg::EFmt("DiskCache: {} was made for a different configuration or build; not using it", Name);
      return false;
    }
    fextl::vector<uint8_t> Stored(DB.MetadataSize);
    if (!DB.ReadCacheBlob(DB.MetadataOffset, Stored) || memcmp(Stored.data(), ExpectedMetadata.data(), Stored.size()) != 0) {
      LogMan::Msg::EFmt("DiskCache: {} was made for a different configuration or build, or is damaged; not using it", Name);
      return false;
    }
    return true;
  }

  // Largest number of bytes each relocation type rewrites at its offset: a 64-bit literal,
  // or a padded four-instruction constant load.
  static uint32_t RelocationWriteSize(uint8_t Type) {
    switch (static_cast<CPU::RelocationTypes>(Type)) {
    case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL:
    case CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL: return 8;
    case CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE:
    case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE: return 16;
    }
    return 0;
  }

  // Checks a cache entry before any of it is used. Body is the entry without its guest
  // code copy, starting with the fixed header. Everything the loader will index or write
  // through is bounds-checked here, so the relocation code and the copy into executable
  // memory only ever see a consistent entry.
  bool DiskCache::ValidateHit(const BlobFixedHeader& Header, std::span<const uint8_t> Body, uint64_t LookupKey, uint64_t GuestSizeAvailable) {
    // 1. Integrity: the hash covers the header (with the hash field zeroed) and every
    // byte after it up to the guest code copy, and is seeded with the lookup key.
    {
      BlobFixedHeader Zeroed = Header;
      Zeroed.IntegrityHash = {};
      XXH3_state_t HashState;
      XXH3_128bits_reset_withSeed(&HashState, LookupKey);
      XXH3_128bits_update(&HashState, &Zeroed, sizeof(Zeroed));
      XXH3_128bits_update(&HashState, Body.data() + sizeof(Header), Body.size() - sizeof(Header));
      if (!XXH128_isEqual(XXH3_128bits_digest(&HashState), Header.IntegrityHash)) {
        return false;
      }
    }

    // 2. Shape: code size and entry points.
    if (Header.HostSize == 0 || Header.HostSize % 16 != 0 || Header.EntryPointCount == 0 || Header.GuestSize == 0 ||
        Header.GuestSize > GuestSizeAvailable) {
      return false;
    }
    const uint8_t* Cursor = Body.data() + sizeof(Header) + Header.HostSize;
    bool HasBlockEntry = false;
    for (uint32_t i = 0; i < Header.EntryPointCount; ++i) {
      uint64_t RIPOffset;
      memcpy(&RIPOffset, Cursor + i * sizeof(uint64_t), sizeof(RIPOffset));
      HasBlockEntry |= RIPOffset == 0;
    }
    Cursor += Header.EntryPointCount * sizeof(uint64_t);
    for (uint32_t i = 0; i < Header.EntryPointCount; ++i) {
      uint32_t HostOffset;
      memcpy(&HostOffset, Cursor + i * sizeof(uint32_t), sizeof(HostOffset));
      if (HostOffset >= Header.HostSize || HostOffset % 4 != 0) {
        return false;
      }
    }
    Cursor += Header.EntryPointCount * sizeof(uint32_t);
    if (!HasBlockEntry) {
      return false;
    }

    // 3. Relocations: known type, write inside the code, operands in range.
    for (uint32_t i = 0; i < Header.SmallRelocCount; ++i) {
      BlobSmallRelocation Reloc;
      memcpy(&Reloc, Cursor + i * sizeof(Reloc), sizeof(Reloc));
      const uint32_t WriteSize = RelocationWriteSize(Reloc.Type);
      if (WriteSize == 0 || Reloc.Type == uint8_t(CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE) || Reloc.Offset % 4 != 0 ||
          uint64_t(Reloc.Offset) + WriteSize > Header.HostSize) {
        return false;
      }
      switch (static_cast<CPU::RelocationTypes>(Reloc.Type)) {
      case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL:
        if (Reloc.Named.Symbol > uint32_t(CPU::RelocNamedSymbolLiteral::NamedSymbol::SYMBOL_LITERAL_EXITFUNCTION_LINKER)) {
          return false;
        }
        break;
      case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE:
        if (Reloc.RIPMove.RegisterIndex >= 32) {
          return false;
        }
        break;
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE: {
        // These read live guest bytes at the site, which must lie inside the guest code
        // this entry was hashed over.
        const auto Size = Reloc.PatchableData.ValueSize;
        const bool IsCRC = Reloc.Type == uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE);
        const bool SizeOk = IsCRC ? (Size >= 1 && Size <= 15) : (Size == 1 || Size == 2 || Size == 4 || Size == 8);
        if (!SizeOk || Reloc.PatchableData.RegisterIndex >= 32 || uint64_t(Reloc.PatchableData.SiteOffset) + Size > Header.GuestSize) {
          return false;
        }
        break;
      }
      default: break;
      }
    }
    Cursor += Header.SmallRelocCount * sizeof(BlobSmallRelocation);
    if (Header.ThunkRelocCount) {
      // Thunks resolve through the thunk handler, which a macOS embedder may not have.
      if (!CTX->ThunkHandler) {
        return false;
      }
      for (uint32_t i = 0; i < Header.ThunkRelocCount; ++i) {
        BlobThunkRelocation Reloc;
        memcpy(&Reloc, Cursor + i * sizeof(Reloc), sizeof(Reloc));
        if (Reloc.Offset % 4 != 0 || uint64_t(Reloc.Offset) + 16 > Header.HostSize || Reloc.RegisterIndex >= 32) {
          return false;
        }
      }
    }
    return true;
  }
#endif

  struct DiskCache::FlushWorkItem final : WorkQueueThread::WorkItem {
    struct State {
      std::mutex Mutex;
      std::condition_variable CV;
      bool Done = false;
    };
    fextl::shared_ptr<State> Shared;
    explicit FlushWorkItem(fextl::shared_ptr<State> Shared)
      : Shared(std::move(Shared)) {}
    void Run() override {
      {
        std::lock_guard lk {Shared->Mutex};
        Shared->Done = true;
      }
      Shared->CV.notify_all();
    }
  };

  bool DiskCache::Flush() {
    if (!IsWritingDiskCache() || !Writer) {
      return false;
    }
    // The writer runs its queue in order, so once this marker has run every store queued
    // before it has been written.
    auto Shared = fextl::make_shared<FlushWorkItem::State>();
    Writer->QueueWork(fextl::make_unique<FlushWorkItem>(Shared));
    {
      std::unique_lock lk {Shared->Mutex};
      Shared->CV.wait(lk, [&] { return Shared->Done; });
    }
#ifdef __APPLE__
    // A full flush costs milliseconds; skip it when nothing was stored since the last one.
    const uint64_t Queued = QueuedCount.load(std::memory_order_relaxed);
    if (Queued == FlushedQueuedCount) {
      return true;
    }
    FlushedQueuedCount = Queued;
    return RWCacheDB->FlushToDisk();
#else
    return true;
#endif
  }

  FEX_DEFAULT_VISIBILITY void SetFileMapper(FileMapperFunc Func) {
    FileMapper = Func;
  }

#ifdef __APPLE__
  // The core's build identity: the LC_UUID of the Mach-O image this code is linked into.
  static void AppendImageUUID(fextl::vector<uint8_t>& Out) {
    Dl_info Info {};
    if (!dladdr(reinterpret_cast<const void*>(&AppendImageUUID), &Info) || !Info.dli_fbase) {
      return;
    }
    const auto* Header = static_cast<const mach_header_64*>(Info.dli_fbase);
    if (Header->magic != MH_MAGIC_64) {
      return;
    }
    const auto* Command = reinterpret_cast<const load_command*>(Header + 1);
    for (uint32_t i = 0; i < Header->ncmds; ++i) {
      if (Command->cmd == LC_UUID) {
        const auto* UUID = reinterpret_cast<const uuid_command*>(Command);
        Out.insert(Out.end(), UUID->uuid, UUID->uuid + sizeof(UUID->uuid));
        return;
      }
      Command = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(Command) + Command->cmdsize);
    }
  }

  static fextl::vector<uint8_t> MakePortIdentity(const FEXCore::Context::ContextImpl& CTX) {
    // Bump when this port changes the cache's contents or checks.
    constexpr uint32_t PortCacheRevision = 1;
    fextl::vector<uint8_t> Out;
    const char Tag[8] = {'F', 'E', 'X', 'M', 'A', 'C', 'O', 'S'};
    Out.insert(Out.end(), Tag, Tag + sizeof(Tag));
    Out.insert(Out.end(), reinterpret_cast<const uint8_t*>(&PortCacheRevision),
               reinterpret_cast<const uint8_t*>(&PortCacheRevision) + sizeof(PortCacheRevision));
    Out.insert(Out.end(), GIT_HASH.begin(), GIT_HASH.end());
    AppendImageUUID(Out);
    char Brand[256] {};
    size_t BrandSize = sizeof(Brand) - 1;
    if (sysctlbyname("machdep.cpu.brand_string", Brand, &BrandSize, nullptr, 0) == 0) {
      Out.insert(Out.end(), Brand, Brand + strnlen(Brand, sizeof(Brand)));
    }
    for (uint32_t MIDR : CTX.HostFeatures.CPUMIDRs) {
      Out.insert(Out.end(), reinterpret_cast<const uint8_t*>(&MIDR), reinterpret_cast<const uint8_t*>(&MIDR) + sizeof(MIDR));
    }
    return Out;
  }

  // Trust model: a cache file is code that will be executed, so the directories holding
  // it must be writable by this user only. The bucket directory is created by the core
  // and made private; the base directory may be shared with other things the user owns
  // but must not be writable by other users. This stops other users, not the user's own
  // processes: anything running as this user can write the cache, and the integrity
  // checks cannot tell its writes from the core's.
  static bool CheckDirectoryTrust(const fextl::string& Path, bool MakePrivate) {
    struct stat Info;
    if (lstat(Path.c_str(), &Info) != 0 || !S_ISDIR(Info.st_mode)) {
      LogMan::Msg::EFmt("DiskCache: {} is not a directory; disk cache disabled", Path);
      return false;
    }
    if (Info.st_uid != geteuid()) {
      LogMan::Msg::EFmt("DiskCache: {} is not owned by this user; disk cache disabled", Path);
      return false;
    }
    if (MakePrivate && (Info.st_mode & (S_IRWXG | S_IRWXO))) {
      if (chmod(Path.c_str(), S_IRWXU) != 0) {
        LogMan::Msg::EFmt("DiskCache: could not make {} private; disk cache disabled", Path);
        return false;
      }
    } else if (Info.st_mode & (S_IWGRP | S_IWOTH)) {
      LogMan::Msg::EFmt("DiskCache: {} is writable by other users; disk cache disabled", Path);
      return false;
    }
    return true;
  }

  // True if any file in the bucket directory was written in the last week.
  static bool BucketRecentlyUsed(const fextl::string& Path) {
    constexpr time_t KeepSeconds = 7 * 24 * 60 * 60;
    const time_t Now = time(nullptr);
    struct stat DirInfo;
    if (lstat(Path.c_str(), &DirInfo) != 0 || !S_ISDIR(DirInfo.st_mode)) {
      return true; // not ours to judge; leave it
    }
    bool Recent = Now - DirInfo.st_mtime < KeepSeconds;
    struct Capture {
      const fextl::string* Path;
      time_t Now;
      bool* Recent;
    } C {&Path, Now, &Recent};
    FEXCore::FileUtils::WalkDirectory(
      Path,
      [](std::string_view Name, bool, const void* UserData) {
        auto* C = static_cast<const Capture*>(UserData);
        struct stat Info;
        if (lstat(fextl::fmt::format("{}/{}", *C->Path, Name).c_str(), &Info) == 0 && C->Now - Info.st_mtime < KeepSeconds) {
          *C->Recent = true;
        }
      },
      &C);
    return Recent;
  }

  // Only directories named like a bucket (16 hex digits) are ever removed, so pointing the
  // cache path at a directory that holds other things cannot delete them.
  static bool LooksLikeBucketName(std::string_view Name) {
    return Name.size() == 16 && std::all_of(Name.begin(), Name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  }
#endif

  static inline void PruneStaleEntries(std::string_view CacheBase, std::string_view MachineBucketHash) {
    FEXCORE_PROFILE_SCOPED("DiskCache::PruneStaleEntries");
    FEX_CONFIG_OPT(DiskCachePruneStaleEntries, DISKCACHEPRUNESTALEENTRIES);

    if (!DiskCachePruneStaleEntries) {
      return;
    }

    struct SimpleCapture {
      std::string_view CacheBase, MachineBucketHash;
    } const SimpleCapture {
      .CacheBase = CacheBase,
      .MachineBucketHash = MachineBucketHash,
    };

    FEXCore::FileUtils::WalkDirectory(
      CacheBase,
      [](std::string_view name, bool is_dir, const void* user_data) {
        if (!is_dir) {
          return;
        }

        auto capture = reinterpret_cast<const struct SimpleCapture*>(user_data);

        // Current behaviour is to remove entries that no longer match the MachineBucketHash.
        // This means that if the Disk Cache version no longer matches, or the FEXCore::HostFeatures differ, then they get removed.
#ifdef __APPLE__
        // Processes with different host features or builds use different buckets and may
        // run at the same time (the instruction tests do), so a bucket is only removed
        // once nothing has written to it for a while; otherwise processes would delete
        // each other's caches.
        if (!LooksLikeBucketName(name) || name == capture->MachineBucketHash ||
            BucketRecentlyUsed(fextl::fmt::format("{}/{}", capture->CacheBase, name))) {
          return;
        }
#endif
        if (name != capture->MachineBucketHash) {
          FEXCore::FileUtils::RecursiveRemoveDirectory(fextl::fmt::format("{}/{}", capture->CacheBase, name));
        }
      },
      &SimpleCapture);
  }

  void DiskCache::Init(FEXCore::Context::ContextImpl* CTX) {
    this->CTX = CTX;

    if (!EnableDiskCache) {
      return;
    }

    fextl::string SerializedConfig = FEXCore::Config::SerializeForCache();

    const auto HostFeatureHash = CTX->HostFeatures.HashForCaching();

    struct __attribute__((packed)) {
      uint16_t FormatVersion;
      uint64_t HostFeaturesHash;
    } MachineBucketData = {FormatVersion, HostFeatureHash.HostFeaturesHash};

    uint8_t ProcessFeaturesHash {};
    memcpy(&ProcessFeaturesHash, &HostFeatureHash.ProcessFeaturesHash, sizeof(ProcessFeaturesHash));
    LogMan::Msg::IFmt("Disk Cache features:");
    LogMan::Msg::IFmt("HostFeatures Hash: 0x{:016x}", HostFeatureHash.HostFeaturesHash);
    LogMan::Msg::IFmt("ProcessFeaturesHash: 0x{:02x}", ProcessFeaturesHash);

#ifdef __APPLE__
    // macOS port: upstream relies on FormatVersion being bumped by hand whenever code
    // generation changes. This port also keys the cache on the exact build (the core's
    // Mach-O UUID, which the linker derives from the code), so a rebuilt core never loads
    // code cached by another one, and on the CPU model, which reaches the guest through
    // CPUID.
    const fextl::vector<uint8_t> PortIdentity = MakePortIdentity(*CTX);
#else
    const fextl::vector<uint8_t> PortIdentity {};
#endif
    const size_t MachineSize = sizeof(MachineBucketData) + PortIdentity.size();

    fextl::vector<uint8_t> BucketBytes(MachineSize + sizeof(HostFeatureHash.ProcessFeaturesHash) + SerializedConfig.size());
    memcpy(BucketBytes.data(), &MachineBucketData, sizeof(MachineBucketData));
    if (!PortIdentity.empty()) {
      memcpy(BucketBytes.data() + sizeof(MachineBucketData), PortIdentity.data(), PortIdentity.size());
    }

    // 64-bit mode and HostType is in the ProcessBucket hash only instead of the MachineBucketHash
    // These effect code-gen, but they aren't part of the MachineBucketHash, as it comes from Process state.
    memcpy(&BucketBytes[MachineSize], &HostFeatureHash.ProcessFeaturesHash, sizeof(HostFeatureHash.ProcessFeaturesHash));
    memcpy(BucketBytes.data() + MachineSize + sizeof(HostFeatureHash.ProcessFeaturesHash), SerializedConfig.data(), SerializedConfig.size());

    uint64_t MachineBucketHash = XXH3_64bits(BucketBytes.data(), MachineSize);
    uint64_t ProcessBucketHash = XXH3_64bits(BucketBytes.data() + MachineSize, sizeof(HostFeatureHash.ProcessFeaturesHash) + SerializedConfig.size());
    BucketHash.high64 = MachineBucketHash;
    BucketHash.low64 = ProcessBucketHash;

    fextl::string BasePath = BasePathOverride();
    if (BasePath.empty()) {
      BasePath = FEXCore::Config::GetCacheDirectory() + "DiskCache/";
    }
#ifdef __APPLE__
    if (!BasePath.ends_with('/')) {
      BasePath += '/';
    }
    ExpectedMetadata = BucketBytes;
    FHU::Filesystem::CreateDirectories(BasePath);
    if (!CheckDirectoryTrust(BasePath, false)) {
      return;
    }
#endif

    const auto MachineBucketHashAsString = fextl::fmt::format("{:016x}", MachineBucketHash);
    PruneStaleEntries(BasePath, MachineBucketHashAsString);

    BasePath += MachineBucketHashAsString + "/";
    FHU::Filesystem::CreateDirectories(BasePath);
#ifdef __APPLE__
    if (!CheckDirectoryTrust(BasePath, true)) {
      return;
    }
#endif

    if (!MapDiskCacheFiles) {
      FileMapper = nullptr;
    }

    const auto RWDBBasePath = fextl::fmt::format("{}RWCacheDB_{:016x}", BasePath, ProcessBucketHash);
    OpenCacheDB(RWDBBasePath, false);

    if (RWCacheDB && !FoundMetadata) {
      // we just opened a fresh cache, add a metadata blob
      MesaFOZ::foz_payload_key MetadataKey;
      memset(MetadataKey.bytes, 0xFF, sizeof(MetadataKey));
      IndexExtraBlobHeader MetaDataHeader = {};
      MesaFOZ::mesa_index_db_file_entry IndexEntry = {};
      RWCacheDB->StoreCacheBlob(MetadataKey, ~0, {BucketBytes.data(), BucketBytes.size()}, IndexEntry,
                                {reinterpret_cast<uint8_t*>(&MetaDataHeader), sizeof(MetaDataHeader)});
      Index.erase(~0);
    }

    std::string_view RONames = RODBNames();
    while (!RONames.empty()) {
      const auto Delim = RONames.find(',');
      const std::string_view ROName = RONames.substr(0, Delim);
      if (!ROName.empty()) {
        fextl::string RODBBasePath = BasePath;
        RODBBasePath += ROName;
        OpenCacheDB(RODBBasePath, true);
      }
      if (Delim == std::string_view::npos) {
        break;
      }
      // advance to next
      RONames.remove_prefix(Delim + 1);
    }

    WritingDiskCache = (bool)RWCacheDB;
    ReadingDiskCache = !ROCacheDBs.empty() || RWCacheDB != nullptr;

    if (IsWritingDiskCache()) {
      FEXCore::Threads::Flags WriterThreadFlags = {.LowPriority = true, .Internal = true};
      Writer = fextl::make_unique<WorkQueueThread>(WriterThreadFlags, "FEX:DiskCache");
    }
  }

  uint64_t DiskCache::MakeLookupKey(Core::InternalThreadState* Thread, const uint64_t CodeKey, bool Writable, bool MonoBackpatcher) {
    struct __attribute__((packed)) {
      uint64_t CodeKey;
      XXH128_hash_t BucketHash;
      uint8_t Flags;
    } BlobKeyBytes = {CodeKey, BucketHash, 0};

    if (Writable) {
      BlobKeyBytes.Flags |= 1 << 0;
    }
    if (CTX->AreMonoHacksActive()) {
      BlobKeyBytes.Flags |= 1 << 1;
    }
    if (Thread->CurrentFrame->State.flags[X86State::RFLAG_TF_RAW_LOC]) {
      BlobKeyBytes.Flags |= 1 << 2;
    }
    if (MonoBackpatcher) {
      BlobKeyBytes.Flags |= 1 << 3;
    }

    return XXH3_64bits(&BlobKeyBytes, sizeof(BlobKeyBytes));
  }

  struct DiskCache::PruneMemoryLRUWorkItem final : WorkQueueThread::WorkItem {
    DiskCache* Self;
    PruneMemoryLRUWorkItem(DiskCache* Self)
      : Self(Self) {}
    void Run() override {
      if (Self->MemoryLRUCurrentSize <= Self->MemoryLRUMaxSize + Self->MemoryLRUEvictThreshold) {
        return;
      }
      std::lock_guard IndexGuard(Self->IndexLock);
      std::lock_guard LRUGuard(Self->MemoryLRULock);
      if (Self->MemoryLRU.empty()) {
        return;
      }
      auto Last = std::prev(Self->MemoryLRU.end());

      while (Self->MemoryLRUCurrentSize > Self->MemoryLRUMaxSize && !Self->MemoryLRU.empty()) {
        auto LastKey = *Last;
        auto IndexEntry = Self->LookupLocked(LastKey.LookupKey, LastKey.GuestHash, LastKey.GuestFootprint);
        bool AtFront = (Last == Self->MemoryLRU.begin());
        if (IndexEntry && IndexEntry->MemoryBlob.use_count() > 1) {
          // being read rn, keep moving
          if (AtFront) {
            break;
          }
          Last--;
          continue;
        }
        if (IndexEntry) {
          IndexEntry->MemoryBlob.reset();
          IndexEntry->LRUEntry.reset();
        }
        Self->MemoryLRUCurrentSize -= LastKey.Size;
        auto Deleted = Last;
        if (AtFront) {
          Self->MemoryLRU.erase(Deleted);
          break;
        }
        Last--;
        Self->MemoryLRU.erase(Deleted);
      }
    }
  };

  std::optional<CodeHitData> DiskCache::Lookup(Core::InternalThreadState* Thread, std::optional<ExecutableFileSectionInfo> Region,
                                               uint64_t GuestRIP, std::optional<uint64_t>& GuestCodeKey) {
    if (!IsReadingDiskCache()) {
      return std::nullopt;
    }
    if (Region && Region->FileStartVA) {
      struct __attribute__((packed)) {
        uint64_t GuestOffset;
        uint64_t FileId;
      } FileBackedKey = {GuestRIP - Region->FileStartVA, Region->FileInfo.FileId};
      GuestCodeKey = XXH3_64bits(&FileBackedKey, sizeof(FileBackedKey));
    } else {
      if (!AnonCaching) {
        return std::nullopt;
      }
      Thread->FrontendDecoder->DecodeLoop(reinterpret_cast<const uint8_t*>(GuestRIP), AnonPrefixGuestBytes);
      const auto* BlockInfo = Thread->FrontendDecoder->GetDecodedBlockInfo();

      XXH3_state_t HashState;
      XXH3_64bits_reset(&HashState);
      for (auto& SubBlock : BlockInfo->Blocks) {
        if (SubBlock.BlockStatus != Frontend::Decoder::DecodedBlockStatus::SUCCESS) {
          return std::nullopt;
        }
        uint64_t HashStart = SubBlock.Entry;
        // skip over masked in-block data and data/etc gaps between blocks
        for (auto& DataMask : SubBlock.DataMasks) {
          XXH3_64bits_update(&HashState, reinterpret_cast<const uint8_t*>(HashStart), DataMask.FieldAddress - HashStart);
          HashStart = DataMask.FieldAddress + DataMask.ValueSize;
        }
        if (HashStart != SubBlock.Entry + SubBlock.Size) {
          XXH3_64bits_update(&HashState, reinterpret_cast<const uint8_t*>(HashStart), SubBlock.Size - (HashStart - SubBlock.Entry));
        }
      }
      GuestCodeKey = XXH3_64bits_digest(&HashState);
      // if (TotalSize < AnonPrefixGuestBytes) {
      //   GuestCodeKey = 0;
      //   return std::nullopt;
      // }
      // LogMan::Msg::IFmt("anon lookup! length {:d} {}", GuestCodeKey, TotalSize);
    }

    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    if (RangeInfo.Size == 0 || RangeInfo.Base > GuestRIP) {
      return std::nullopt;
    }
    uint64_t Available = RangeInfo.Base + RangeInfo.Size - GuestRIP;

    uint64_t LookupKey =
      MakeLookupKey(Thread, *GuestCodeKey, RangeInfo.Writable, GuestRIP == CTX->GetMonoBackPatcherBlock().load(std::memory_order_relaxed));

    XXH128_hash_t LiveGuestHash;
    uint64_t LastFootprintHashed = 0;
    bool FirstHash = true; // 0 is probably a valid footprint hash so we need an extra bit there
    bool TriedMainEntry = false;
    IndexEntry MainEntry;
    uint64_t MainEntryFootprint;
    fextl::multimap<uint64_t, IndexEntry>::iterator MoreEntriesIt;
    fextl::multimap<uint64_t, IndexEntry>* MapPointer = nullptr;
    IndexEntry* EntryUnderReview;
    fextl::shared_ptr<fextl::vector<uint8_t>> BlobRef;
    std::optional<fextl::list<MemoryLRUKey>::iterator> LRUIter;
    uint64_t CurrentFootprint = 0;
    {
      std::lock_guard Guard(IndexLock);
      auto It = Index.find(LookupKey);
      if (It == Index.end()) {
        // definite miss
        return std::nullopt;
      }

      MainEntry = It->second.MainEntry;
      MainEntryFootprint = It->second.MainEntryFootprint;
      MapPointer = It->second.MoreEntries.get();
      if (MapPointer) {
        MoreEntriesIt = It->second.MoreEntries->begin();
      }
      if (MapPointer && MoreEntriesIt != It->second.MoreEntries->end()) {
        EntryUnderReview = &MoreEntriesIt->second;
        BlobRef = EntryUnderReview->MemoryBlob;
        LRUIter = EntryUnderReview->LRUEntry;
        CurrentFootprint = MoreEntriesIt->first;
      } else {
        EntryUnderReview = &MainEntry;
        BlobRef = EntryUnderReview->MemoryBlob;
        LRUIter = EntryUnderReview->LRUEntry;
        CurrentFootprint = MainEntryFootprint;
        TriedMainEntry = true;
      }
    }
    // found a lookup key match, check for guest hash match now
    bool FoundMatchingHash = false;
    bool Advance = false;
    while (!FoundMatchingHash) {
      if (Advance) {
        std::lock_guard Guard(IndexLock);

        EntryUnderReview = nullptr;
        if (MapPointer && MoreEntriesIt != MapPointer->end()) {
          // if the current footprint is also the main entry's footprint, give main entry a shot next
          if (!TriedMainEntry && MoreEntriesIt->first == MainEntryFootprint) {
            EntryUnderReview = &MainEntry;
            BlobRef = EntryUnderReview->MemoryBlob;
            LRUIter = EntryUnderReview->LRUEntry;
            CurrentFootprint = MainEntryFootprint;
            TriedMainEntry = true;
          } else {
            MoreEntriesIt++;
          }
        }
        if (!MapPointer || MoreEntriesIt == MapPointer->end()) {
          if (TriedMainEntry) {
            // ran out
            break;
          } else {
            EntryUnderReview = &MainEntry;
            BlobRef = EntryUnderReview->MemoryBlob;
            LRUIter = EntryUnderReview->LRUEntry;
            CurrentFootprint = MainEntryFootprint;
            TriedMainEntry = true;
          }
        }
        if (!EntryUnderReview && MapPointer && MoreEntriesIt != MapPointer->end()) {
          EntryUnderReview = &MoreEntriesIt->second;
          BlobRef = EntryUnderReview->MemoryBlob;
          LRUIter = EntryUnderReview->LRUEntry;
          CurrentFootprint = MoreEntriesIt->first;
        }
      }

      Advance = true;

      // entry not backed by anything right now - todo prune..
      if (!EntryUnderReview->DB && !BlobRef) {
        continue;
      }

      // do we have enough room in our live code to even hash GuestSize worth?
      if (Available < EntryUnderReview->GuestSize) {
        continue;
      }

      // if (Entry.GuestExtents.size()) {
      //   LogMan::Msg::IFmt("lookup! length {:d}", Entry.GuestSize);
      //   for(uint32_t i = 0; i < Entry.GuestExtents.size(); i+=2 ) {
      //     LogMan::Msg::IFmt("extent {} {}", Entry.GuestExtents[i], Entry.GuestExtents[i]+Entry.GuestExtents[i+1]);
      //   }
      // }
      {
        IndexEntry& Entry = *EntryUnderReview;
        // only renew the hash if the candidate footprint is different than previous
        if (FirstHash || LastFootprintHashed != CurrentFootprint) {
          XXH3_state_t HashState;
          XXH3_128bits_reset(&HashState);
          for (uint32_t i = 0; i < Entry.GuestExtents.size(); i += 2) {
            XXH3_128bits_update(&HashState, reinterpret_cast<uint8_t*>(GuestRIP) + Entry.GuestExtents[i], Entry.GuestExtents[i + 1]);
          }
          LiveGuestHash = XXH3_128bits_digest(&HashState);
          LastFootprintHashed = CurrentFootprint;
          FirstHash = false;
        }

        if (XXH128_isEqual(LiveGuestHash, Entry.GuestHash)) {
          FoundMatchingHash = true;
          break;
        } else if (Validation) {
          fextl::vector<uint8_t> GuestCode(Entry.GuestSize);
          if (Entry.Size >= Entry.GuestSize && Entry.DB && Entry.DB->ReadCacheBlob(Entry.Offset + Entry.Size - Entry.GuestSize, GuestCode)) {
            const uint8_t* CachedGuest = GuestCode.data();
            const uint8_t* LiveGuest = reinterpret_cast<const uint8_t*>(GuestRIP);
            uint64_t DiffCount = 0;
            uint64_t DiffOffset = 0;
            for (uint32_t i = 0; i < Entry.GuestExtents.size(); i += 2) {
              uint32_t Begin = Entry.GuestExtents[i];
              uint32_t End = Begin + Entry.GuestExtents[i + 1];
              bool PreviousByteDiff = false;
              for (uint32_t Offset = Begin; Offset < End; Offset++) {
                if (LiveGuest[Offset] != CachedGuest[Offset]) {
                  if (DiffCount == 0) {
                    DiffOffset = Offset;
                  }
                  if (!PreviousByteDiff) {
                    DiffCount++;
                  }
                  PreviousByteDiff = true;
                } else {
                  PreviousByteDiff = false;
                }
              }
            }
            if (DiffCount) {
              uint64_t DiffStart = DiffOffset >= 8 ? DiffOffset - 8 : 0;
              uint64_t DiffContextBytes = std::min<uint64_t>(16, Entry.GuestSize - DiffStart);
              auto LiveDump = fmt::join(std::span<const uint8_t> {LiveGuest + DiffStart, DiffContextBytes}, " ");
              auto CacheDump = fmt::join(std::span<const uint8_t> {CachedGuest + DiffStart, DiffContextBytes}, " ");
              auto KeyPrefix = (Region && Region->FileStartVA) ? "file" : "anon";
              LogMan::Msg::IFmt("DiskCache: lookup guest hash mismatch key={}-{:x} gsize={}, ndiff={}, first diff: offset={} "
                                "live=[{:02x}] "
                                "cached=[{:02x}]",
                                KeyPrefix, *GuestCodeKey, Entry.GuestSize, DiffCount, DiffOffset, LiveDump, CacheDump);
            } else {
              LogMan::Msg::IFmt("DiskCache: guest hash mismatch but no diff?");
            }
          }
        }
      }
    }

    if (!FoundMatchingHash || !EntryUnderReview) {
      return std::nullopt;
    }

    IndexEntry& Entry = *EntryUnderReview;

    fextl::vector<uint64_t> GuestPages;
    for (uint32_t i = 0; i < Entry.GuestExtents.size(); i += 2) {
      uint64_t FirstPage = (Entry.GuestExtents[i] + GuestRIP) & Utils::FEX_PAGE_MASK;
      uint64_t LastPage = (Entry.GuestExtents[i] + GuestRIP + Entry.GuestExtents[i + 1] - 1) & Utils::FEX_PAGE_MASK;
      for (uint64_t Page = FirstPage; Page <= LastPage; Page += Utils::FEX_PAGE_SIZE) {
        if (GuestPages.size() == 0 || Page != GuestPages.back()) {
          GuestPages.push_back(Page);
        }
      }
    }

    // this seems to be a full hit, pull from disk and check the entry is big enough to have everything (except GuestCode)
    CodeHitData HitData;
    uint32_t EntrySizeWithoutGuestCode = Entry.Size - Entry.GuestSize;
    HitData.Blob.resize(GuestPages.size() * sizeof(uint64_t) + EntrySizeWithoutGuestCode);
    memcpy(HitData.Blob.data(), GuestPages.data(), GuestPages.size() * sizeof(uint64_t));
    uint32_t BlobOffset = GuestPages.size() * sizeof(uint64_t);
    bool FoundInLRU = false;
    if (BlobRef && BlobRef->size() >= EntrySizeWithoutGuestCode) {
      FoundInLRU = true;
      memcpy(HitData.Blob.data() + BlobOffset, BlobRef->data(), EntrySizeWithoutGuestCode);
      if (LRUIter) {
        std::lock_guard Guard(MemoryLRULock);
        // avoided disk by nabbing from lru, bump to front
        // LogMan::Msg::IFmt("lru hit! {}", MemoryLRUCurrentSize);
        MemoryLRU.splice(MemoryLRU.begin(), MemoryLRU, *LRUIter);
      }
    } else if (!Entry.DB->ReadCacheBlob(Entry.Offset, {HitData.Blob.data() + BlobOffset, EntrySizeWithoutGuestCode})) {
      return std::nullopt;
    }

    if (EntrySizeWithoutGuestCode < sizeof(BlobFixedHeader)) {
      return std::nullopt;
    }
    BlobFixedHeader Header;
    memcpy(&Header, HitData.Blob.data() + BlobOffset, sizeof(Header));
#ifdef __APPLE__
    const std::span<const uint8_t> BlobBody {HitData.Blob.data() + BlobOffset, EntrySizeWithoutGuestCode};
#endif
    BlobOffset += sizeof(Header);

#ifdef __APPLE__
    // Computed in 64 bits: a damaged count must not wrap around to a matching size.
    uint64_t SizeNeeded = sizeof(Header) + uint64_t(Header.HostSize) + uint64_t(Header.EntryPointCount) * (sizeof(uint64_t) + sizeof(uint32_t));
    SizeNeeded += uint64_t(Header.SmallRelocCount) * sizeof(BlobSmallRelocation) + uint64_t(Header.ThunkRelocCount) * sizeof(BlobThunkRelocation);
#else
    uint32_t SizeNeeded = sizeof(Header) + Header.HostSize + Header.EntryPointCount * (sizeof(uint64_t) + sizeof(uint32_t));
    SizeNeeded += Header.SmallRelocCount * sizeof(BlobSmallRelocation) + Header.ThunkRelocCount * sizeof(BlobThunkRelocation);
#endif
    if (EntrySizeWithoutGuestCode != SizeNeeded) {
#ifdef __APPLE__
      RejectedCount.fetch_add(1, std::memory_order_relaxed);
#endif
      return std::nullopt;
    }

    if (Entry.GuestSize != Header.GuestSize || !XXH128_isEqual(Header.GuestHash, Entry.GuestHash)) {
#ifdef __APPLE__
      RejectedCount.fetch_add(1, std::memory_order_relaxed);
#endif
      return std::nullopt;
    }

#ifdef __APPLE__
    if (!ValidateHit(Header, BlobBody, LookupKey, Available)) {
      RejectedCount.fetch_add(1, std::memory_order_relaxed);
      LogMan::Msg::EFmt("DiskCache: entry for guest code at {:#x} failed its integrity or sanity check; compiling instead", GuestRIP);
      return std::nullopt;
    }
#endif

    bool StoreInMemory = !FoundInLRU;
    if (EntrySizeWithoutGuestCode > MemoryLRUMaxSize) {
      StoreInMemory = false;
    }

    if (StoreInMemory) {
      auto NewBlobRef = fextl::make_shared<fextl::vector<uint8_t>>(EntrySizeWithoutGuestCode);
      memcpy(NewBlobRef->data(), HitData.Blob.data() + BlobOffset, EntrySizeWithoutGuestCode);
      std::lock_guard Guard(IndexLock);
      auto CurrentIndexEntry = LookupLocked(LookupKey, Header.GuestHash, LastFootprintHashed);
      if (CurrentIndexEntry && !CurrentIndexEntry->MemoryBlob) {
        std::lock_guard LRUGuard(MemoryLRULock);
        MemoryLRU.push_front({LookupKey, Header.GuestHash, LastFootprintHashed, EntrySizeWithoutGuestCode});
        CurrentIndexEntry->LRUEntry = MemoryLRU.begin();
        CurrentIndexEntry->MemoryBlob = NewBlobRef;
        MemoryLRUCurrentSize += EntrySizeWithoutGuestCode;
      }
    }

    if (StoreInMemory && MemoryLRUCurrentSize > MemoryLRUMaxSize + MemoryLRUEvictThreshold) {
      Writer->QueueWork(fextl::make_unique<PruneMemoryLRUWorkItem>(this));
    }

    HitData.HostCode = {HitData.Blob.data() + BlobOffset, Header.HostSize};
    BlobOffset += Header.HostSize;
    HitData.EntryPointRIPs = {reinterpret_cast<uint64_t*>(HitData.Blob.data() + BlobOffset), Header.EntryPointCount};
    BlobOffset += Header.EntryPointCount * sizeof(uint64_t);
    HitData.EntryPointHostOffsets = {reinterpret_cast<const uint32_t*>(HitData.Blob.data() + BlobOffset), Header.EntryPointCount};
    BlobOffset += Header.EntryPointCount * sizeof(uint32_t);
    std::span<const BlobSmallRelocation> SmallRelocs = {reinterpret_cast<const BlobSmallRelocation*>(HitData.Blob.data() + BlobOffset),
                                                        Header.SmallRelocCount};
    BlobOffset += Header.SmallRelocCount * sizeof(BlobSmallRelocation);
    std::span<const BlobThunkRelocation> ThunkRelocs = {reinterpret_cast<const BlobThunkRelocation*>(HitData.Blob.data() + BlobOffset),
                                                        Header.ThunkRelocCount};
    BlobOffset += Header.ThunkRelocCount * sizeof(BlobThunkRelocation);

    HitData.GuestPages = {reinterpret_cast<uint64_t*>(HitData.Blob.data()), GuestPages.size()};

    for (auto& EntryPointRip : HitData.EntryPointRIPs) {
      EntryPointRip += GuestRIP;
    }

    if (!CTX->CodeCache.ApplyPackedCodeRelocations(GuestRIP, std::as_writable_bytes(HitData.HostCode), SmallRelocs, ThunkRelocs)) {
      return std::nullopt;
    }

    return HitData;
  }

  void DiskCache::Validate(uint64_t GuestCodeKey, const CodeHitData& Hit, const CPU::CPUBackend::CompiledCode& CompiledCode,
                           std::optional<ExecutableFileSectionInfo> Region) {
    if (!Validation) {
      return;
    }

    auto KeyPrefix = (Region && Region->FileStartVA) ? "file" : "anon";

    if (Hit.HostCode.size() != CompiledCode.Size) {
      LogMan::Msg::EFmt("DiskCache: validate host size mismatch key={}-{:x} cached={} live={}", KeyPrefix, GuestCodeKey,
                        Hit.HostCode.size(), CompiledCode.Size);
    } else if (memcmp(Hit.HostCode.data(), CompiledCode.BlockBegin, CompiledCode.Size) != 0) {
      bool PreviousByteDiff = false;
      size_t FirstDiff = 0;
      size_t DiffCount = 0;
      for (size_t i = 0; i < CompiledCode.Size; i++) {
        if (Hit.HostCode[i] != CompiledCode.BlockBegin[i]) {
          if (DiffCount == 0) {
            FirstDiff = i;
          }
          if (!PreviousByteDiff) {
            DiffCount++;
          }
          PreviousByteDiff = true;
        } else {
          PreviousByteDiff = false;
        }
      }

      // align to 4 bytes to read host arm easier
      const size_t DiffStart = (FirstDiff & ~3ULL) >= 8 ? (FirstDiff & ~3ULL) - 8 : 0;
      const size_t ContextBytes = std::min<size_t>(16, CompiledCode.Size - DiffStart);
      LogMan::Msg::EFmt("DiskCache: validate host code mismatch key={}-{:x} size={} firstoffset={} ndiff={} cached=[{:02x}] live=[{:02x}]",
                        KeyPrefix, GuestCodeKey, CompiledCode.Size, FirstDiff, DiffCount,
                        fmt::join(std::span<const uint8_t> {Hit.HostCode.data() + DiffStart, ContextBytes}, " "),
                        fmt::join(std::span<const uint8_t> {CompiledCode.BlockBegin + DiffStart, ContextBytes}, " "));
    }
  }

  IndexEntry* DiskCache::LookupLocked(const uint64_t LookupKey, const XXH128_hash_t& GuestHash, const uint64_t GuestFootprint) {
    auto It = Index.find(LookupKey);
    if (It != Index.end()) {
      if (XXH128_isEqual(It->second.MainEntry.GuestHash, GuestHash)) {
        return &It->second.MainEntry;
      }
      if (It->second.MoreEntries.get()) {
        auto Range = It->second.MoreEntries->equal_range(GuestFootprint);
        for (auto MoreEntriesIt = Range.first; MoreEntriesIt != Range.second; MoreEntriesIt++) {
          if (XXH128_isEqual(MoreEntriesIt->second.GuestHash, GuestHash)) {
            return &MoreEntriesIt->second;
          }
        }
      }
    }
    return nullptr;
  }

  struct DiskCache::CacheStoreWorkItem final : WorkQueueThread::WorkItem {
    DiskCache* Self;
    IndexedDB* DB;
    MesaFOZ::foz_payload_key UniqueKey;
    uint64_t LookupKey;
    std::span<uint8_t> Blob;
    fextl::vector<uint8_t> IndexBlob;
    bool StoreDisk;
    CacheStoreWorkItem(DiskCache* Self, IndexedDB* DB, const MesaFOZ::foz_payload_key& UniqueKey, uint64_t LookupKey,
                       std::span<uint8_t> Blob, fextl::vector<uint8_t>&& IndexBlob, bool StoreDisk)
      : Self(Self)
      , DB(DB)
      , UniqueKey(UniqueKey)
      , LookupKey(LookupKey)
      , Blob(Blob)
      , IndexBlob(std::move(IndexBlob))
      , StoreDisk(StoreDisk) {}
    void Run() override {
      struct MesaFOZ::mesa_index_db_file_entry IndexHeader;
      bool DiskSuccess = !StoreDisk || DB->StoreCacheBlob(UniqueKey, LookupKey, Blob, IndexHeader, IndexBlob);

      bool KeepEntryInMemory = true;
      // todo possible other lru condition here like entry size?
      if (Blob.size() > Self->MemoryLRUMaxSize) {
        KeepEntryInMemory = false;
      } else {
        Self->MemoryLRUCurrentSize += Blob.size();
      }

      const IndexExtraBlobHeader* IndexAfterHeader = reinterpret_cast<const IndexExtraBlobHeader*>(IndexBlob.data());

      fextl::list<MemoryLRUKey>::iterator NewLRUEntry;
      if (KeepEntryInMemory) {
        std::lock_guard Guard(Self->MemoryLRULock);
        Self->MemoryLRU.push_front({LookupKey, IndexAfterHeader->GuestHash, IndexAfterHeader->GuestFootprint, (uint32_t)Blob.size()});
        NewLRUEntry = Self->MemoryLRU.begin();
      }
      {
        std::lock_guard Guard(Self->IndexLock);
        auto IndexEntry = Self->LookupLocked(LookupKey, IndexAfterHeader->GuestHash, IndexAfterHeader->GuestFootprint);
        LOGMAN_THROW_A_FMT(IndexEntry != nullptr, "Stored Index entry not found?");
        if (IndexEntry) {
          if (StoreDisk && DiskSuccess) {
            IndexEntry->DB = DB;
            IndexEntry->Offset = IndexHeader.cache_db_file_offset;
          }
          if (!KeepEntryInMemory) {
            IndexEntry->MemoryBlob.reset();
          } else {
            IndexEntry->LRUEntry = NewLRUEntry;
          }
        }
      }

      if (KeepEntryInMemory && Self->MemoryLRUCurrentSize > Self->MemoryLRUMaxSize + Self->MemoryLRUEvictThreshold) {
        Self->Writer->QueueWork(fextl::make_unique<PruneMemoryLRUWorkItem>(Self));
      }
    }
  };

  bool DiskCache::Store(Core::InternalThreadState* Thread, std::optional<ExecutableFileSectionInfo> Region, uint64_t GuestRIP,
                        uint64_t GuestCodeKey, std::span<const uint8_t> GuestCode, const CPU::CPUBackend::CompiledCode& CompiledCode,
                        std::span<const FEXCore::CPU::Relocation> Relocations, const Frontend::Decoder::DecodedBlockInformation* DecodedBlockInfo) {
    if (!IsWritingDiskCache()) {
      return false;
    }
    if (!DecodedBlockInfo) {
      return false;
    }
    // check for any reloc targets outside of our jurisdiction
    // todo what are they exactly? caching those blocks is great when it works, so need to figure this out and make finer-grained if we can
    if (RelocationFilter && Region) {
      for (const auto& Reloc : Relocations) {
        if (Reloc.Header.Type != CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL && Reloc.Header.Type != CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE) {
          continue;
        }
        uint64_t Target = Reloc.GuestRIP.GuestRIP;
        if (Target >= Region->BeginVA && Target < Region->EndVA) {
          continue;
        }
        // let it through if it's inside the same ELF image? (like bss)
        if (Region->FileInfo.MappedSize && Target >= Region->FileStartVA && Target < Region->FileStartVA + Region->FileInfo.MappedSize) {
          continue;
        }

        auto TargetSection = CTX->SyscallHandler->LookupExecutableFileSection(Thread, Target);
        if (!TargetSection || TargetSection->FileInfo.FileId != Region->FileInfo.FileId) {
          // we don't know where it's pointing, so we don't know how to encode the offset, so we can't cache atm
          return false;
        }
      }
    }

    uint32_t SmallRelocCount = 0;
    uint32_t ThunkRelocCount = 0;
    for (const auto& Reloc : Relocations) {
      if (Reloc.Header.Type == CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE) {
        ThunkRelocCount++;
      } else {
        SmallRelocCount++;
      }
    }

    const uint32_t EntryPointCount = (uint32_t)CompiledCode.EntryPoints.size();

    const size_t HeaderOffset = 0;
    const size_t HostCodeOffset = HeaderOffset + sizeof(BlobFixedHeader);
    const size_t EntryPointRIPsOffset = HostCodeOffset + CompiledCode.Size;
    const size_t EntryPointHostOffsetsOffset = EntryPointRIPsOffset + EntryPointCount * sizeof(uint64_t);
    const size_t SmallRelocsOffset = EntryPointHostOffsetsOffset + EntryPointCount * sizeof(uint32_t);
    const size_t ThunkRelocsOffset = SmallRelocsOffset + SmallRelocCount * sizeof(BlobSmallRelocation);
    const size_t GuestCodeOffset = ThunkRelocsOffset + ThunkRelocCount * sizeof(BlobThunkRelocation);
    const size_t TotalSize = GuestCodeOffset + GuestCode.size();

    // we'll copy everything into here and pass it to the Writer, then return to caller quickly
    auto BlobRef = fextl::make_shared<fextl::vector<uint8_t>>(TotalSize);
    uint8_t* BlobData = BlobRef->data();

    BlobFixedHeader Header {
      .GuestSize = (uint32_t)GuestCode.size(),
      .HostSize = (uint32_t)CompiledCode.Size,
      .EntryPointCount = EntryPointCount,
      .SmallRelocCount = SmallRelocCount,
      .ThunkRelocCount = ThunkRelocCount,
    };

    memcpy(BlobData + HostCodeOffset, CompiledCode.BlockBegin, CompiledCode.Size);

    // pack and relocate entrypoints
    auto* EntryRIPs = reinterpret_cast<uint64_t*>(BlobData + EntryPointRIPsOffset);
    auto* EntryHostOffsets = reinterpret_cast<uint32_t*>(BlobData + EntryPointHostOffsetsOffset);
    uint32_t EntryIdx = 0;
    for (auto [GuestAddr, HostAddr] : CompiledCode.EntryPoints) {
      EntryRIPs[EntryIdx] = GuestAddr - GuestRIP;
      EntryHostOffsets[EntryIdx] = uint32_t(HostAddr - CompiledCode.BlockBegin);
      EntryIdx++;
    }

    fextl::set<uint64_t> DataMasksConsumed;

    // pack relocations
    auto* SmallRelocs = reinterpret_cast<BlobSmallRelocation*>(BlobData + SmallRelocsOffset);
    auto* ThunkRelocs = reinterpret_cast<BlobThunkRelocation*>(BlobData + ThunkRelocsOffset);
    uint32_t SmallIdx = 0;
    uint32_t ThunkIdx = 0;
    for (const auto& Reloc : Relocations) {
      switch (Reloc.Header.Type) {
      // it's important to zero-init the element completely so we don't have garbage in unused fields
      // this way, the caches stay deterministic across machines
      case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = Reloc.Header.Offset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.Named.Symbol = uint32_t(Reloc.NamedSymbolLiteral.Symbol);
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_GUEST_RIP_LITERAL: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = Reloc.Header.Offset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.RIPLiteral.GuestRIP = Reloc.GuestRIP.GuestRIP - GuestRIP;
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE: {
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = Reloc.Header.Offset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.RIPMove.RegisterIndex = Reloc.GuestRIP.RegisterIndex;
        SmallReloc.RIPMove.GuestRIP = Reloc.GuestRIP.GuestRIP - GuestRIP;
        SmallRelocs[SmallIdx++] = SmallReloc;
        break;
      }
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL:
      case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE: {
        // same data for all, relative vs. not and register vs. literal will depend on type on apply
        BlobSmallRelocation SmallReloc = {};
        SmallReloc.Offset = Reloc.Header.Offset;
        SmallReloc.Type = uint8_t(Reloc.Header.Type);
        SmallReloc.PatchableData.RegisterIndex = Reloc.GuestPatchableData.RegisterIndex;
        SmallReloc.PatchableData.ValueSize = Reloc.GuestPatchableData.ValueSize;
        SmallReloc.PatchableData.SiteOffset = uint32_t(Reloc.GuestPatchableData.SiteAddress - GuestRIP);
        SmallRelocs[SmallIdx++] = SmallReloc;

        // mark the corresponding data mask consumed
        DataMasksConsumed.insert(Reloc.GuestPatchableData.SiteAddress);

        break;
      }
      case CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE: {
        BlobThunkRelocation BigReloc = {};
        BigReloc.Offset = Reloc.Header.Offset;
        BigReloc.RegisterIndex = Reloc.NamedThunkMove.RegisterIndex;
        memcpy(BigReloc.SymbolHash, &Reloc.NamedThunkMove.Symbol, sizeof(BigReloc.SymbolHash));
        ThunkRelocs[ThunkIdx++] = BigReloc;
        break;
      }
      }
    }

    fextl::vector<uint32_t> ExactGuestCodeExtents;
    uint64_t CurStartExtent = 0, CurEndExtent = 0;
    const Frontend::Decoder::DecodedBlocks* LastBlock = nullptr;
    for (auto& SubBlock : DecodedBlockInfo->Blocks) {
      if (SubBlock.BlockStatus != Frontend::Decoder::DecodedBlockStatus::SUCCESS) {
        return false;
      }
      if (!CurStartExtent) {
        CurStartExtent = SubBlock.Entry;
        CurEndExtent = SubBlock.Entry + SubBlock.Size;
      } else {
        LOGMAN_THROW_A_FMT(SubBlock.Entry >= CurEndExtent, "DecodedBlocks not sorted or overlapping?");
        if (SubBlock.Entry == CurEndExtent) {
          CurEndExtent = SubBlock.Entry + SubBlock.Size;
        } else {
          ExactGuestCodeExtents.push_back(CurStartExtent - GuestRIP);
          ExactGuestCodeExtents.push_back(CurEndExtent - CurStartExtent);
          CurStartExtent = SubBlock.Entry;
          CurEndExtent = SubBlock.Entry + SubBlock.Size;
        }
      }
      // split extents according to data masks as well
      for (auto& Mask : SubBlock.DataMasks) {
        if (Mask.Type == Frontend::Decoder::DataMaskType::NOP || DataMasksConsumed.find(Mask.FieldAddress) != DataMasksConsumed.end()) {
          if (Mask.FieldAddress > CurStartExtent) {
            ExactGuestCodeExtents.push_back(CurStartExtent - GuestRIP);
            ExactGuestCodeExtents.push_back(Mask.FieldAddress - CurStartExtent);
          }
          CurStartExtent = Mask.FieldAddress + Mask.ValueSize;
        }
      }
      LastBlock = &SubBlock;
    }
    if (LastBlock && (CurStartExtent != GuestRIP || CurEndExtent != GuestRIP + GuestCode.size())) {
      ExactGuestCodeExtents.push_back(CurStartExtent - GuestRIP);
      ExactGuestCodeExtents.push_back(CurEndExtent - CurStartExtent);
    }

    if (ExactGuestCodeExtents.size() == 0) {
      ExactGuestCodeExtents.reserve(2);
      ExactGuestCodeExtents.push_back(0);
      ExactGuestCodeExtents.push_back(GuestCode.size());
    }

    uint64_t GuestFootprint = XXH3_64bits(ExactGuestCodeExtents.data(), ExactGuestCodeExtents.size() * sizeof(uint32_t));

    // if (ExactGuestCodeExtents.size()) {
    //   LogMan::Msg::IFmt("store! length {:d}", GuestCode.size());
    //   for(uint32_t i = 0; i < ExactGuestCodeExtents.size(); i+=2 ) {
    //     LogMan::Msg::IFmt("extent {} {}", ExactGuestCodeExtents[i], ExactGuestCodeExtents[i]+ExactGuestCodeExtents[i+1]);
    //   }
    // }
    {
      XXH3_state_t HashState;
      XXH3_128bits_reset(&HashState);
      for (uint32_t i = 0; i < ExactGuestCodeExtents.size(); i += 2) {
        XXH3_128bits_update(&HashState, GuestCode.data() + ExactGuestCodeExtents[i], ExactGuestCodeExtents[i + 1]);
      }
      Header.GuestHash = XXH3_128bits_digest(&HashState);
    }
    memcpy(BlobData + HeaderOffset, &Header, sizeof(Header));

    auto RangeInfo = CTX->SyscallHandler->QueryGuestExecutableRange(Thread, GuestRIP);
    uint64_t LookupKey =
      MakeLookupKey(Thread, GuestCodeKey, RangeInfo.Writable, GuestRIP == CTX->GetMonoBackPatcherBlock().load(std::memory_order_relaxed));

#ifdef __APPLE__
    // Seal the entry: see BlobFixedHeader::IntegrityHash. The header already in the blob
    // has the field zeroed.
    Header.IntegrityHash = XXH3_128bits_withSeed(BlobData, GuestCodeOffset, LookupKey);
    memcpy(BlobData + HeaderOffset, &Header, sizeof(Header));
#endif

    // blob done, publish to index as in-memory for now, flush to disk below
    struct IndexEntry NewEntry {nullptr, 0, (uint32_t)TotalSize, Header.GuestSize, Header.GuestHash, BlobRef};
    NewEntry.GuestExtents = ExactGuestCodeExtents;
    {
      std::lock_guard Guard(IndexLock);
      auto It = Index.find(LookupKey);
      if (It == Index.end()) {
        Index.emplace(LookupKey, IndexCacheHead {std::move(NewEntry), GuestFootprint, nullptr});
      } else {
        bool Dupe = false;
        if (It->second.MoreEntries.get()) {
          for (auto& [Key, Elem] : *It->second.MoreEntries) {
            if (XXH128_isEqual(Elem.GuestHash, Header.GuestHash)) {
              Dupe = true;
              break;
            }
          }
        }
        if (!Dupe && XXH128_isEqual(It->second.MainEntry.GuestHash, Header.GuestHash)) {
          Dupe = true;
        }
        // could happen if it's seen again while in flight in the store queue
        if (Dupe) {
          return true;
        }
        if (!It->second.MoreEntries.get()) {
          It->second.MoreEntries = fextl::make_unique<fextl::multimap<uint64_t, struct IndexEntry>>();
        } else if (It->second.MoreEntries->size() >= LOOKUP_KEY_MAX_BUCKET_DEPTH) {
          return false;
        }
        It->second.MoreEntries->insert({GuestFootprint, std::move(NewEntry)});
      }
    }

    memcpy(BlobData + GuestCodeOffset, GuestCode.data(), GuestCode.size());

    MesaFOZ::foz_payload_key Key = {};
    {
      XXH3_state_t HashState;
      XXH3_128bits_reset(&HashState);
      XXH3_128bits_update(&HashState, &Header.GuestHash, sizeof(Header.GuestHash));
      XXH3_128bits_update(&HashState, &LookupKey, sizeof(LookupKey));
      XXH128_hash_t UniqueKey = XXH3_128bits_digest(&HashState);
      fextl::string BlobName = fextl::fmt::format("{:016x}{:016x}", UniqueKey.high64, UniqueKey.low64);
      memcpy(Key.bytes, BlobName.data(), BlobName.size());
    }

    fextl::vector<uint8_t> IndexBlob;
    IndexBlob.resize(sizeof(IndexExtraBlobHeader) + ExactGuestCodeExtents.size() * sizeof(uint32_t));

    IndexExtraBlobHeader IndexBlobHeader {Header.GuestHash, GuestFootprint, Header.GuestSize, (uint32_t)ExactGuestCodeExtents.size()};
    memcpy(IndexBlob.data(), &IndexBlobHeader, sizeof(IndexExtraBlobHeader));
    memcpy(IndexBlob.data() + sizeof(IndexExtraBlobHeader), ExactGuestCodeExtents.data(), ExactGuestCodeExtents.size() * sizeof(uint32_t));

    bool StoreDisk = true;

    if (RWCacheDB->Full()) {
      StoreDisk = false;
    }

    // hand the rest off to the writer thread
    Writer->QueueWork(fextl::make_unique<CacheStoreWorkItem>(this, RWCacheDB.get(), Key, LookupKey, std::span(BlobData, TotalSize),
                                                             std::move(IndexBlob), StoreDisk));
    QueuedCount.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  uint16_t GetFormatVersion() {
    return FormatVersion;
  }
} // namespace DiskCache

} // namespace FEXCore
