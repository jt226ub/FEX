// SPDX-License-Identifier: MIT
//
// FEXCoreBench: loads a freestanding x86-64 program and runs it through the fexmac
// interface (fexmac.h only). Made for the benchmark in unittests/macOS/Bench, but it
// runs any image built the same way.
//
//     FEXCoreBench [--option NAME=VALUE]... [--code-cache DIR] [--quick]
//                  [--literal [--expect-kernels N]] [--reps N] [--only k1,k2] image
//
// The image is a static, non-position-independent x86-64 Mach-O executable linked at
// a fixed address above 4GB (`ld -static -no_pie -image_base ...`), so it has no
// relocations or imports: the loader maps its segments at their addresses, copies the
// file contents, and starts it at the entry point in LC_UNIXTHREAD. Mach-O is used only
// as a container because the installed linker (ld64) writes nothing else; the program
// never sees macOS. Guest addresses are host addresses.
//
// The program's only host services are the benchmark's private system calls
// (platform_guest.c documents them): exit, write to standard output, a clock, start a
// guest thread, wait for it, end a guest thread. Each guest thread runs on its own host
// thread at the user-interactive quality of service. Only the main guest thread may
// exit the program, and only a started thread may end itself; anything else, and any
// other system call, is a failure.
//
// --literal runs the guest's literal mode (exit criterion 7 to the letter): the guest
// may make one system call, exit, and nothing else; it reports the number of wrong
// checksums and the number of kernels it ran. The loader requires 0 wrong, at least one
// kernel run (exactly N with --expect-kernels), and no other system call.
//
// The image's executable segments are registered with fexmac_add_code_range (not
// writable: the guest never writes its code). --code-cache DIR turns the code cache on
// through fexmac's config fields (code_cache_enabled, code_cache_directory); the cache
// is flushed before exit. The translation counters (fexmac_context_get_counters) are
// printed on standard error at exit as
//   FEXCoreBench: translation: compiled N loaded N rejected N queued N cache STATE
//
// The guest's exit report is printed on standard error. Exit status of this program:
//   0    the guest exited normally and every checksum was right;
//   1    the guest exited normally and reported wrong checksums;
//   100  a loader failure (bad arguments, bad image, the library refused);
//   101  the guest broke the contract: it stopped other than by exit, made a system
//        call it may not make, exited with guest threads not joined, or (literal mode)
//        ran the wrong number of kernels.
// A guest fault the library reports as a stop (an invalid instruction, int3, ...) is
// printed with the thread it happened on, and the process then dies with that signal.

#include <fexmac.h>

#include <mach-o/loader.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <pthread/qos.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {
// The private system call numbers (see platform_guest.c).
constexpr uint64_t SysExit = 0xBE00;
constexpr uint64_t SysWrite = 0xBE01;
constexpr uint64_t SysTime = 0xBE02;
constexpr uint64_t SysSpawn = 0xBE03;
constexpr uint64_t SysJoin = 0xBE04;
constexpr uint64_t SysThreadExit = 0xBE05;

// Guest flags (bench.h).
constexpr uint64_t FlagQuick = 1;
constexpr uint64_t FlagLiteral = 2;

constexpr int ExitWrongChecksums = 1;
constexpr int ExitLoaderFailure = 100;
constexpr int ExitContractBroken = 101;

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R10 = 10 };

constexpr size_t MainStackSize = 64ull << 20; // deep recursion runs on the main thread

struct Image {
  uint64_t Entry {};
  uint64_t Low {};
  uint64_t High {};
  // Executable segments, [address, address + size).
  std::vector<std::pair<uint64_t, uint64_t>> Code;
};

[[noreturn]] void Die(const char* Format, const char* Detail = "") {
  fprintf(stderr, "FEXCoreBench: ");
  fprintf(stderr, Format, Detail);
  fprintf(stderr, "\n");
  exit(ExitLoaderFailure);
}

