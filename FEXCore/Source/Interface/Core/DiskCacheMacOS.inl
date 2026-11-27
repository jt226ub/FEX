// SPDX-License-Identifier: MIT
//
// macOS port: the parts of the disk cache that differ on macOS. Included at the end of
// DiskCache.cpp. See docs/macOS-code-cache-design.md for the design and trust model.
//
// In short: a cache file is code that will be executed, so it is only used from a
// directory and files that only this user can write (no group or other write bits, no
// ACL entry granting write, no symbolic links), every entry is checked in full before
// any of it reaches executable memory, and the index is read once and parsed lazily.

#include <git_version.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace FEXCore::DiskCache {

namespace {
  // Bump when this port changes the cache's contents or checks.
  // 2: guest extents in the integrity hash; entry-point bounds; lazy index.
  constexpr uint32_t PortCacheRevision = 2;

  constexpr size_t FOZRecordHeaderSize = sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header);
  constexpr size_t IndexRecordMinPayload = sizeof(MesaFOZ::mesa_index_db_file_entry) + sizeof(IndexExtraBlobHeader);

  // True if any ACL entry on the object allows something other than reading: writing or
  // appending data, adding or deleting entries, or changing attributes, security or owner.
  // Deny entries are fine (macOS puts "everyone deny delete" on some home folders).
  bool HasWriteACL(int FD) {
    acl_t ACL = acl_get_fd_np(FD, ACL_TYPE_EXTENDED);
    if (!ACL) {
      // ENOENT: no ACL. Any other error (a file system without ACLs) also means none.
      return false;
    }
    static constexpr acl_perm_t WritePerms[] = {
      ACL_WRITE_DATA,       ACL_APPEND_DATA,         ACL_DELETE,         ACL_DELETE_CHILD, ACL_WRITE_ATTRIBUTES,
      ACL_WRITE_EXTATTRIBUTES, ACL_WRITE_SECURITY,   ACL_CHANGE_OWNER,
    };
    bool Found = false;
    acl_entry_t Entry;
    for (int Which = ACL_FIRST_ENTRY; !Found && acl_get_entry(ACL, Which, &Entry) == 0; Which = ACL_NEXT_ENTRY) {
      acl_tag_t Tag;
      acl_permset_t Perms;
      if (acl_get_tag_type(Entry, &Tag) != 0 || Tag != ACL_EXTENDED_ALLOW || acl_get_permset(Entry, &Perms) != 0) {
        continue;
      }
      for (auto Perm : WritePerms) {
        if (acl_get_perm_np(Perms, Perm) == 1) {
          Found = true;
          break;
        }
      }
    }
    acl_free(ACL);
    return Found;
  }

  // A directory may hold cache files only if this user owns it and nobody else can change
  // what is in it. The bucket directory must also be private (we create it 0700).
  bool DirectoryIsTrusted(int FD, std::string_view Name, bool Private) {
    struct stat Info;
    const char* Why = nullptr;
    if (fstat(FD, &Info) != 0 || !S_ISDIR(Info.st_mode)) {
      Why = "is not a directory";
    } else if (Info.st_uid != geteuid()) {
      Why = "is not owned by this user";
    } else if (Info.st_mode & (Private ? (S_IRWXG | S_IRWXO) : (S_IWGRP | S_IWOTH))) {
      Why = Private ? "is not private to this user" : "is writable by other users";
    } else if (HasWriteACL(FD)) {
      Why = "has an access control entry that lets others change it";
    }
    if (Why) {
      LogMan::Msg::EFmt("DiskCache: {} {}; disk cache disabled", Name, Why);
      return false;
    }
    return true;
  }

  // A cache file is trusted only if it is a regular file with one name, owned by this
  // user, private to it (0600 or tighter) and without an ACL that grants write.
  bool FileIsTrusted(int FD, std::string_view Name) {
    struct stat Info;
    const char* Why = nullptr;
    if (fstat(FD, &Info) != 0 || !S_ISREG(Info.st_mode)) {
      Why = "is not a regular file";
    } else if (Info.st_uid != geteuid()) {
      Why = "is not owned by this user";
    } else if (Info.st_nlink != 1) {
      Why = "has more than one name";
    } else if (Info.st_mode & (S_IRWXG | S_IRWXO)) {
      Why = "is not private to this user";
    } else if (HasWriteACL(FD)) {
      Why = "has an access control entry that lets others change it";
    }
    if (Why) {
      LogMan::Msg::EFmt("DiskCache: {} {}; not using it", Name, Why);
      return false;
    }
    return true;
  }

  // The build identity: the LC_UUID of the Mach-O image the core's code is in. The linker
  // derives it from the image's contents, so any change to the code changes it.
  bool AppendImageUUID(fextl::vector<uint8_t>& Out) {
    Dl_info Info {};
    if (!dladdr(reinterpret_cast<const void*>(&AppendImageUUID), &Info) || !Info.dli_fbase) {
      return false;
    }
    const auto* Header = static_cast<const mach_header_64*>(Info.dli_fbase);
    if (Header->magic != MH_MAGIC_64) {
      return false;
    }
    const auto* Command = reinterpret_cast<const load_command*>(Header + 1);
    for (uint32_t i = 0; i < Header->ncmds; ++i) {
      if (Command->cmd == LC_UUID) {
        const auto* UUID = reinterpret_cast<const uuid_command*>(Command);
        Out.insert(Out.end(), UUID->uuid, UUID->uuid + sizeof(UUID->uuid));
        return true;
      }
      Command = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(Command) + Command->cmdsize);
    }
    return false;
  }

  // Empty if the build cannot be identified, in which case nothing is cached.
  fextl::vector<uint8_t> MakePortIdentity(const FEXCore::Context::ContextImpl& CTX) {
    fextl::vector<uint8_t> Out;
    const char Tag[8] = {'F', 'E', 'X', 'M', 'A', 'C', 'O', 'S'};
    Out.insert(Out.end(), Tag, Tag + sizeof(Tag));
    Out.insert(Out.end(), reinterpret_cast<const uint8_t*>(&PortCacheRevision),
               reinterpret_cast<const uint8_t*>(&PortCacheRevision) + sizeof(PortCacheRevision));
    Out.insert(Out.end(), GIT_HASH.begin(), GIT_HASH.end());
    if (!AppendImageUUID(Out)) {
      return {};
    }
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

  // Workaround: on APFS, when several processes create the same new file at once with
  // openat(O_CREAT), a loser can get ENOENT although the directory exists (seen about one
  // run in five with six processes starting together; a ten-line C program shows it,
  // with or without O_NOFOLLOW). There is no flag that avoids it, so a create that fails
  // with ENOENT is retried briefly.
  int OpenAtRetrying(int DirFD, const char* Name, int Flags) {
    int Handle = -1;
    for (int Attempt = 0; Attempt < 100; ++Attempt) {
      Handle = openat(DirFD, Name, Flags, S_IRUSR | S_IWUSR);
      if (Handle >= 0 || errno != ENOENT || !(Flags & O_CREAT)) {
        break;
      }
      usleep(1000);
    }
    return Handle;
  }

  constexpr const char* LastUsedMarker = "last-used";
  constexpr time_t KeepIdleBucketSeconds = 7 * 24 * 60 * 60;

  // Largest number of bytes each relocation type rewrites at its offset: a 64-bit literal,
  // or a padded four-instruction constant load.
  uint32_t RelocationWriteSize(uint8_t Type) {
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
} // namespace

namespace MacOS {
  XXH128_hash_t EntryHash(const uint8_t* Blob, size_t Size, std::span<const uint32_t> GuestExtents, uint64_t LookupKey) {
    BlobFixedHeader Zeroed;
    memcpy(&Zeroed, Blob, sizeof(Zeroed));
    Zeroed.IntegrityHash = {};
    // XXH3_INITSTATE is required for a state on the stack used with a seed: the reset
    // compares the seed with the state's previous one to skip rebuilding its secret, and
    // stack garbage left by an earlier hash with the same seed made that skip happen with
    // a garbage secret (seen as random integrity failures on worker threads).
    XXH3_state_t HashState;
    XXH3_INITSTATE(&HashState);
    XXH3_128bits_reset_withSeed(&HashState, LookupKey);
    XXH3_128bits_update(&HashState, &Zeroed, sizeof(Zeroed));
    XXH3_128bits_update(&HashState, Blob + sizeof(Zeroed), Size - sizeof(Zeroed));
    XXH3_128bits_update(&HashState, GuestExtents.data(), GuestExtents.size_bytes());
    return XXH3_128bits_digest(&HashState);
  }

  // Pruning keeps a directory unless it is named like a bucket (16 hex digits, so a cache
  // path that holds other things is safe) and nobody has used it for a week. Use, not
  // writes: every process touches the bucket's marker when it opens it.
  bool KeepBucket(std::string_view CacheBase, std::string_view Name) {
    if (Name.size() != 16 || !std::all_of(Name.begin(), Name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
      return true;
    }
    const auto Path = fextl::fmt::format("{}/{}", CacheBase, Name);
    struct stat Info;
    if (lstat(Path.c_str(), &Info) != 0 || !S_ISDIR(Info.st_mode)) {
      return true; // not ours to judge
    }
    const time_t Now = time(nullptr);
    if (Now - Info.st_mtime < KeepIdleBucketSeconds) {
      return true;
    }
    if (lstat(fextl::fmt::format("{}/{}", Path, LastUsedMarker).c_str(), &Info) == 0 && Now - Info.st_mtime < KeepIdleBucketSeconds) {
      return true;
    }
    return false;
  }
} // namespace MacOS

bool FOZFile::OpenAt(int DirFD, const fextl::string& Leaf, bool ReadOnly, bool& Refused) {
  Refused = false;
  FileName = Leaf;
  this->ReadOnly = ReadOnly;
  FD.reset();

  // O_NOFOLLOW: a symbolic link in place of the file is refused (ELOOP), never followed.
  const int Flags = (ReadOnly ? O_RDONLY : (O_RDWR | O_CREAT)) | O_NOFOLLOW | O_CLOEXEC;
  const int Handle = OpenAtRetrying(DirFD, Leaf.c_str(), Flags);
  if (Handle < 0) {
    if (errno == ELOOP) {
      LogMan::Msg::EFmt("DiskCache: {} is a symbolic link; not using it", Leaf);
      Refused = true;
    } else {
      LogMan::Msg::IFmt("DiskCache: cannot open {}: {}", Leaf, strerror(errno));
    }
    return false;
  }
  if (!FileIsTrusted(Handle, Leaf)) {
    close(Handle);
    Refused = true;
    return false;
  }
  FD = fextl::make_unique<File::File>(File::File::FromOwnedHandle(Handle, false));

  // As FOZFile::Open: write the magic into a new file, check it in an existing one.
  bool Valid = false;
  bool TookLock = false;
  ssize_t Size = FD->Size();
  if (Size < FOZ_REF_MAGIC_SIZE && !ReadOnly) {
    if (!FD->Lock(OPEN_LOCK_TIMEOUT_MS)) {
      LogMan::Msg::IFmt("DiskCache: {} stayed locked by another process", Leaf);
      FD.reset();
      return false;
    }
    TookLock = true;
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
    LogMan::Msg::EFmt("DiskCache: {} is not a cache file of this format", Leaf);
    Refused = true;
    FD.reset();
  }
  return Valid;
}

bool FOZFile::FlushToDisk() {
  if (!FD) {
    return false;
  }
  // F_FULLFSYNC is the macOS way to push data through the drive's own cache; plain fsync
  // only reaches the drive.
  if (fcntl(FD->GetHandle(), F_FULLFSYNC) == 0) {
    return true;
  }
  return fsync(FD->GetHandle()) == 0;
}

bool FOZFile::Truncate(uint64_t NewSize) {
  return FD && ftruncate(FD->GetHandle(), NewSize) == 0;
}

bool IndexedDB::OpenAt(int DirFD, const fextl::string& Leaf, bool ReadOnly, uint64_t) {
  this->DirFD = DirFD;
  Name = Leaf;
  this->ReadOnly = ReadOnly;
  const auto CacheName = Leaf + ".foz";
  const auto IndexName = Leaf + "_idx.foz";
  for (int Attempt = 0; Attempt < 2; ++Attempt) {
    bool CacheRefused = false;
    bool IndexRefused = false;
    if (CacheFOZ.OpenAt(DirFD, CacheName, ReadOnly, CacheRefused) && IndexFOZ.OpenAt(DirFD, IndexName, ReadOnly, IndexRefused)) {
      return true;
    }
    CacheFOZ = FOZFile {};
    IndexFOZ = FOZFile {};
    if (ReadOnly || Attempt != 0 || !(CacheRefused || IndexRefused)) {
      return false;
    }
    // The directory is verified private, so removing names from it is safe. Both files
    // go: one half of a database is no use without the other.
    LogMan::Msg::IFmt("DiskCache: removing {} and starting it again", Leaf);
    unlinkat(DirFD, CacheName.c_str(), 0);
    unlinkat(DirFD, IndexName.c_str(), 0);
  }
  return false;
}

bool IndexedDB::LoadIndex(uint64_t MaxSize, bool& FoundMetadata) {
  const ssize_t IndexSize = IndexFOZ.Size();
  const ssize_t CacheSize = CacheFOZ.Size();
  // Record offsets are kept in 32 bits; the size limit keeps a database far below that.
  if (IndexSize < FOZ_REF_MAGIC_SIZE || CacheSize < FOZ_REF_MAGIC_SIZE || uint64_t(IndexSize) > std::min<uint64_t>(MaxSize, ResolvedSlot) ||
      uint64_t(CacheSize) > MaxSize) {
    LogMan::Msg::EFmt("DiskCache: {} is larger than the cache size limit", Name);
    return false;
  }
  CacheFOZSizeAtOpen = CacheSize;
  if (!IndexFOZ.ReadAll(IndexData)) {
    return false;
  }

  // One pass over the record framing: find the description, count the data records and,
  // for a read-write database, find where a partly written last record starts.
  auto Walk = [&](auto&& OnRecord) {
    uint64_t Offset = 0;
    while (Offset + FOZRecordHeaderSize <= IndexData.size()) {
      MesaFOZ::foz_payload_header Header;
      memcpy(&Header, IndexData.data() + Offset + sizeof(MesaFOZ::foz_payload_key), sizeof(Header));
      if (Offset + FOZRecordHeaderSize + Header.payload_size > IndexData.size()) {
        break;
      }
      OnRecord(Offset, Header.payload_size);
      Offset += FOZRecordHeaderSize + Header.payload_size;
    }
    return Offset;
  };

  DataRecords = 0;
  const uint64_t ValidEnd = Walk([&](uint64_t Offset, uint32_t Payload) {
    if (Payload < sizeof(MesaFOZ::mesa_index_db_file_entry)) {
      return;
    }
    MesaFOZ::mesa_index_db_file_entry Entry;
    memcpy(&Entry, IndexData.data() + Offset + FOZRecordHeaderSize, sizeof(Entry));
    if (IndexData[Offset + sizeof(MesaFOZ::foz_payload_key) - 1] == 0xFF) {
      FoundMetadata = true;
      if (!MetadataSize && Entry.cache_db_file_offset <= CacheFOZSizeAtOpen && Entry.size <= CacheFOZSizeAtOpen - Entry.cache_db_file_offset) {
        MetadataOffset = Entry.cache_db_file_offset;
        MetadataSize = Entry.size;
      }
    } else {
      ++DataRecords;
    }
  });

  IndexKnownEnd = FOZ_REF_MAGIC_SIZE + ValidEnd;
  if (ValidEnd != IndexData.size()) {
    if (!ReadOnly) {
      // The caller holds both locks, so this record is not being written right now: a
      // process died while appending it. Cut it off, or every later record would be
      // unreachable.
      LogMan::Msg::IFmt("DiskCache: dropping {} bytes of a partly written record at the end of {}", IndexData.size() - ValidEnd, Name);
      if (!IndexFOZ.Truncate(ValidEnd + FOZ_REF_MAGIC_SIZE)) {
        return false;
      }
    }
    IndexData.resize(ValidEnd);
  }

  // Open-addressed table of (lookup key, record offset), at most half full.
  size_t Capacity = 16;
  while (Capacity < DataRecords * 2) {
    Capacity *= 2;
  }
  LazyTable.assign(Capacity, LazySlot {0, EmptySlot});
  const uint64_t Mask = Capacity - 1;
  Walk([&](uint64_t Offset, uint32_t Payload) {
    if (Payload < sizeof(MesaFOZ::mesa_index_db_file_entry) || IndexData[Offset + sizeof(MesaFOZ::foz_payload_key) - 1] == 0xFF) {
      return;
    }
    uint64_t Key;
    memcpy(&Key, IndexData.data() + Offset + FOZRecordHeaderSize, sizeof(Key));
    for (uint64_t Slot = (Key * 0x9E3779B97F4A7C15ULL) & Mask;; Slot = (Slot + 1) & Mask) {
      if (LazyTable[Slot].Offset == EmptySlot) {
        LazyTable[Slot] = {Key, static_cast<uint32_t>(Offset)};
        break;
      }
    }
  });
  return true;
}

// Parses one index record into the in-memory index. The index file is outside the
// entries' integrity hashes except for the extents (which are hashed into them), so
// every field is checked here before anything trusts it.
bool IndexedDB::ParseRecord(uint32_t Offset, Index& CacheIndex) {
  MesaFOZ::foz_payload_header Header;
  memcpy(&Header, IndexData.data() + Offset + sizeof(MesaFOZ::foz_payload_key), sizeof(Header));
  if (Header.payload_size < IndexRecordMinPayload) {
    return false;
  }
  const uint8_t* Body = IndexData.data() + Offset + FOZRecordHeaderSize;
  MesaFOZ::mesa_index_db_file_entry Common;
  IndexExtraBlobHeader Extra;
  memcpy(&Common, Body, sizeof(Common));
  memcpy(&Extra, Body + sizeof(Common), sizeof(Extra));
  if (Common.cache_db_file_offset > CacheFOZSizeAtOpen || Common.size > CacheFOZSizeAtOpen - Common.cache_db_file_offset) {
    return false;
  }
  if (Extra.GuestSize == 0 || uint64_t(Extra.GuestSize) + sizeof(BlobFixedHeader) > Common.size) {
    return false;
  }
  // Extents come in (offset, length) pairs.
  const uint32_t Count = Extra.GuestExtentsCount;
  if (Count == 0 || Count % 2 != 0 || Header.payload_size != IndexRecordMinPayload + uint64_t(Count) * sizeof(uint32_t)) {
    return false;
  }
  IndexEntry NewEntry {this, Common.cache_db_file_offset, Common.size, Extra.GuestSize, Extra.GuestHash};
  NewEntry.GuestExtents.resize(Count);
  memcpy(NewEntry.GuestExtents.data(), Body + IndexRecordMinPayload, Count * sizeof(uint32_t));
  // Inside the guest size, in order and not overlapping, as Store makes them. (Store can
  // make an empty last extent, when a masked field ends the code.)
  uint64_t PreviousEnd = 0;
  for (uint32_t i = 0; i < Count; i += 2) {
    const uint64_t Begin = NewEntry.GuestExtents[i];
    const uint64_t End = Begin + NewEntry.GuestExtents[i + 1];
    if (End > Extra.GuestSize || Begin < PreviousEnd) {
      return false;
    }
    PreviousEnd = End;
  }
  auto It = CacheIndex.find(Common.hash);
  if (It == CacheIndex.end()) {
    CacheIndex.emplace(Common.hash, IndexCacheHead {std::move(NewEntry), Extra.GuestFootprint, nullptr});
  } else {
    if (!It->second.MoreEntries) {
      It->second.MoreEntries = fextl::make_unique<fextl::multimap<uint64_t, IndexEntry>>();
    }
    It->second.MoreEntries->insert({Extra.GuestFootprint, std::move(NewEntry)});
  }
  return true;
}

void IndexedDB::ResolveKey(uint64_t LookupKey, Index& CacheIndex) {
  if (LazyTable.empty()) {
    return;
  }
  const uint64_t Mask = LazyTable.size() - 1;
  fextl::vector<uint32_t> Offsets;
  for (uint64_t Slot = (LookupKey * 0x9E3779B97F4A7C15ULL) & Mask; LazyTable[Slot].Offset != EmptySlot; Slot = (Slot + 1) & Mask) {
    auto& Entry = LazyTable[Slot];
    if (Entry.Key == LookupKey && Entry.Offset != ResolvedSlot) {
      Offsets.push_back(Entry.Offset);
      Entry.Offset = ResolvedSlot;
    }
  }
  // In file order, as upstream's PopulateIndex adds them: the first becomes the key's main
  // entry, and upstream's candidate walk in Lookup depends on that order.
  std::sort(Offsets.begin(), Offsets.end());
  for (uint32_t Offset : Offsets) {
    ParseRecord(Offset, CacheIndex);
  }
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

bool IndexedDB::Reset() {
  if (ReadOnly || DirFD < 0) {
    return false;
  }
  MetadataOffset = 0;
  MetadataSize = 0;
  DataRecords = 0;
  IndexData.clear();
  LazyTable.clear();
  // Replace the files rather than truncate them: another process may still be appending
  // to them and expects what it already wrote to stay where it is (see RepairIndexTail).
  // Its later writes go to the removed files and are lost, which is harmless.
  unlinkat(DirFD, (Name + ".foz").c_str(), 0);
  unlinkat(DirFD, (Name + "_idx.foz").c_str(), 0);
  CacheFOZ = FOZFile {};
  IndexFOZ = FOZFile {};
  bool Refused = false;
  if (!CacheFOZ.OpenAt(DirFD, Name + ".foz", false, Refused) || !IndexFOZ.OpenAt(DirFD, Name + "_idx.foz", false, Refused)) {
    return false;
  }
  CacheFOZSizeAtOpen = FOZ_REF_MAGIC_SIZE;
  IndexKnownEnd = FOZ_REF_MAGIC_SIZE;
  return true;
}

bool IndexedDB::RepairIndexTail() {
  // Called with both locks held, before appending. A process that died while appending
  // leaves a partial last index record; appending after it would hide every later record
  // (the framing would run through them). IndexKnownEnd is the end of the last whole
  // record this process has seen, so only what others appended since is checked.
  const ssize_t Size = IndexFOZ.Size();
  if (Size < FOZ_REF_MAGIC_SIZE) {
    return false;
  }
  if (uint64_t(Size) < IndexKnownEnd) {
    // Only a repair like this one shortens the file, and only past every whole record.
    IndexKnownEnd = FOZ_REF_MAGIC_SIZE;
  }
  fextl::vector<uint8_t> Tail(Size - IndexKnownEnd);
  if (!Tail.empty() && !IndexFOZ.ReadBlob(IndexKnownEnd, Tail)) {
    return false;
  }
  uint64_t Offset = 0;
  while (Offset + FOZRecordHeaderSize <= Tail.size()) {
    MesaFOZ::foz_payload_header Header;
    memcpy(&Header, Tail.data() + Offset + sizeof(MesaFOZ::foz_payload_key), sizeof(Header));
    if (Offset + FOZRecordHeaderSize + Header.payload_size > Tail.size()) {
      break;
    }
    Offset += FOZRecordHeaderSize + Header.payload_size;
  }
  if (Offset != Tail.size()) {
    LogMan::Msg::IFmt("DiskCache: dropping {} bytes of a partly written record at the end of {}", Tail.size() - Offset, Name);
    if (!IndexFOZ.Truncate(IndexKnownEnd + Offset)) {
      return false;
    }
  }
  IndexKnownEnd += Offset;
  return true;
}

bool IndexedDB::FlushToDisk() {
  if (ReadOnly) {
    return true;
  }
  const bool CacheOk = CacheFOZ.FlushToDisk();
  return IndexFOZ.FlushToDisk() && CacheOk;
}

bool IndexedDB::WithinSizeLimit(uint64_t BlobSize, uint64_t IndexRecordSize) {
  const ssize_t CacheSize = CacheFOZ.Size();
  const ssize_t IndexSize = IndexFOZ.Size();
  if (CacheSize < 0 || IndexSize < 0) {
    return false;
  }
  const uint64_t Limit = std::min<uint64_t>(MaxFileSize, ResolvedSlot);
  return uint64_t(CacheSize) + FOZRecordHeaderSize + BlobSize <= Limit && uint64_t(IndexSize) + IndexRecordSize <= Limit;
}

uint8_t DiskCache::CodeGenStateFlags() const {
  uint8_t Flags = 0;
  if (CTX->IsAtomicTSOEnabled()) {
    Flags |= 1 << 4;
  }
  if (CTX->IsVectorAtomicTSOEnabled()) {
    Flags |= 1 << 5;
  }
  if (CTX->IsMemcpyAtomicTSOEnabled()) {
    Flags |= 1 << 6;
  }
  return Flags;
}

bool DiskCache::MayUseCache(uint64_t GuestRIP, uint64_t GuestSize, std::span<const uint64_t> EntryPoints) {
  // Instructions forced to TSO by AddForceTSOInformation are compiled differently from
  // what their bytes say, and a custom IR handler replaces the bytes altogether.
  if (CTX->ForceTSOIntersects(GuestRIP, GuestRIP + GuestSize) || CTX->HasCustomIRHandlerAt(GuestRIP)) {
    return false;
  }
  for (uint64_t Entry : EntryPoints) {
    if (CTX->HasCustomIRHandlerAt(Entry)) {
      return false;
    }
  }
  return true;
}

void DiskCache::ResolveLazyLocked(uint64_t LookupKey) {
  if (RWCacheDB) {
    RWCacheDB->ResolveKey(LookupKey, Index);
  }
  for (auto& DB : ROCacheDBs) {
    DB->ResolveKey(LookupKey, Index);
  }
}

bool DiskCache::CheckMetadata(IndexedDB& DB) {
  if (DB.MetadataSize == 0) {
    // A database without a description is acceptable only while it holds nothing, which
    // is the state of a new file before the description is appended.
    if (DB.DataRecords == 0) {
      return true;
    }
    LogMan::Msg::EFmt("DiskCache: {} has entries but no description; not using it", DB.Name);
    return false;
  }
  fextl::vector<uint8_t> Stored(DB.MetadataSize);
  if (DB.MetadataSize != ExpectedMetadata.size() || !DB.ReadCacheBlob(DB.MetadataOffset, Stored) ||
      memcmp(Stored.data(), ExpectedMetadata.data(), Stored.size()) != 0) {
    LogMan::Msg::EFmt("DiskCache: {} was made for a different configuration or build, or is damaged; not using it", DB.Name);
    return false;
  }
  return true;
}

bool DiskCache::OpenCacheDBMacOS(const fextl::string& Leaf, bool ReadOnly) {
  if ((!ReadOnly && RWCacheDB) || BucketDirFD < 0 || Leaf.empty() || Leaf.find('/') != fextl::string::npos) {
    return false;
  }
  auto CurDB = fextl::make_unique<IndexedDB>();
  if (!CurDB->OpenAt(BucketDirFD, Leaf, ReadOnly, 0)) {
    LogMan::Msg::IFmt("DiskCache: could not open {}; translating without it", Leaf);
    return false;
  }
  // A read-write database is shared with other processes, which append under the file
  // locks. Holding both locks while reading it means no record is half written.
  if (!ReadOnly && !CurDB->LockForOpen()) {
    LogMan::Msg::IFmt("DiskCache: {} is busy; translating without it", Leaf);
    return false;
  }
  FEX_CONFIG_OPT(MaxSize, DISKCACHEMAXFILESIZE);
  bool LocalFoundMetadata = false;
  if (!CurDB->LoadIndex(MaxSize, LocalFoundMetadata) || !CheckMetadata(*CurDB)) {
    RejectedCount.fetch_add(1, std::memory_order_relaxed);
    if (ReadOnly || !CurDB->Reset()) {
      if (!ReadOnly) {
        CurDB->UnlockAfterOpen();
      }
      return false;
    }
    // The cache directory is disposable: start this database again rather than
    // translating without a cache from now on.
    LogMan::Msg::IFmt("DiskCache: emptied {} and starting it again", Leaf);
    LocalFoundMetadata = false;
  }
  if (!ReadOnly) {
    CurDB->UnlockAfterOpen();
  }
  FoundMetadata |= LocalFoundMetadata;
  if (ReadOnly) {
    ROCacheDBs.push_back(std::move(CurDB));
  } else {
    RWCacheDB = std::move(CurDB);
  }
  return true;
}

void DiskCache::InitMacOS(const fextl::vector<uint8_t>& BucketBytes, size_t MachineDataSize, uint64_t ProcessBucketHash) {
  // Upstream relies on FormatVersion being bumped by hand whenever code generation
  // changes. The machine bucket here also holds the port identity: the exact build of the
  // core (Mach-O UUID), the CPU model (it reaches the guest through CPUID) and this port's
  // cache revision.
  const auto PortIdentity = MakePortIdentity(*CTX);
  if (PortIdentity.empty()) {
    LogMan::Msg::EFmt("DiskCache: the core's build cannot be identified (no LC_UUID); disk cache disabled");
    return;
  }
  ExpectedMetadata.assign(BucketBytes.begin(), BucketBytes.begin() + MachineDataSize);
  ExpectedMetadata.insert(ExpectedMetadata.end(), PortIdentity.begin(), PortIdentity.end());
  ExpectedMetadata.insert(ExpectedMetadata.end(), BucketBytes.begin() + MachineDataSize, BucketBytes.end());
  const uint64_t MachineBucketHash = XXH3_64bits(ExpectedMetadata.data(), MachineDataSize + PortIdentity.size());
  BucketHash.high64 = MachineBucketHash;

  fextl::string BasePath = BasePathOverride();
  if (BasePath.empty()) {
    BasePath = FEXCore::Config::GetCacheDirectory() + "DiskCache";
  }
  while (BasePath.size() > 1 && BasePath.ends_with('/')) {
    BasePath.pop_back();
  }
  // Create the base directory private; never change an existing one.
  if (const auto Slash = BasePath.rfind('/'); Slash != fextl::string::npos && Slash > 0) {
    FHU::Filesystem::CreateDirectories(BasePath.substr(0, Slash));
  }
  mkdir(BasePath.c_str(), S_IRWXU);
  BaseDirFD = open(BasePath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (BaseDirFD < 0) {
    LogMan::Msg::EFmt("DiskCache: cannot open {} (it must be a directory, not a link); disk cache disabled", BasePath);
    return;
  }
  if (!DirectoryIsTrusted(BaseDirFD, BasePath, false)) {
    close(BaseDirFD);
    BaseDirFD = -1;
    return;
  }

  const auto BucketName = fextl::fmt::format("{:016x}", MachineBucketHash);
  PruneStaleEntries(BasePath, BucketName);

  mkdirat(BaseDirFD, BucketName.c_str(), S_IRWXU);
  BucketDirFD = openat(BaseDirFD, BucketName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const auto BucketPath = fextl::fmt::format("{}/{}", BasePath, BucketName);
  if (BucketDirFD < 0 || !DirectoryIsTrusted(BucketDirFD, BucketPath, true)) {
    if (BucketDirFD < 0) {
      LogMan::Msg::EFmt("DiskCache: cannot open {} (it must be a directory, not a link); disk cache disabled", BucketPath);
    } else {
      close(BucketDirFD);
      BucketDirFD = -1;
    }
    return;
  }

  // Record that this bucket is in use, for pruning: one small write per process start.
  if (const int Marker = OpenAtRetrying(BucketDirFD, LastUsedMarker, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC); Marker >= 0) {
    futimens(Marker, nullptr);
    close(Marker);
  }

  FileMapper = nullptr; // files are read with pread on macOS

  OpenCacheDB(fextl::fmt::format("RWCacheDB_{:016x}", ProcessBucketHash), false);
  if (RWCacheDB && !FoundMetadata) {
    // A fresh database: its first record is the description.
    MesaFOZ::foz_payload_key MetadataKey;
    memset(MetadataKey.bytes, 0xFF, sizeof(MetadataKey));
    IndexExtraBlobHeader MetaDataHeader = {};
    MesaFOZ::mesa_index_db_file_entry IndexEntry = {};
    RWCacheDB->StoreCacheBlob(MetadataKey, ~0, {ExpectedMetadata.data(), ExpectedMetadata.size()}, IndexEntry,
                              {reinterpret_cast<uint8_t*>(&MetaDataHeader), sizeof(MetaDataHeader)});
    Index.erase(~0);
  }

  std::string_view RONames = RODBNames();
  while (!RONames.empty()) {
    const auto Delim = RONames.find(',');
    const std::string_view ROName = RONames.substr(0, Delim);
    if (!ROName.empty()) {
      OpenCacheDB(fextl::string(ROName), true);
    }
    if (Delim == std::string_view::npos) {
      break;
    }
    RONames.remove_prefix(Delim + 1);
  }

  WritingDiskCache = (bool)RWCacheDB;
  ReadingDiskCache = !ROCacheDBs.empty() || RWCacheDB != nullptr;
  if (IsWritingDiskCache()) {
    FEXCore::Threads::Flags WriterThreadFlags = {.LowPriority = true, .Internal = true};
    Writer = fextl::make_unique<WorkQueueThread>(WriterThreadFlags, "FEX:DiskCache");
  }
}

// Checks a cache entry before any of it is used. Body is the entry without its guest code
// copy, starting with the fixed header. Everything the loader will index or write through
// is bounds-checked here, so the relocation code and the copy into executable memory only
// ever see a consistent entry.
bool DiskCache::ValidateHit(const BlobFixedHeader& Header, std::span<const uint8_t> Body, uint64_t LookupKey, uint64_t GuestSizeAvailable,
                            std::span<const uint32_t> GuestExtents) {
  // Says which check failed (by its position below), once per process, for diagnosis.
  auto Reject = [](int Check) {
    static std::atomic<bool> Logged {};
    if (!Logged.exchange(true)) {
      LogMan::Msg::EFmt("DiskCache: entry check {} failed", Check);
    }
    return false;
  };
  // 1. Integrity: header, code, entry points, relocations and the index's guest extents.
  if (!XXH128_isEqual(MacOS::EntryHash(Body.data(), Body.size(), GuestExtents, LookupKey), Header.IntegrityHash)) {
    return Reject(4);
  }

  // 2. Shape: code size, block header and tail.
  if (Header.HostSize < sizeof(CPU::CPUBackend::JITCodeHeader) + sizeof(CPU::CPUBackend::JITCodeTail) || Header.HostSize % 16 != 0 ||
      Header.EntryPointCount == 0 || Header.GuestSize == 0 || Header.GuestSize > GuestSizeAvailable) {
    return Reject(10);
  }
  const uint8_t* Code = Body.data() + sizeof(Header);
  CPU::CPUBackend::JITCodeHeader CodeHeader;
  memcpy(&CodeHeader, Code, sizeof(CodeHeader));
  const uint64_t TailOffset = CodeHeader.OffsetToBlockTail;
  if (TailOffset < sizeof(CodeHeader) || TailOffset % alignof(CPU::CPUBackend::JITCodeTail) != 0 ||
      TailOffset + sizeof(CPU::CPUBackend::JITCodeTail) > Header.HostSize) {
    return Reject(18);
  }
  CPU::CPUBackend::JITCodeTail Tail;
  memcpy(&Tail, Code + TailOffset, sizeof(Tail));
  if (Tail.Size != Header.HostSize) {
    return Reject(23);
  }

  // 3. Entry points: the guest side inside the bytes the block was translated from, the
  // host side inside the code before the tail (the tail and what follows are data).
  const uint8_t* Cursor = Code + Header.HostSize;
  bool HasBlockEntry = false;
  for (uint32_t i = 0; i < Header.EntryPointCount; ++i) {
    uint64_t RIPOffset;
    memcpy(&RIPOffset, Cursor + i * sizeof(uint64_t), sizeof(RIPOffset));
    HasBlockEntry |= RIPOffset == 0;
    bool InExtent = false;
    for (size_t e = 0; e + 1 < GuestExtents.size(); e += 2) {
      if (RIPOffset >= GuestExtents[e] && RIPOffset < uint64_t(GuestExtents[e]) + GuestExtents[e + 1]) {
        InExtent = true;
        break;
      }
    }
    if (!InExtent) {
      return Reject(42);
    }
  }
  Cursor += Header.EntryPointCount * sizeof(uint64_t);
  for (uint32_t i = 0; i < Header.EntryPointCount; ++i) {
    uint32_t HostOffset;
    memcpy(&HostOffset, Cursor + i * sizeof(uint32_t), sizeof(HostOffset));
    if (HostOffset < sizeof(CodeHeader) || HostOffset >= TailOffset || HostOffset % 4 != 0) {
      return Reject(50);
    }
  }
  Cursor += Header.EntryPointCount * sizeof(uint32_t);
  if (!HasBlockEntry) {
    return Reject(55);
  }

  // 4. Relocations: known type, write inside the code, operands in range.
  for (uint32_t i = 0; i < Header.SmallRelocCount; ++i) {
    BlobSmallRelocation Reloc;
    memcpy(&Reloc, Cursor + i * sizeof(Reloc), sizeof(Reloc));
    const uint32_t WriteSize = RelocationWriteSize(Reloc.Type);
    if (WriteSize == 0 || Reloc.Type == uint8_t(CPU::RelocationTypes::RELOC_NAMED_THUNK_MOVE) || Reloc.Offset % 4 != 0 ||
        uint64_t(Reloc.Offset) + WriteSize > Header.HostSize) {
      return Reject(65);
    }
    switch (static_cast<CPU::RelocationTypes>(Reloc.Type)) {
    case CPU::RelocationTypes::RELOC_NAMED_SYMBOL_LITERAL:
      if (Reloc.Named.Symbol > uint32_t(CPU::RelocNamedSymbolLiteral::NamedSymbol::SYMBOL_LITERAL_EXITFUNCTION_LINKER)) {
        return Reject(70);
      }
      break;
    case CPU::RelocationTypes::RELOC_GUEST_RIP_MOVE:
      if (Reloc.RIPMove.RegisterIndex >= 32) {
        return Reject(75);
      }
      break;
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_DATA_MOVE:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_LITERAL:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_RIP_MOVE:
    case CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE: {
      // These read live guest bytes at the site, which must lie inside the guest bytes
      // this entry was hashed over.
      const auto Size = Reloc.PatchableData.ValueSize;
      const bool IsCRC = Reloc.Type == uint8_t(CPU::RelocationTypes::RELOC_GUEST_PATCHABLE_CRC_MOVE);
      const bool SizeOk = IsCRC ? (Size >= 1 && Size <= 15) : (Size == 1 || Size == 2 || Size == 4 || Size == 8);
      if (!SizeOk || Reloc.PatchableData.RegisterIndex >= 32 || uint64_t(Reloc.PatchableData.SiteOffset) + Size > Header.GuestSize) {
        return Reject(88);
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
      return Reject(99);
    }
    for (uint32_t i = 0; i < Header.ThunkRelocCount; ++i) {
      BlobThunkRelocation Reloc;
      memcpy(&Reloc, Cursor + i * sizeof(Reloc), sizeof(Reloc));
      if (Reloc.Offset % 4 != 0 || uint64_t(Reloc.Offset) + 16 > Header.HostSize || Reloc.RegisterIndex >= 32) {
        return Reject(105);
      }
    }
  }
  return true;
}

bool DiskCache::FlushToDiskMacOS(uint64_t QueuedBefore) {
  std::lock_guard lk {FlushMutex};
  // A full flush costs milliseconds; skip it when nothing was queued since the last one.
  if (QueuedBefore == FlushedQueuedCount) {
    return true;
  }
  if (!RWCacheDB->FlushToDisk()) {
    return false;
  }
  FlushedQueuedCount = QueuedBefore;
  return true;
}

// fork: the child has only the forking thread. ContextImpl::LockBeforeFork holds the code
// invalidation lock, so no thread is in Lookup or Store; these locks stop the writer
// thread from being inside the index at the moment of the fork.
void DiskCache::LockBeforeFork() {
  if (!ReadingDiskCache) {
    return;
  }
  IndexLock.lock();
  MemoryLRULock.lock();
  ForkLocksHeld = true;
}

void DiskCache::UnlockAfterFork(bool Child) {
  if (!ForkLocksHeld) {
    return;
  }
  ForkLocksHeld = false;
  MemoryLRULock.unlock();
  IndexLock.unlock();
  if (Child && Writer) {
    // The writer thread does not exist in the child, and the child shares the parent's
    // open files and so its file locks: two writers could interleave appends. The child
    // keeps reading the cache but stores nothing. The old queue object is abandoned (its
    // mutex may have been held by the parent's writer thread at the fork); a new one runs
    // the in-memory cache's housekeeping.
    (void)Writer.release();
    WritingDiskCache = false;
    ForkedChild = true;
    FEXCore::Threads::Flags WriterThreadFlags = {.LowPriority = true, .Internal = true};
    Writer = fextl::make_unique<WorkQueueThread>(WriterThreadFlags, "FEX:DiskCache");
  }
}

} // namespace FEXCore::DiskCache
