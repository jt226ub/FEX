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
#include <pthread.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <dirent.h>
#include <mutex>
#include <pthread.h>
#include <unistd.h>

namespace FEXCore::DiskCache {

namespace {
  // Bump when this port changes the cache's contents or checks.
  // 2: guest extents in the integrity hash; entry-point bounds; lazy index.
  // 3: package D's code generation: register-cache flush before memory instructions,
  //    interrupt checks on conditional back-edges, rep movs/stos with exact progress,
  //    undo records for restartable faults.
  // 4: package D's store-first read-modify-writes and rep loop tuning; package F's
  //    batched writer (merged from macos/v1) without its own bump.
  // 5: x87 stores (fst/fstp m32/m64/m80, fbstp) ordered under vector ordering (round 2).
  // 6: round 2 integration: rep stos{w,d,q} with string ordering gets the placeholder
  //    before its release store (the misaligned back-patch overwrote the loop's count
  //    test without it). New options (MEMCPYSETTSOWIDE, GUESTMEMORYALIASED, VECTORTSOSPLIT,
  //    TSOBARRIERMERGE) are in the configuration hash; TSODEFERRED is not, the effective
  //    ordering state it changes is (CodeGenStateFlags).
  constexpr uint32_t PortCacheRevision = 6;

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
  // user, private to it (0600 or tighter) and without an ACL that grants write. No name
  // at all (another process just deleted this generation) is a race, not a verdict.
  enum class FileVerdict { Trusted, Untrusted, Race };
  FileVerdict JudgeFile(int DirFD, int FD, std::string_view Name, ino_t& Ino, dev_t& Dev) {
    struct stat Info;
    if (fstat(FD, &Info) != 0) {
      LogMan::Msg::EFmt("DiskCache: {} cannot be examined; not using it", Name);
      return FileVerdict::Untrusted;
    }
    Ino = Info.st_ino;
    Dev = Info.st_dev;
    if (S_ISREG(Info.st_mode) && Info.st_nlink != 1) {
      // While another process deletes this name (an old generation) APFS can show a link
      // count other than 1, even 2, for a while before 0. If the name no longer leads to
      // this file it was being deleted: a race. A second name that stays is refused.
      for (int Tries = 0; Tries < 50; ++Tries) {
        struct stat Named;
        const fextl::string Leaf(Name);
        if (fstatat(DirFD, Leaf.c_str(), &Named, AT_SYMLINK_NOFOLLOW) != 0 || Named.st_ino != Info.st_ino || Named.st_dev != Info.st_dev ||
            (fstat(FD, &Info) == 0 && Info.st_nlink == 0)) {
          return FileVerdict::Race;
        }
        if (Info.st_nlink == 1) {
          break;
        }
        usleep(2000);
      }
    }
    const char* Why = nullptr;
    if (!S_ISREG(Info.st_mode)) {
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
      return FileVerdict::Untrusted;
    }
    return FileVerdict::Trusted;
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
  // How long a store, or opening the read-write database, waits for another process's
  // lock: once per process (see DiskCache::StoreMacOS).
  constexpr int STORE_DEADLINE_MS = 5000;

  // The cache the pthread_atfork handlers look after (one context at a time).
  std::atomic<DiskCache*> ForkWatched {};
  std::once_flag AtForkOnce;
  bool AtForkTookLocks = false;

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
  // writes: every process touches the bucket's marker when it opens it. On macOS pruning
  // goes through PruneBuckets below, relative to the verified base directory; this is
  // only the name rule upstream's path-based prune is filtered by.
  bool KeepBucket(std::string_view, std::string_view) {
    return true;
  }
} // namespace MacOS

namespace {
  bool LooksLikeBucketName(std::string_view Name) {
    return Name.size() == 16 && std::all_of(Name.begin(), Name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  }

  // Removes a bucket directory's files (it holds no subdirectories) and the directory,
  // all relative to descriptors, never following links.
  void RemoveBucketAt(int BaseFD, const char* Name) {
    const int DirFD = openat(BaseFD, Name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (DirFD < 0) {
      return;
    }
    if (DIR* Dir = fdopendir(dup(DirFD))) {
      while (const dirent* Entry = readdir(Dir)) {
        if (strcmp(Entry->d_name, ".") != 0 && strcmp(Entry->d_name, "..") != 0) {
          unlinkat(DirFD, Entry->d_name, 0);
        }
      }
      closedir(Dir);
    }
    close(DirFD);
    unlinkat(BaseFD, Name, AT_REMOVEDIR);
  }

  // A bucket was used within the week if its directory or its marker was written then.
  bool BucketRecentlyUsedAt(int BaseFD, const char* Name, time_t Now, time_t Keep) {
    struct stat Info;
    if (fstatat(BaseFD, Name, &Info, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(Info.st_mode)) {
      return true; // not ours to judge
    }
    if (Now - Info.st_mtime < Keep) {
      return true;
    }
    const int DirFD = openat(BaseFD, Name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (DirFD < 0) {
      return true;
    }
    const bool Recent = fstatat(DirFD, "last-used", &Info, AT_SYMLINK_NOFOLLOW) == 0 && Now - Info.st_mtime < Keep;
    close(DirFD);
    return Recent;
  }

  void PruneBuckets(int BaseFD, std::string_view Current) {
    FEX_CONFIG_OPT(DiskCachePruneStaleEntries, DISKCACHEPRUNESTALEENTRIES);
    if (!DiskCachePruneStaleEntries) {
      return;
    }
    DIR* Dir = fdopendir(dup(BaseFD));
    if (!Dir) {
      return;
    }
    rewinddir(Dir); // the duplicate shares BaseFD's read position
    fextl::vector<fextl::string> Stale;
    const time_t Now = time(nullptr);
    while (const dirent* Entry = readdir(Dir)) {
      const std::string_view Name = Entry->d_name;
      if (LooksLikeBucketName(Name) && Name != Current && !BucketRecentlyUsedAt(BaseFD, Entry->d_name, Now, KeepIdleBucketSeconds)) {
        Stale.emplace_back(Name);
      }
    }
    closedir(Dir);
    for (const auto& Name : Stale) {
      LogMan::Msg::IFmt("DiskCache: removing unused bucket {}", Name);
      RemoveBucketAt(BaseFD, Name.c_str());
    }
  }
} // namespace

bool FOZFile::NameMatches(int DirFD) const {
  struct stat Name, Open;
  return FD && fstatat(DirFD, FileName.c_str(), &Name, AT_SYMLINK_NOFOLLOW) == 0 && fstat(FD->GetHandle(), &Open) == 0 &&
         Name.st_ino == Open.st_ino && Name.st_dev == Open.st_dev && Open.st_nlink > 0;
}

bool FOZFile::OpenAt(int DirFD, const fextl::string& Leaf, bool ReadOnly, bool Create, bool& Refused, bool& Raced) {
  Refused = false;
  Raced = false;
  FileName = Leaf;
  this->ReadOnly = ReadOnly;
  FD.reset();

  // O_NOFOLLOW: a symbolic link in place of the file is refused (ELOOP), never followed.
  const int Flags = (ReadOnly ? O_RDONLY : (O_RDWR | (Create ? O_CREAT : 0))) | O_NOFOLLOW | O_CLOEXEC;
  const int Handle = OpenAtRetrying(DirFD, Leaf.c_str(), Flags);
  if (Handle < 0) {
    if (errno == ENOENT && !Create) {
      Raced = true; // the generation is gone
    } else if (errno == ELOOP) {
      LogMan::Msg::EFmt("DiskCache: {} is a symbolic link; not using it", Leaf);
      Refused = true;
    } else {
      LogMan::Msg::IFmt("DiskCache: cannot open {}: {}", Leaf, strerror(errno));
    }
    return false;
  }
  switch (JudgeFile(DirFD, Handle, Leaf, Ino, Dev)) {
  case FileVerdict::Trusted: break;
  case FileVerdict::Race:
    close(Handle);
    Raced = true;
    return false;
  case FileVerdict::Untrusted:
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
    if (!FD->LockUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(STORE_DEADLINE_MS))) {
      LogMan::Msg::IFmt("DiskCache: {} stayed locked by another process", Leaf);
      FD.reset();
      return false;
    }
    TookLock = true;
    Size = FD->Size();
  }
  auto CheckMagic = [&] {
    uint8_t magic[FOZ_REF_MAGIC_SIZE];
    if (FD->PRead(magic, FOZ_REF_MAGIC_SIZE, 0) == FOZ_REF_MAGIC_SIZE &&
        memcmp(magic, MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE - 1) == 0) {
      int version = magic[FOZ_REF_MAGIC_SIZE - 1];
      return version <= MesaFOZ::FOSSILIZE_FORMAT_VERSION && version >= MesaFOZ::FOSSILIZE_FORMAT_MIN_COMPAT_VERSION;
    }
    return false;
  };
  if (Size == 0 && !ReadOnly) {
    Valid = FD->PWrite(MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE, 0) == FOZ_REF_MAGIC_SIZE;
  } else {
    Valid = CheckMagic();
    if (!Valid && !ReadOnly && !TookLock) {
      // Another process may be writing the magic of a file it just created (the size can
      // show 16 before the bytes are there). It writes it under the lock, so look again
      // holding it before calling the file damaged.
      if (FD->LockUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(STORE_DEADLINE_MS))) {
        TookLock = true;
        Valid = CheckMagic() || (FD->Size() == 0 && FD->PWrite(MesaFOZ::stream_reference_magic_and_version, FOZ_REF_MAGIC_SIZE, 0) ==
                                                         FOZ_REF_MAGIC_SIZE);
      } else {
        LogMan::Msg::IFmt("DiskCache: {} stayed locked by another process", Leaf);
        FD.reset();
        return false;
      }
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

IndexedDB::OpenResult IndexedDB::OpenAt(int DirFD, const fextl::string& Leaf, bool ReadOnly, bool Create, bool& Replaced) {
  this->DirFD = DirFD;
  Name = Leaf;
  this->ReadOnly = ReadOnly;
  Replaced = false;
  const auto CacheName = Leaf + ".foz";
  const auto IndexName = Leaf + "_idx.foz";
  for (int Attempt = 0; Attempt < 2; ++Attempt) {
    bool CacheRefused = false, IndexRefused = false, CacheRaced = false, IndexRaced = false;
    if (CacheFOZ.OpenAt(DirFD, CacheName, ReadOnly, Create, CacheRefused, CacheRaced) &&
        IndexFOZ.OpenAt(DirFD, IndexName, ReadOnly, Create, IndexRefused, IndexRaced)) {
      // Both names must still be the files opened: another process deleting this
      // generation meanwhile could leave an old cache file paired with a new index.
      if (NamesMatch()) {
        return OpenResult::Opened;
      }
      CacheRaced = true;
    }
    const ino_t CacheIno = CacheFOZ.Ino, IndexIno = IndexFOZ.Ino;
    CacheFOZ = FOZFile {};
    IndexFOZ = FOZFile {};
    if (CacheRaced || IndexRaced) {
      return OpenResult::Raced;
    }
    if (!(CacheRefused || IndexRefused)) {
      return OpenResult::Failed;
    }
    if (ReadOnly || Attempt != 0) {
      return OpenResult::Untrusted;
    }
    // The directory is verified private, so removing names from it is safe. Both files
    // go: one half of a database is no use without the other. A name is removed only if
    // it is still the file that was judged, never one another process created since.
    LogMan::Msg::EFmt("DiskCache: removing {}, which may not be trusted, and starting it again", Leaf);
    auto RemoveIfSame = [&](const fextl::string& FileName, ino_t Judged) {
      struct stat Info;
      // The file that was judged, or a symbolic link (refused without being opened).
      if (fstatat(DirFD, FileName.c_str(), &Info, AT_SYMLINK_NOFOLLOW) == 0 && ((Judged && Info.st_ino == Judged) || S_ISLNK(Info.st_mode))) {
        unlinkat(DirFD, FileName.c_str(), 0);
      }
    };
    RemoveIfSame(CacheName, CacheIno);
    RemoveIfSame(IndexName, IndexIno);
    Replaced = true;
  }
  return OpenResult::Untrusted;
}

bool IndexedDB::LoadIndex(uint64_t MaxSize, bool& FoundMetadata) {
  const ssize_t IndexSize = IndexFOZ.Size();
  const ssize_t CacheSize = CacheFOZ.Size();
  Limit = MaxSize;
  // Record offsets are kept in 32 bits.
  if (IndexSize < FOZ_REF_MAGIC_SIZE || CacheSize < FOZ_REF_MAGIC_SIZE || uint64_t(IndexSize) > ResolvedSlot) {
    LogMan::Msg::EFmt("DiskCache: {} cannot be read", Name);
    return false;
  }
  // Filled past this process's limit by a process with a larger one: use it, but do not
  // add to it (and never empty it because of the smaller limit here).
  WriteRefused = uint64_t(CacheSize) > MaxSize || uint64_t(IndexSize) > MaxSize;
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
    return true;
  }
  // A later record for the same guest code replaces an earlier one: that is how an entry
  // that failed its checks (and was compiled and stored again) is superseded. Records are
  // parsed oldest first. Assigned in place, so iterators other lookups hold stay valid.
  auto& Head = It->second;
  if (XXH128_isEqual(Head.MainEntry.GuestHash, NewEntry.GuestHash)) {
    Head.MainEntry = std::move(NewEntry);
    return true;
  }
  if (!Head.MoreEntries) {
    Head.MoreEntries = fextl::make_unique<fextl::multimap<uint64_t, IndexEntry>>();
  }
  for (auto& [Footprint, Existing] : *Head.MoreEntries) {
    if (XXH128_isEqual(Existing.GuestHash, NewEntry.GuestHash)) {
      Existing = std::move(NewEntry);
      return true;
    }
  }
  Head.MoreEntries->insert({Extra.GuestFootprint, std::move(NewEntry)});
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

bool IndexedDB::LockForOpen(std::chrono::steady_clock::time_point Deadline) {
  if (!CacheFOZ.LockUntil(Deadline)) {
    return false;
  }
  if (!IndexFOZ.LockUntil(Deadline)) {
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
  // Only names that are still these files (the caller holds their locks and checked).
  if (CacheFOZ.NameMatches(DirFD)) {
    unlinkat(DirFD, (Name + ".foz").c_str(), 0);
  }
  if (IndexFOZ.NameMatches(DirFD)) {
    unlinkat(DirFD, (Name + "_idx.foz").c_str(), 0);
  }
  CacheFOZ = FOZFile {};
  IndexFOZ = FOZFile {};
  bool Refused = false, Raced = false;
  if (!CacheFOZ.OpenAt(DirFD, Name + ".foz", false, true, Refused, Raced) ||
      !IndexFOZ.OpenAt(DirFD, Name + "_idx.foz", false, true, Refused, Raced)) {
    return false;
  }
  CacheFOZSizeAtOpen = FOZ_REF_MAGIC_SIZE;
  IndexKnownEnd = FOZ_REF_MAGIC_SIZE;
  return true;
}

// With both locks held. Removes the names of a database with no records (only the magic).
void IndexedDB::RemoveIfEmpty() {
  if (ReadOnly || DirFD < 0 || CacheFOZ.Size() > FOZ_REF_MAGIC_SIZE || IndexFOZ.Size() > FOZ_REF_MAGIC_SIZE || !NamesMatch()) {
    return;
  }
  unlinkat(DirFD, (Name + ".foz").c_str(), 0);
  unlinkat(DirFD, (Name + "_idx.foz").c_str(), 0);
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

IndexedDB::StoreResult IndexedDB::StoreCacheBlobMacOS(const MesaFOZ::foz_payload_key& UniqueKey, uint64_t LookupKey,
                                                      std::span<const uint8_t> Blob, MesaFOZ::mesa_index_db_file_entry& IndexEntry,
                                                      std::span<const uint8_t> IndexBlob, std::chrono::steady_clock::time_point Deadline) {
  if (ReadOnly || WriteRefused) {
    return StoreResult::Failed;
  }
  if (!CacheFOZ.LockUntil(Deadline)) {
    return StoreResult::Busy;
  }
  if (!IndexFOZ.LockUntil(Deadline)) {
    CacheFOZ.Unlock();
    return StoreResult::Busy;
  }

  auto Done = [&](StoreResult Result) {
    CacheFOZ.Unlock();
    IndexFOZ.Unlock();
    return Result;
  };
  // The real sizes of both files, under the locks, so other processes' appends count.
  if (!WithinSizeLimit(Blob.size(),
                       sizeof(MesaFOZ::foz_payload_key) + sizeof(MesaFOZ::foz_payload_header) + sizeof(IndexEntry) + IndexBlob.size())) {
    MaxSizeReached = true;
    return Done(StoreResult::Full);
  }
  if (!RepairIndexTail()) {
    return Done(StoreResult::Failed);
  }
  std::span<const uint8_t> BlobChunks[] = {Blob};
  uint64_t BlobOffset = 0;
  if (!CacheFOZ.WriteBlob(UniqueKey, BlobChunks, BlobOffset)) {
    return Done(StoreResult::Failed);
  }
  IndexEntry = {.hash = LookupKey, .size = (uint32_t)Blob.size(), .last_access_time = 0, .cache_db_file_offset = BlobOffset};
  std::span<const uint8_t> IndexBlobChunks[] = {
    {(const uint8_t*)&IndexEntry, sizeof(IndexEntry)},
    IndexBlob,
  };
  uint64_t UnusedIndexBlobOffset = 0;
  if (!IndexFOZ.WriteBlob(UniqueKey, IndexBlobChunks, UnusedIndexBlobOffset)) {
    return Done(StoreResult::Failed);
  }
  IndexKnownEnd = IndexFOZ.Size();
  return Done(StoreResult::Stored);
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
  const uint64_t Max = std::min<uint64_t>(Limit ? Limit : uint64_t(MaxFileSize), ResolvedSlot);
  return uint64_t(CacheSize) + FOZRecordHeaderSize + BlobSize <= Max && uint64_t(IndexSize) + IndexRecordSize <= Max;
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
  // Oldest generation first, so a newer record for the same code replaces an older one.
  for (auto& DB : Generations) {
    DB->ResolveKey(LookupKey, Index);
  }
  for (auto& DB : ROCacheDBs) {
    DB->ResolveKey(LookupKey, Index);
  }
}

void DiskCache::MarkRejectedLocked(uint64_t LookupKey, const IndexEntry& Entry) {
  auto It = Index.find(LookupKey);
  if (It == Index.end()) {
    return;
  }
  auto Matches = [&](const IndexEntry& Candidate) {
    return Candidate.DB == Entry.DB && Candidate.Offset == Entry.Offset && XXH128_isEqual(Candidate.GuestHash, Entry.GuestHash);
  };
  if (Matches(It->second.MainEntry)) {
    It->second.MainEntry.Rejected = true;
  }
  if (It->second.MoreEntries) {
    for (auto& [Footprint, Candidate] : *It->second.MoreEntries) {
      if (Matches(Candidate)) {
        Candidate.Rejected = true;
      }
    }
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

// Read-only databases named by DiskCacheRODBNames (the read-write one is opened by
// OpenGeneration). They are used as they are: no lock, no repair, no reset.
bool DiskCache::OpenCacheDBMacOS(const fextl::string& Leaf, bool ReadOnly) {
  if (!ReadOnly || BucketDirFD < 0 || Leaf.empty() || Leaf.find('/') != fextl::string::npos) {
    return false;
  }
  auto CurDB = fextl::make_unique<IndexedDB>();
  bool Replaced = false;
  if (CurDB->OpenAt(BucketDirFD, Leaf, true, false, Replaced) != IndexedDB::OpenResult::Opened) {
    LogMan::Msg::IFmt("DiskCache: could not open {}; translating without it", Leaf);
    return false;
  }
  FEX_CONFIG_OPT(MaxSize, DISKCACHEMAXFILESIZE);
  bool LocalFoundMetadata = false;
  if (!CurDB->LoadIndex(MaxSize, LocalFoundMetadata) || !CheckMetadata(*CurDB)) {
    RejectedCount.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  ROCacheDBs.push_back(std::move(CurDB));
  return true;
}

fextl::string DiskCache::GenerationName(uint32_t Generation) const {
  return Generation == 0 ? GenerationBase : fextl::fmt::format("{}-{}", GenerationBase, Generation);
}

// Opens one generation of the read-write database (creating it when Write is set), checks
// it and gives it its description. Returns null, with CurrentState set, if it cannot be
// used.
fextl::unique_ptr<IndexedDB> DiskCache::OpenGeneration(uint32_t Generation, bool Create, bool& Raced, State* FailState) {
  Raced = false;
  auto DB = fextl::make_unique<IndexedDB>();
  DB->Generation = Generation;
  const auto Name = GenerationName(Generation);
  bool Replaced = false;
  auto Fail = [&](State Reason) -> fextl::unique_ptr<IndexedDB> {
    if (FailState) {
      *FailState = Reason;
    }
    return nullptr;
  };
  // The read-write files are opened writable even for an older generation: other
  // processes may still be appending to it, and its index is repaired under the locks.
  const auto Result = DB->OpenAt(BucketDirFD, Name, false, Create, Replaced);
  if (Replaced) {
    UntrustedReplaced.fetch_add(1, std::memory_order_relaxed);
  }
  if (Result == IndexedDB::OpenResult::Raced) {
    Raced = true;
    return nullptr;
  }
  if (Result != IndexedDB::OpenResult::Opened) {
    LogMan::Msg::IFmt("DiskCache: could not open {}; translating without it", Name);
    return Fail(Result == IndexedDB::OpenResult::Untrusted ? State::Untrusted : State::Unavailable);
  }
  if (!DB->LockForOpen(std::chrono::steady_clock::now() + std::chrono::milliseconds(STORE_DEADLINE_MS))) {
    LogMan::Msg::IFmt("DiskCache: {} is busy; translating without it", Name);
    return Fail(State::Unavailable);
  }
  // Deleted (as an old generation) by another process between the open and the lock.
  if (!DB->NamesMatch()) {
    DB->UnlockAfterOpen();
    Raced = true;
    return nullptr;
  }
  if (Create) {
    // Chosen from a listing that is out of date: the generation is already stale (this
    // open may just have recreated a name another process removed). Look again; remove it
    // if it holds nothing.
    const auto Found = ListGenerations();
    if (Generation + 2 <= NewestGeneration(Found)) {
      DB->RemoveIfEmpty();
      DB->UnlockAfterOpen();
      Raced = true;
      return nullptr;
    }
  }
  FEX_CONFIG_OPT(MaxSize, DISKCACHEMAXFILESIZE);
  bool LocalFoundMetadata = false;
  bool Ok = DB->LoadIndex(MaxSize, LocalFoundMetadata);
  if (Ok && !CheckMetadata(*DB)) {
    Ok = false;
  }
  if (!Ok) {
    // An old cache file paired with a new index (another process was deleting this
    // generation's two names while it was opened) looks like this too. A race goes away;
    // damage stays: only a pair that fails twice with the same files is damaged.
    const std::pair<ino_t, ino_t> Pair {DB->CacheIno(), DB->IndexIno()};
    if (SuspectPair != Pair) {
      SuspectPair = Pair;
      DB->UnlockAfterOpen();
      usleep(20000);
      Raced = true;
      return nullptr;
    }
    SuspectPair = {};
    RejectedCount.fetch_add(1, std::memory_order_relaxed);
    // Damaged or made for something else under this name: the directory is disposable,
    // so start this database again (new files, so a process still appending to the old
    // ones never sees what it wrote move). Under the locks, with both names checked, and
    // with the description written before the locks are let go, a well-behaved process
    // can never produce this.
    if (!DB->Reset()) {
      DB->UnlockAfterOpen();
      return Fail(State::Unavailable);
    }
    DamagedReset.fetch_add(1, std::memory_order_relaxed);
    LogMan::Msg::IFmt("DiskCache: emptied {} and starting it again", Name);
    LocalFoundMetadata = false;
    DB->Limit = MaxSize;
    DB->WriteRefused = false;
  }
  if (!LocalFoundMetadata && !DB->WriteRefused) {
    // A fresh database: its first record is the description, written before the locks
    // are released (StoreCacheBlobMacOS takes the locks it already holds and releases
    // them), so no process can see entries without it.
    MesaFOZ::foz_payload_key MetadataKey;
    memset(MetadataKey.bytes, 0xFF, sizeof(MetadataKey));
    IndexExtraBlobHeader MetaDataHeader = {};
    MesaFOZ::mesa_index_db_file_entry IndexEntry = {};
    DB->StoreCacheBlobMacOS(MetadataKey, ~0, {ExpectedMetadata.data(), ExpectedMetadata.size()}, IndexEntry,
                            {reinterpret_cast<uint8_t*>(&MetaDataHeader), sizeof(MetaDataHeader)},
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(STORE_DEADLINE_MS));
  } else {
    DB->UnlockAfterOpen();
  }
  return DB;
}

// Generation numbers in the bucket. A number is believed only if it is the lowest found
// or its predecessor is found too (a generation is started only from the one before it,
// which is kept until the one after exists), and at most 2^24: a planted name far ahead
// is ignored, not acted on.
fextl::vector<uint32_t> DiskCache::ListGenerations() {
  fextl::vector<uint32_t> Found;
  if (DIR* Dir = fdopendir(dup(BucketDirFD))) {
    // The duplicate shares the directory's read position with BucketDirFD and every earlier
    // listing: start from the beginning, or a second listing in this process finds nothing
    // (and a process would start again from generation 0 next to the live ones).
    rewinddir(Dir);
    while (const dirent* Entry = readdir(Dir)) {
      std::string_view Name = Entry->d_name;
      if (!Name.starts_with(GenerationBase) || !Name.ends_with("_idx.foz")) {
        continue;
      }
      Name.remove_prefix(GenerationBase.size());
      Name.remove_suffix(strlen("_idx.foz"));
      uint32_t Generation = 0;
      bool Valid = true;
      if (!Name.empty()) {
        Valid = Name[0] == '-' && Name.size() >= 2 && Name.size() <= 9 && Name[1] != '0';
        for (char c : Name.substr(1)) {
          Valid &= c >= '0' && c <= '9';
          Generation = Generation * 10 + uint32_t(c - '0');
        }
      }
      if (Valid && Generation <= (1U << 24)) {
        Found.push_back(Generation);
      }
    }
    closedir(Dir);
  }
  std::sort(Found.begin(), Found.end());
  Found.erase(std::unique(Found.begin(), Found.end()), Found.end());
  return Found;
}

uint32_t DiskCache::NewestGeneration(const fextl::vector<uint32_t>& Found) {
  uint32_t Newest = Found.empty() ? 0 : Found.front();
  for (uint32_t Generation : Found) {
    if (Generation == Found.front() || std::binary_search(Found.begin(), Found.end(), Generation - 1)) {
      Newest = std::max(Newest, Generation);
    }
  }
  return Newest;
}

// Which generations may exist. N is the newest believed generation (NewestGeneration).
// Processes write N and read N - 1; anything older is stale. Stale generations come from
// a process that started from an old listing (it recreated a name just deleted), lost a
// race, or was killed. Every open (InitMacOS) and every rotation removes stale generations
// it finds, up to MaxStaleRemovals per call, so with no process opening or rotating the
// bucket holds N - 1 and N (or only N) after any open, whatever the gaps. A stale
// generation is removed only while this process holds both of its locks, so never in the
// middle of another process's write; a process whose generation has gone notices before
// its next write (StoreBatchMacOS) and moves to N. Names that are not believed (ahead of
// N with a gap before them) are ignored, not acted on.
void DiskCache::RemoveStaleGenerations(const fextl::vector<uint32_t>& Found, uint32_t Newest) {
  size_t Budget = MaxStaleRemovals;
  for (uint32_t Old : Found) {
    if (Old + 2 > Newest || Budget == 0) {
      break; // sorted
    }
    --Budget;
    const auto Name = GenerationName(Old);
    const fextl::string Leaves[2] = {Name + ".foz", Name + "_idx.foz"};
    int FDs[2] = {-1, -1};
    bool Busy = false;
    for (int k = 0; k < 2; ++k) {
      FDs[k] = openat(BucketDirFD, Leaves[k].c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (FDs[k] >= 0 && flock(FDs[k], LOCK_EX | LOCK_NB) != 0) {
        Busy = true; // another process (or this one's writer) is using it now; a later open removes it
      }
    }
    if (!Busy) {
      for (const auto& Leaf : Leaves) {
        unlinkat(BucketDirFD, Leaf.c_str(), 0);
      }
    }
    for (int FD : FDs) {
      if (FD >= 0) {
        close(FD);
      }
    }
  }
}

// The current generation is full (or was removed as stale by another process): continue in
// the newest one, starting the next generation if this one was the newest. Safe across
// processes: a process that finds the next generation already started opens it; one still
// writing an older generation fills it, or finds it gone, and moves on too.
bool DiskCache::Rotate(IndexedDB* Full, bool Gone) {
  if (WriteDB.load() != Full || WritesStopped.load()) {
    return WriteDB.load() != Full;
  }
  // Opened outside the index lock (it can wait for another process's lock), then swapped
  // in. Only the writer thread rotates.
  fextl::unique_ptr<IndexedDB> NewDB;
  for (int Attempt = 0; Attempt < 8 && !NewDB; ++Attempt) {
    const uint32_t Newest = NewestGeneration(ListGenerations());
    const uint32_t Next = Gone ? std::max(Newest, Full->Generation) : std::max(Full->Generation + 1, Newest);
    bool Raced = false;
    NewDB = OpenGeneration(Next, true, Raced, nullptr);
    if (!NewDB && !Raced) {
      break;
    }
  }
  if (!NewDB) {
    return false;
  }
  auto* Raw = NewDB.get();
  {
    std::lock_guard IndexGuard(IndexLock);
    Generations.push_back(std::move(NewDB));
    if (Raw->WriteRefused) {
      // Another process with a larger limit filled it: read it, write nothing more.
      WritesStopped = true;
      CurrentState = State::ReadOnly;
      LogMan::Msg::IFmt("DiskCache: {} is full and {} is over this process's limit; saving stops", Full->Name, Raw->Name);
      return false;
    }
    LogMan::Msg::IFmt("DiskCache: {} is {}; continuing in {}", Full->Name, Gone ? "gone" : "full", Raw->Name);
    WriteDB = Raw;
  }
  const auto Found = ListGenerations();
  RemoveStaleGenerations(Found, NewestGeneration(Found));
  return true;
}

bool FOZFile::PWriteAll(const uint8_t* Data, size_t Size, uint64_t Offset) {
  while (Size) {
    const ssize_t Wrote = FD->PWrite(Data, Size, Offset);
    if (Wrote <= 0) {
      if (Wrote < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    Data += Wrote;
    Size -= Wrote;
    Offset += Wrote;
  }
  return true;
}

IndexedDB::StoreResult IndexedDB::StoreBatchMacOS(std::span<const PendingStore> Entries, std::span<uint64_t> Offsets,
                                                  std::chrono::steady_clock::time_point Deadline, size_t& Count) {
  Count = 0;
  if (ReadOnly || WriteRefused) {
    return StoreResult::Failed;
  }
  if (!CacheFOZ.LockUntil(Deadline)) {
    return StoreResult::Busy;
  }
  if (!IndexFOZ.LockUntil(Deadline)) {
    CacheFOZ.Unlock();
    return StoreResult::Busy;
  }
  auto Done = [&](StoreResult Result) {
    CacheFOZ.Unlock();
    IndexFOZ.Unlock();
    return Result;
  };
  // Removed as stale by another process (RemoveStaleGenerations holds these locks while it
  // removes the names): write to the newest generation instead.
  if (!NamesMatch()) {
    return Done(StoreResult::Gone);
  }
  // Under the locks, so other processes' appends count.
  if (!RepairIndexTail()) {
    return Done(StoreResult::Failed);
  }
  const ssize_t CacheSize = CacheFOZ.Size();
  const uint64_t IndexSize = IndexKnownEnd;
  if (CacheSize < FOZ_REF_MAGIC_SIZE) {
    return Done(StoreResult::Failed);
  }
  const uint64_t Max = std::min<uint64_t>(Limit ? Limit : uint64_t(MaxFileSize), ResolvedSlot);
  CacheBatchBuffer.clear();
  IndexBatchBuffer.clear();
  auto Append = [](fextl::vector<uint8_t>& Buffer, const void* Data, size_t Size) {
    Buffer.insert(Buffer.end(), static_cast<const uint8_t*>(Data), static_cast<const uint8_t*>(Data) + Size);
  };
  for (const auto& Entry : Entries) {
    const uint64_t IndexRecord = FOZRecordHeaderSize + sizeof(MesaFOZ::mesa_index_db_file_entry) + Entry.IndexBlob.size();
    if (uint64_t(CacheSize) + CacheBatchBuffer.size() + FOZRecordHeaderSize + Entry.Blob.size() > Max ||
        IndexSize + IndexBatchBuffer.size() + IndexRecord > Max) {
      break;
    }
    const MesaFOZ::foz_payload_header BlobHeader {.payload_size = (uint32_t)Entry.Blob.size(),
                                                  .format = MesaFOZ::FOSSILIZE_COMPRESSION_NONE,
                                                  .crc = 0,
                                                  .uncompressed_size = (uint32_t)Entry.Blob.size()};
    Append(CacheBatchBuffer, Entry.Key.bytes, sizeof(Entry.Key.bytes));
    Append(CacheBatchBuffer, &BlobHeader, sizeof(BlobHeader));
    Offsets[Count] = uint64_t(CacheSize) + CacheBatchBuffer.size();
    Append(CacheBatchBuffer, Entry.Blob.data(), Entry.Blob.size());

    const MesaFOZ::mesa_index_db_file_entry FileEntry {
      .hash = Entry.LookupKey, .size = (uint32_t)Entry.Blob.size(), .last_access_time = 0, .cache_db_file_offset = Offsets[Count]};
    const uint32_t IndexPayload = sizeof(FileEntry) + Entry.IndexBlob.size();
    const MesaFOZ::foz_payload_header IndexHeader {
      .payload_size = IndexPayload, .format = MesaFOZ::FOSSILIZE_COMPRESSION_NONE, .crc = 0, .uncompressed_size = IndexPayload};
    Append(IndexBatchBuffer, Entry.Key.bytes, sizeof(Entry.Key.bytes));
    Append(IndexBatchBuffer, &IndexHeader, sizeof(IndexHeader));
    Append(IndexBatchBuffer, &FileEntry, sizeof(FileEntry));
    Append(IndexBatchBuffer, Entry.IndexBlob.data(), Entry.IndexBlob.size());
    ++Count;
  }
  if (Count == 0) {
    MaxSizeReached = true;
    return Done(StoreResult::Full);
  }
  // The cache records first: an index record is written only after what it points to. A
  // torn index tail is repaired by the next writer (RepairIndexTail).
  if (!CacheFOZ.PWriteAll(CacheBatchBuffer.data(), CacheBatchBuffer.size(), CacheSize) ||
      !IndexFOZ.PWriteAll(IndexBatchBuffer.data(), IndexBatchBuffer.size(), IndexSize)) {
    Count = 0;
    return Done(StoreResult::Failed);
  }
  IndexKnownEnd = IndexSize + IndexBatchBuffer.size();
  if (CacheBatchBuffer.capacity() > (8U << 20)) { // twice the batch limit: a rotation-sized batch
    CacheBatchBuffer = {};
    IndexBatchBuffer = {};
  }
  if (Count < Entries.size()) {
    MaxSizeReached = true;
    return Done(StoreResult::Full);
  }
  return Done(StoreResult::Stored);
}

// Writer thread only.
void DiskCache::AddPendingStoreMacOS(IndexedDB::PendingStore&& Entry) {
  PendingStoreBytes += Entry.Blob.size() + Entry.IndexBlob.size();
  PendingStores.push_back(std::move(Entry));
  const bool LastQueued = StoresInQueue.fetch_sub(1, std::memory_order_acq_rel) == 1;
  if (LastQueued || PendingStores.size() >= MaxBatchEntries || PendingStoreBytes >= MaxBatchBytes) {
    WriteBatchMacOS();
  }
}

// Writer thread only.
void DiskCache::SetWriterPriorityMacOS() {
  const bool Boost = FlushWaiters.load(std::memory_order_acquire) != 0;
  if (Boost != WriterBoosted) {
    WriterBoosted = Boost;
    pthread_set_qos_class_self_np(Boost ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY, 0);
  }
}

// Writes the pending stores into the current generation, rotating as it fills. One lock
// wait per process at most: after a timeout (another process holds the database's lock,
// perhaps stopped) every later store is dropped and counted, so a flush before exit never
// waits more than one deadline.
void DiskCache::WriteBatchMacOS() {
  if (PendingStores.empty()) {
    return;
  }
  SetWriterPriorityMacOS();
  fextl::vector<IndexedDB::PendingStore> Batch;
  Batch.swap(PendingStores);
  PendingStoreBytes = 0;
  fextl::vector<IndexedDB*> DBs(Batch.size(), nullptr);
  fextl::vector<uint64_t> Offsets(Batch.size(), 0);
  size_t Done = 0;
  int FullWithNothingWritten = 0;
  // Each turn writes, drops an entry or rotates; rotations are bounded too.
  for (int Rotations = 0; Done < Batch.size() && Rotations <= 16;) {
    IndexedDB* DB = WriteDB.load();
    if (!DB || StoresBusy.load(std::memory_order_relaxed) || WritesStopped.load(std::memory_order_relaxed) || InForkedChild()) {
      break;
    }
    size_t Count = 0;
    const auto Result = DB->StoreBatchMacOS(std::span(Batch).subspan(Done), std::span(Offsets).subspan(Done),
                                            std::chrono::steady_clock::now() + std::chrono::milliseconds(STORE_DEADLINE_MS), Count);
    std::fill(DBs.begin() + Done, DBs.begin() + Done + Count, DB);
    Done += Count;
    if (Result == IndexedDB::StoreResult::Stored) {
      break;
    }
    if (Result == IndexedDB::StoreResult::Busy) {
      if (!StoresBusy.exchange(true)) {
        LogMan::Msg::EFmt("DiskCache: {} stayed locked by another process; translations of this run are not saved", DB->Name);
      }
      break;
    }
    if (Result == IndexedDB::StoreResult::Gone) {
      if (!Rotate(DB, true)) {
        break;
      }
      ++Rotations;
      continue;
    }
    if (Result != IndexedDB::StoreResult::Full) {
      break;
    }
    FullWithNothingWritten = Count ? 0 : FullWithNothingWritten + 1;
    if (FullWithNothingWritten == 2) {
      // Too large even for a new generation: drop this entry, not the batch.
      ++Done;
      FullWithNothingWritten = 0;
      continue;
    }
    if (!Rotate(DB, false)) {
      break;
    }
    ++Rotations;
  }
  DroppedCount.fetch_add(std::count(DBs.begin(), DBs.end(), nullptr), std::memory_order_relaxed);
  FinishBatchMacOS(Batch, DBs, Offsets);
}

// What the store work item does after writing (upstream's CacheStoreWorkItem::Run), once
// per batch: the entries move from memory-only to their place in the file, and stay in the
// in-memory LRU while it has room.
void DiskCache::FinishBatchMacOS(std::span<IndexedDB::PendingStore> Entries, std::span<IndexedDB*> DBs, std::span<const uint64_t> Offsets) {
  fextl::vector<fextl::list<MemoryLRUKey>::iterator> LRUEntries(Entries.size());
  fextl::vector<bool> Keep(Entries.size(), false);
  {
    std::lock_guard Guard(MemoryLRULock);
    for (size_t i = 0; i < Entries.size(); ++i) {
      if (Entries[i].Blob.size() > MemoryLRUMaxSize) {
        continue;
      }
      Keep[i] = true;
      MemoryLRUCurrentSize += Entries[i].Blob.size();
      const auto* Extra = reinterpret_cast<const IndexExtraBlobHeader*>(Entries[i].IndexBlob.data());
      MemoryLRU.push_front({Entries[i].LookupKey, Extra->GuestHash, Extra->GuestFootprint, (uint32_t)Entries[i].Blob.size()});
      LRUEntries[i] = MemoryLRU.begin();
    }
  }
  {
    std::lock_guard Guard(IndexLock);
    for (size_t i = 0; i < Entries.size(); ++i) {
      const auto* Extra = reinterpret_cast<const IndexExtraBlobHeader*>(Entries[i].IndexBlob.data());
      auto* Entry = LookupLocked(Entries[i].LookupKey, Extra->GuestHash, Extra->GuestFootprint);
      LOGMAN_THROW_A_FMT(Entry != nullptr, "Stored Index entry not found?");
      if (!Entry) {
        continue;
      }
      if (DBs[i]) {
        Entry->DB = DBs[i];
        Entry->Offset = Offsets[i];
      }
      if (!Keep[i]) {
        Entry->MemoryBlob.reset();
      } else {
        Entry->LRUEntry = LRUEntries[i];
      }
    }
  }
  if (MemoryLRUCurrentSize > MemoryLRUMaxSize + MemoryLRUEvictThreshold) {
    Writer->QueueWork(fextl::make_unique<PruneMemoryLRUWorkItem>(this));
  }
}

void DiskCache::InitMacOS(const fextl::vector<uint8_t>& BucketBytes, size_t MachineDataSize, uint64_t ProcessBucketHash) {
  // Upstream relies on FormatVersion being bumped by hand whenever code generation
  // changes. The machine bucket here also holds the port identity: the exact build of the
  // core (Mach-O UUID), the CPU model (it reaches the guest through CPUID) and this port's
  // cache revision.
  CurrentState = State::Unavailable;
  OwnerPid = getpid();
  std::call_once(AtForkOnce, [] { pthread_atfork(&DiskCache::AtForkPrepare, &DiskCache::AtForkParent, &DiskCache::AtForkChild); });
  ForkWatched = this;
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
    CurrentState = errno == ELOOP || errno == ENOTDIR ? State::Untrusted : State::Unavailable;
    return;
  }
  if (!DirectoryIsTrusted(BaseDirFD, BasePath, false)) {
    close(BaseDirFD);
    BaseDirFD = -1;
    CurrentState = State::Untrusted;
    return;
  }

  const auto BucketName = fextl::fmt::format("{:016x}", MachineBucketHash);
  PruneBuckets(BaseDirFD, BucketName);

  mkdirat(BaseDirFD, BucketName.c_str(), S_IRWXU);
  BucketDirFD = openat(BaseDirFD, BucketName.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  const auto BucketPath = fextl::fmt::format("{}/{}", BasePath, BucketName);
  if (BucketDirFD < 0 || !DirectoryIsTrusted(BucketDirFD, BucketPath, true)) {
    if (BucketDirFD < 0) {
      LogMan::Msg::EFmt("DiskCache: cannot open {} (it must be a directory, not a link); disk cache disabled", BucketPath);
      CurrentState = errno == ELOOP || errno == ENOTDIR ? State::Untrusted : State::Unavailable;
    } else {
      close(BucketDirFD);
      BucketDirFD = -1;
      CurrentState = State::Untrusted;
    }
    return;
  }

  // Record that this bucket is in use, for pruning: one small write per process start.
  if (const int Marker = OpenAtRetrying(BucketDirFD, LastUsedMarker, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC); Marker >= 0) {
    futimens(Marker, nullptr);
    close(Marker);
  }

  FileMapper = nullptr; // files are read with pread on macOS

  // The read-write database's generations: write the newest, also read the one before,
  // delete older ones.
  GenerationBase = fextl::fmt::format("RWCacheDB_{:016x}", ProcessBucketHash);
  // Open the newest generation for writing and the one before for reading. A generation
  // that disappears while it is being opened (another process rotated twice and deleted
  // it) means the listing is stale: look again.
  CurrentState = State::Unavailable;
  for (int Attempt = 0; Attempt < 8; ++Attempt) {
    const auto Found = ListGenerations();
    const uint32_t Newest = NewestGeneration(Found);
    RemoveStaleGenerations(Found, Newest);
    Generations.clear();
    bool Raced = false;
    fextl::unique_ptr<IndexedDB> Previous;
    if (Newest >= 1 && std::binary_search(Found.begin(), Found.end(), Newest - 1)) {
      Previous = OpenGeneration(Newest - 1, false, Raced, nullptr);
      if (Raced) {
        continue;
      }
    }
    State FailState = State::Unavailable;
    auto Current = OpenGeneration(Newest, true, Raced, &FailState);
    if (Raced) {
      continue;
    }
    if (Previous) {
      Generations.push_back(std::move(Previous));
    }
    if (Current) {
      WriteDB = Current.get();
      CurrentState = Current->WriteRefused ? State::ReadOnly : State::Active;
      Generations.push_back(std::move(Current));
    } else {
      CurrentState = FailState;
    }
    break;
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

  WritingDiskCache = CurrentState == State::Active;
  ReadingDiskCache = !ROCacheDBs.empty() || !Generations.empty();
  if (!WritingDiskCache && ReadingDiskCache && CurrentState != State::Untrusted && CurrentState != State::Unavailable) {
    CurrentState = State::ReadOnly;
  }
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
  // No larger block can be stored in a code buffer (or produced by the translator).
  if (Header.HostSize > CPU::MaxBlockHostSize) {
    return Reject(11);
  }
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
  auto* DB = WriteDB.load();
  if (!DB || !DB->FlushToDisk()) {
    return false;
  }
  FlushedQueuedCount = QueuedBefore;
  return true;
}

// fork. The child has only the forking thread: the writer thread is gone, and the child
// shares the parent's open cache files and so their file locks, so it must not write.
// pthread_atfork handlers (registered once, for the context's cache) hold the index locks
// across every fork, so the child never inherits one held by the writer; with
// fexmac_before_fork the locks are already held (ForkLocksHeld) and the handlers leave
// them. A fork the handlers did not see is still caught by the process id check.
DiskCache::~DiskCache() {
  DiskCache* Self = this;
  ForkWatched.compare_exchange_strong(Self, nullptr);
}

void DiskCache::AtForkPrepare() {
  auto* Self = ForkWatched.load();
  if (!Self || Self->ForkLocksHeld || !Self->ReadingDiskCache) {
    return;
  }
  Self->IndexLock.lock();
  Self->MemoryLRULock.lock();
  AtForkTookLocks = true;
}

void DiskCache::AtForkParent() {
  auto* Self = ForkWatched.load();
  if (Self && AtForkTookLocks) {
    AtForkTookLocks = false;
    Self->MemoryLRULock.unlock();
    Self->IndexLock.unlock();
  }
}

void DiskCache::AtForkChild() {
  auto* Self = ForkWatched.load();
  if (!Self) {
    return;
  }
  if (AtForkTookLocks) {
    AtForkTookLocks = false;
    Self->MemoryLRULock.unlock();
    Self->IndexLock.unlock();
  }
  Self->BecomeReadOnlyAfterFork();
}

void DiskCache::BecomeReadOnlyAfterFork() {
  if (ForkedChild) {
    return;
  }
  ForkedChild = true;
  OwnerPid = getpid();
  if (WritingDiskCache) {
    WritingDiskCache = false;
    CurrentState = State::ReadOnly;
  }
  if (Writer) {
    // The old queue object is abandoned (its mutex may have been held by the parent's
    // writer thread at the fork); a new one runs the in-memory cache's housekeeping.
    (void)Writer.release();
    FEXCore::Threads::Flags WriterThreadFlags = {.LowPriority = true, .Internal = true};
    Writer = fextl::make_unique<WorkQueueThread>(WriterThreadFlags, "FEX:DiskCache");
  }
}

bool DiskCache::InForkedChild() {
  return OwnerPid != 0 && getpid() != OwnerPid;
}

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
  if (Child) {
    BecomeReadOnlyAfterFork();
  }
}

} // namespace FEXCore::DiskCache