// Maps a static x86-64 Mach-O executable at its link addresses. Every size and offset
// from the file is checked without overflow before it is used.
Image LoadImage(const char* Path) {
  const int FD = open(Path, O_RDONLY);
  if (FD < 0) {
    Die("cannot open %s", Path);
  }
  struct stat St {};
  if (fstat(FD, &St) != 0 || St.st_size < 0) {
    Die("cannot stat %s", Path);
  }
  std::vector<uint8_t> File(static_cast<size_t>(St.st_size));
  if (read(FD, File.data(), File.size()) != static_cast<ssize_t>(File.size())) {
    Die("cannot read %s", Path);
  }
  close(FD);
  const uint64_t Size = File.size();

  if (Size < sizeof(mach_header_64)) {
    Die("%s is too small to be a Mach-O file", Path);
  }
  mach_header_64 Header;
  memcpy(&Header, File.data(), sizeof(Header));
  if (Header.magic != MH_MAGIC_64 || Header.cputype != CPU_TYPE_X86_64 || Header.filetype != MH_EXECUTE) {
    Die("%s is not an x86-64 Mach-O executable", Path);
  }
  if (Header.flags & MH_PIE) {
    Die("%s is position independent; link it with -static -no_pie at a fixed address", Path);
  }

  struct Segment {
    uint64_t VMAddr, VMSize, FileOff, FileSize;
  };
  std::vector<Segment> Segments;
  Image Result {};
  bool HaveEntry = false;
  uint64_t Offset = sizeof(mach_header_64);
  for (uint32_t i = 0; i < Header.ncmds; ++i) {
    load_command Cmd;
    if (Offset > Size || Size - Offset < sizeof(Cmd)) {
      Die("%s: load commands run past the end of the file", Path);
    }
    memcpy(&Cmd, File.data() + Offset, sizeof(Cmd));
    if (Cmd.cmdsize < sizeof(Cmd) || Cmd.cmdsize > Size - Offset) {
      Die("%s: bad load command size", Path);
    }
    const uint8_t* CmdData = File.data() + Offset;
    if (Cmd.cmd == LC_SEGMENT_64) {
      segment_command_64 Seg;
      if (Cmd.cmdsize < sizeof(Seg)) {
        Die("%s: LC_SEGMENT_64 too small", Path);
      }
      memcpy(&Seg, CmdData, sizeof(Seg));
      // __PAGEZERO reserves the low 4GB and has no contents.
      if (Seg.vmsize != 0 && !(Seg.vmaddr == 0 && Seg.filesize == 0)) {
        if (Seg.fileoff > Size || Seg.filesize > Size - Seg.fileoff || Seg.filesize > Seg.vmsize || Seg.vmsize > UINT64_MAX - Seg.vmaddr) {
          Die("%s: segment outside the file or the address space", Path);
        }
        Segments.push_back({Seg.vmaddr, Seg.vmsize, Seg.fileoff, Seg.filesize});
        if (Seg.initprot & VM_PROT_EXECUTE) {
          Result.Code.emplace_back(Seg.vmaddr, Seg.vmsize);
        }
      }
    } else if (Cmd.cmd == LC_UNIXTHREAD) {
      // thread_command (cmd, cmdsize), then flavor and count (in 32-bit words), then
      // x86_THREAD_STATE64: rax rbx rcx rdx rdi rsi rbp rsp r8-r15 rip ...
      constexpr uint32_t X86ThreadState64 = 4;
      constexpr uint32_t X86ThreadState64Count = 42;
      constexpr uint32_t StateOffset = 16;
      if (Cmd.cmdsize < StateOffset) {
        Die("%s: LC_UNIXTHREAD too small", Path);
      }
      uint32_t Flavor, Count;
      memcpy(&Flavor, CmdData + 8, 4);
      memcpy(&Count, CmdData + 12, 4);
      if (Flavor != X86ThreadState64 || Count < X86ThreadState64Count || Count > (Cmd.cmdsize - StateOffset) / 4) {
        Die("%s: LC_UNIXTHREAD is not an x86-64 thread state", Path);
      }
      memcpy(&Result.Entry, CmdData + StateOffset + 16 * 8, 8);
      HaveEntry = true;
    } else if (Cmd.cmd == LC_LOAD_DYLINKER || Cmd.cmd == LC_LOAD_DYLIB || Cmd.cmd == LC_DYLD_INFO_ONLY || Cmd.cmd == LC_DYLD_CHAINED_FIXUPS) {
      Die("%s needs the dynamic linker; link it with -static", Path);
    }
    Offset += Cmd.cmdsize;
  }
  if (Segments.empty() || !HaveEntry || Result.Code.empty()) {
    Die("%s has no segments, no executable segment or no entry point", Path);
  }

  const uint64_t Page = static_cast<uint64_t>(getpagesize());
  Result.Low = ~0ull;
  for (auto& S : Segments) {
    Result.Low = std::min(Result.Low, S.VMAddr);
    Result.High = std::max(Result.High, S.VMAddr + S.VMSize);
  }
  Result.Low &= ~(Page - 1);
  if (Result.High > UINT64_MAX - Page) {
    Die("%s: image reaches the end of the address space", Path);
  }
  Result.High = (Result.High + Page - 1) & ~(Page - 1);
  if (Result.Low < (4ull << 30)) {
    Die("%s is linked below 4GB, where macOS allows no memory", Path);
  }
  if (Result.Entry < Result.Low || Result.Entry >= Result.High) {
    Die("%s: entry point outside the image", Path);
  }
  // A hint, not MAP_FIXED: MAP_FIXED would silently replace anything the host already
  // has there, and macOS has no MAP_FIXED_NOREPLACE. Refuse if the range is taken.
  void* Want = reinterpret_cast<void*>(Result.Low);
  void* Got = mmap(Want, Result.High - Result.Low, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Got != Want) {
    if (Got != MAP_FAILED) {
      munmap(Got, Result.High - Result.Low);
    }
    Die("cannot map the image at its link address (range in use?): %s", Path);
  }
  for (auto& S : Segments) {
    memcpy(reinterpret_cast<void*>(S.VMAddr), File.data() + S.FileOff, S.FileSize);
  }
  return Result;
}

uint64_t NowNs() {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

struct GuestThread {
  uint64_t Handle {};
  fexmac_thread* Thread {};
  std::thread Host;
  std::atomic<bool> Failed {};
  std::atomic<bool> Finished {};
};

std::mutex ThreadsLock;
// Index = guest handle. A slot is reserved (nullptr) while its host thread is started,
// filled once the thread is joinable, and cleared when the guest joins it.
std::vector<GuestThread*> Threads;
std::atomic<bool> ContractBroken {};
fexmac_thread* MainThread {};
bool Literal {};

// What the main thread's exit reported.
std::atomic<bool> MainExited {};
uint64_t GuestFailures = 0;
uint64_t GuestKernels = 0;

void SetUserInteractive() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

void Broken(const char* Who, const char* What, unsigned long long Detail = 0) {
  fprintf(stderr, "FEXCoreBench: %s: %s (0x%llx)\n", Who, What, Detail);
  ContractBroken = true;
}

// Runs a guest thread until it stops. Returns true if it stopped through its proper
// exit (exit for the main thread, thread exit for a started one).
bool RunUntilExit(fexmac_thread* Thread, const std::string& Who, bool IsMain) {
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  const fexmac_result Result = fexmac_thread_run(Thread, &Stop);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "FEXCoreBench: %s: run failed: %s\n", Who.c_str(), fexmac_result_string(Result));
    return false;
  }
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  fexmac_thread_get_state(Thread, &State);
  if (Stop.reason == FEXMAC_STOP_SYSCALL && State.gpr[RAX] == (IsMain ? SysExit : SysThreadExit) && !ContractBroken) {
    return true;
  }
  if (Stop.reason == FEXMAC_STOP_SYSCALL) {
    // The system call callback has already said what was wrong.
    return false;
  }
  fprintf(stderr, "FEXCoreBench: %s stopped: reason %d at 0x%llx (rax 0x%llx, signal %d, trap %u, si_code %d, fault address 0x%llx)\n",
          Who.c_str(), Stop.reason, static_cast<unsigned long long>(Stop.instruction_address), static_cast<unsigned long long>(State.gpr[RAX]),
          Stop.signal, Stop.trap_number, Stop.si_code, static_cast<unsigned long long>(Stop.fault_address));
  if ((Stop.reason == FEXMAC_STOP_GUEST_FAULT || Stop.reason == FEXMAC_STOP_UNSUPPORTED_INSTRUCTION || Stop.reason == FEXMAC_STOP_HALT) &&
      Stop.signal > 0 && Stop.signal < NSIG) {
    // Die as the guest would have: with the signal its fault maps to.
    fflush(stdout);
    signal(Stop.signal, SIG_DFL);
    raise(Stop.signal);
  }
  return false;
}

fexmac_syscall_action OnSyscall(fexmac_thread* Thread, void*) {
  const bool IsMain = Thread == MainThread;
  auto* Self = static_cast<GuestThread*>(fexmac_thread_get_user_data(Thread));
  const std::string Who = IsMain ? "main guest thread" : "guest thread " + std::to_string(Self ? Self->Handle : ~0ull);
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  if (fexmac_thread_get_state(Thread, &State) != FEXMAC_OK) {
    Broken(Who.c_str(), "cannot read the guest state");
    return FEXMAC_SYSCALL_STOP;
  }
  const uint64_t Number = State.gpr[RAX];
  if (Literal && Number != SysExit) {
    Broken(Who.c_str(), "system call other than exit in the literal mode", Number);
    return FEXMAC_SYSCALL_STOP;
  }
  uint64_t& Ret = State.gpr[RAX];
  switch (Number) {
  case SysExit:
    if (!IsMain) {
      Broken(Who.c_str(), "exit from a started thread (only the main thread may exit the program)");
      return FEXMAC_SYSCALL_STOP;
    }
    GuestFailures = State.gpr[RDI];
    GuestKernels = State.gpr[RSI];
    MainExited = true;
    return FEXMAC_SYSCALL_STOP;
  case SysThreadExit:
    if (IsMain) {
      Broken(Who.c_str(), "thread exit from the main thread (it must exit the program)");
    }
    return FEXMAC_SYSCALL_STOP;
  case SysWrite: {
    const auto* Data = reinterpret_cast<const char*>(State.gpr[RDI]);
    size_t Left = State.gpr[RSI];
    while (Left) {
      const ssize_t N = write(1, Data, Left);
      if (N <= 0) {
        break;
      }
      Data += N;
      Left -= static_cast<size_t>(N);
    }
    Ret = State.gpr[RSI] - Left;
    break;
  }
  case SysTime: Ret = NowNs(); break;
  case SysSpawn: {
    // Creating a thread and setting its state from the callback is allowed; running it
    // here is not, so a new host thread runs it.
    auto* G = new GuestThread;
    if (fexmac_thread_create(fexmac_thread_context(Thread), &G->Thread) != FEXMAC_OK) {
      delete G;
      Ret = ~0ull;
      break;
    }
    fexmac_thread_set_user_data(G->Thread, G);
    fexmac_x86_state New;
    fexmac_x86_state_init(&New);
    fexmac_thread_get_state(G->Thread, &New);
    New.rip = State.gpr[RDI];
    New.gpr[RSP] = State.gpr[RSI];
    New.gpr[RDI] = State.gpr[RDX];
    New.gpr[RSI] = State.gpr[R10];
    New.mxcsr = State.mxcsr;
    New.fcw = State.fcw;
    fexmac_thread_set_state(G->Thread, &New);
    {
      std::lock_guard Lock(ThreadsLock);
      G->Handle = Threads.size();
      Threads.push_back(nullptr); // reserved: not joinable yet
    }
    G->Host = std::thread([G] {
      SetUserInteractive();
      if (!RunUntilExit(G->Thread, "guest thread " + std::to_string(G->Handle), false)) {
        G->Failed = true;
        ContractBroken = true;
      }
      G->Finished = true;
    });
    {
      std::lock_guard Lock(ThreadsLock);
      Threads[G->Handle] = G; // joinable from now on
    }
    Ret = G->Handle;
    break;
  }
  case SysJoin: {
    GuestThread* G {};
    {
      std::lock_guard Lock(ThreadsLock);
      const uint64_t Handle = State.gpr[RDI];
      if (Handle < Threads.size()) {
        G = Threads[Handle];
        Threads[Handle] = nullptr;
      }
    }
    if (!G) {
      Ret = ~0ull;
      break;
    }
    G->Host.join();
    fexmac_thread_destroy(G->Thread);
    Ret = G->Failed ? ~0ull : 0;
    delete G;
    break;
  }
  default: Broken(Who.c_str(), "unknown system call", Number); return FEXMAC_SYSCALL_STOP;
  }
  fexmac_thread_set_state(Thread, &State);
  return FEXMAC_SYSCALL_RESUME;
}

void Usage() {
  fprintf(stderr, "usage: FEXCoreBench [--option NAME=VALUE]... [--code-cache DIR] [--quick] [--literal [--expect-kernels N]] [--reps N] "
                  "[--only k1,k2] image\n");
  exit(ExitLoaderFailure);
}
} // namespace

int main(int argc, char** argv) {
  SetUserInteractive();
  std::vector<std::string> OptionText;
  uint64_t Flags = 0, Reps = 5;
  long long ExpectKernels = -1;
  std::string Only;
  const char* CacheDir = nullptr;
  const char* ImagePath = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string Arg = argv[i];
    if (Arg == "--option" && i + 1 < argc) {
      OptionText.push_back(argv[++i]);
    } else if (Arg == "--code-cache" && i + 1 < argc) {
      CacheDir = argv[++i];
    } else if (Arg == "--quick") {
      Flags |= FlagQuick;
    } else if (Arg == "--literal") {
      Flags |= FlagLiteral;
    } else if (Arg == "--expect-kernels" && i + 1 < argc) {
      ExpectKernels = strtoll(argv[++i], nullptr, 10);
    } else if (Arg == "--reps" && i + 1 < argc) {
      Reps = strtoull(argv[++i], nullptr, 10);
    } else if (Arg == "--only" && i + 1 < argc) {
      Only = argv[++i];
    } else if (!Arg.empty() && Arg[0] != '-' && !ImagePath) {
      ImagePath = argv[i];
    } else {
      Usage();
    }
  }
  if (!ImagePath) {
    Usage();
  }
  Literal = (Flags & FlagLiteral) != 0;
  if ((fexmac_version() >> 16) != FEXMAC_VERSION_MAJOR) {
    Die("fexmac version mismatch");
  }

  std::vector<std::string> Names, Values;
  for (auto& T : OptionText) {
    const auto Eq = T.find('=');
    if (Eq == std::string::npos) {
      Die("--option needs NAME=VALUE: %s", T.c_str());
    }
    Names.push_back(T.substr(0, Eq));
    Values.push_back(T.substr(Eq + 1));
  }
  std::vector<fexmac_option> Options;
  for (size_t i = 0; i < Names.size(); ++i) {
    Options.push_back({Names[i].c_str(), Values[i].c_str()});
  }

  const Image Img = LoadImage(ImagePath);

  // The filter string and the main stack live in guest memory (any host memory).
  auto* Stack = static_cast<uint8_t*>(mmap(nullptr, MainStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Stack == MAP_FAILED) {
    Die("cannot allocate the guest stack");
  }
  char* OnlyGuest = reinterpret_cast<char*>(Stack);
  if (Only.size() >= 4096) {
    Die("--only list too long");
  }
  memcpy(OnlyGuest, Only.c_str(), Only.size() + 1);

  if (fexmac_install_fault_handlers() != FEXMAC_OK) {
    Die("cannot install the fault handlers");
  }

  fexmac_context* Context {};
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Options.data();
  Config.option_count = Options.size();
  Config.syscall = OnSyscall;
  if (CacheDir) {
    Config.code_cache_enabled = 1;
    Config.code_cache_directory = CacheDir;
  }
  if (const fexmac_result R = fexmac_context_create(&Config, &Context); R != FEXMAC_OK) {
    fprintf(stderr, "FEXCoreBench: cannot create the context: %s\n", fexmac_result_string(R));
    return ExitLoaderFailure;
  }
  for (const auto& [Address, Size] : Img.Code) {
    if (fexmac_add_code_range(Context, Address, Size, 0) != FEXMAC_OK) {
      Die("cannot register the image's code range");
    }
  }

  if (fexmac_thread_create(Context, &MainThread) != FEXMAC_OK) {
    Die("cannot create the main guest thread");
  }
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  fexmac_thread_get_state(MainThread, &State);
  State.rip = Img.Entry;
  State.gpr[RSP] = reinterpret_cast<uint64_t>(Stack) + MainStackSize - 64;
  State.gpr[RDI] = Flags;
  State.gpr[RSI] = Reps;
  State.gpr[RDX] = Only.empty() ? 0 : reinterpret_cast<uint64_t>(OnlyGuest);
  fexmac_thread_set_state(MainThread, &State);

  const bool Ok = RunUntilExit(MainThread, "main guest thread", true) && MainExited;
  fflush(stdout);

  // Guest threads the guest never joined. They may still be running guest code, so the
  // context must not be destroyed under them: report and leave without tearing down.
  size_t Unjoined = 0, Running = 0;
  {
    std::lock_guard Lock(ThreadsLock);
    for (auto* G : Threads) {
      if (G) {
        ++Unjoined;
        Running += G->Finished ? 0 : 1;
      }
    }
  }
  if (Unjoined) {
    fprintf(stderr, "FEXCoreBench: the guest exited with %zu guest thread(s) not joined, %zu still running\n", Unjoined, Running);
  }
  if (MainExited) {
    fprintf(stderr, "FEXCoreBench: guest exit: %llu wrong checksum(s), %llu kernel(s) run%s\n", static_cast<unsigned long long>(GuestFailures),
            static_cast<unsigned long long>(GuestKernels), Literal ? " (literal mode: no system call but exit)" : "");
  }
  // The translation counters, after the cache has written everything (when it is on).
  if (CacheDir && Unjoined == 0) {
    fexmac_context_flush_cache(Context);
  }
  fexmac_translation_counters Counters;
  fexmac_translation_counters_init(&Counters);
  if (fexmac_context_get_counters(Context, &Counters) == FEXMAC_OK) {
    static const char* const States[] = {"off", "active", "read-only", "untrusted", "unavailable"};
    fprintf(stderr, "FEXCoreBench: translation: compiled %llu loaded %llu rejected %llu queued %llu cache %s\n",
            static_cast<unsigned long long>(Counters.blocks_compiled), static_cast<unsigned long long>(Counters.blocks_loaded),
            static_cast<unsigned long long>(Counters.cache_entries_rejected), static_cast<unsigned long long>(Counters.cache_entries_queued),
            Counters.cache_state <= FEXMAC_CACHE_UNAVAILABLE ? States[Counters.cache_state] : "?");
    if (CacheDir && Counters.cache_state != FEXMAC_CACHE_ACTIVE) {
      fprintf(stderr, "FEXCoreBench: the code cache in %s was not used\n", CacheDir);
      ContractBroken = true;
    }
  }
  bool Contract = Ok && !ContractBroken && Unjoined == 0;
  if (Contract && Literal) {
    if (GuestKernels == 0 || (ExpectKernels >= 0 && GuestKernels != static_cast<unsigned long long>(ExpectKernels))) {
      fprintf(stderr, "FEXCoreBench: literal mode: expected %lld kernel(s) to run\n", ExpectKernels);
      Contract = false;
    }
  }
  if (!Contract) {
    fprintf(stderr, "FEXCoreBench: FAIL: the guest did not finish as the benchmark ABI requires\n");
    _exit(ExitContractBroken);
  }
  fexmac_thread_destroy(MainThread);
  fexmac_context_destroy(Context);
  return GuestFailures == 0 ? 0 : ExitWrongChecksums;
}
