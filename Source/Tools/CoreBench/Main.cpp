// SPDX-License-Identifier: MIT
//
// FEXCoreBench: loads a freestanding x86-64 program and runs it through the fexmac
// interface (fexmac.h only). Made for the benchmark in unittests/macOS/Bench, but it
// runs any image built the same way.
//
//     FEXCoreBench [--option NAME=VALUE]... [--quick] [--reps N] [--only k1,k2] image
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
// guest thread, wait for it. Each guest thread runs on its own host thread at the
// user-interactive quality of service. Any other system call stops the program.
//
// Exit status: the guest's exit status (the number of wrong checksums), or 100 and up
// for a loader failure.

#include <fexmac.h>

#include <mach-o/loader.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
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

constexpr int ExitLoaderFailure = 100;
constexpr int ExitGuestFailure = 101;

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R10 = 10 };

constexpr size_t MainStackSize = 64ull << 20; // deep recursion runs on the main thread

struct Image {
  uint64_t Entry {};
  uint64_t Low {};
  uint64_t High {};
};

[[noreturn]] void Die(const char* Format, const char* Detail = "") {
  fprintf(stderr, "FEXCoreBench: ");
  fprintf(stderr, Format, Detail);
  fprintf(stderr, "\n");
  exit(ExitLoaderFailure);
}

// Maps a static x86-64 Mach-O executable at its link addresses.
Image LoadImage(const char* Path) {
  const int FD = open(Path, O_RDONLY);
  if (FD < 0) {
    Die("cannot open %s", Path);
  }
  struct stat St {};
  fstat(FD, &St);
  std::vector<uint8_t> File(static_cast<size_t>(St.st_size));
  if (read(FD, File.data(), File.size()) != static_cast<ssize_t>(File.size())) {
    Die("cannot read %s", Path);
  }
  close(FD);

  if (File.size() < sizeof(mach_header_64)) {
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
  size_t Offset = sizeof(mach_header_64);
  for (uint32_t i = 0; i < Header.ncmds; ++i) {
    load_command Cmd;
    if (Offset + sizeof(Cmd) > File.size()) {
      Die("%s: load commands run past the end of the file", Path);
    }
    memcpy(&Cmd, File.data() + Offset, sizeof(Cmd));
    if (Cmd.cmdsize < sizeof(Cmd) || Offset + Cmd.cmdsize > File.size()) {
      Die("%s: bad load command size", Path);
    }
    if (Cmd.cmd == LC_SEGMENT_64) {
      segment_command_64 Seg;
      memcpy(&Seg, File.data() + Offset, sizeof(Seg));
      // __PAGEZERO reserves the low 4GB and has no contents.
      if (Seg.vmsize != 0 && !(Seg.vmaddr == 0 && Seg.filesize == 0)) {
        if (Seg.fileoff + Seg.filesize > File.size() || Seg.filesize > Seg.vmsize) {
          Die("%s: segment contents outside the file", Path);
        }
        Segments.push_back({Seg.vmaddr, Seg.vmsize, Seg.fileoff, Seg.filesize});
      }
    } else if (Cmd.cmd == LC_UNIXTHREAD) {
      // thread_command, then flavor and count, then x86_THREAD_STATE64: rax rbx rcx rdx
      // rdi rsi rbp rsp r8-r15 rip ...
      constexpr uint32_t X86ThreadState64 = 4;
      uint32_t Flavor;
      memcpy(&Flavor, File.data() + Offset + 8, 4);
      if (Flavor != X86ThreadState64 || Cmd.cmdsize < 16 + 17 * 8) {
        Die("%s: LC_UNIXTHREAD is not an x86-64 thread state", Path);
      }
      memcpy(&Result.Entry, File.data() + Offset + 16 + 16 * 8, 8);
      HaveEntry = true;
    } else if (Cmd.cmd == LC_LOAD_DYLINKER || Cmd.cmd == LC_LOAD_DYLIB || Cmd.cmd == LC_DYLD_INFO_ONLY || Cmd.cmd == LC_DYLD_CHAINED_FIXUPS) {
      Die("%s needs the dynamic linker; link it with -static", Path);
    }
    Offset += Cmd.cmdsize;
  }
  if (Segments.empty() || !HaveEntry) {
    Die("%s has no segments or no entry point", Path);
  }

  const uint64_t Page = static_cast<uint64_t>(getpagesize());
  Result.Low = ~0ull;
  for (auto& S : Segments) {
    Result.Low = std::min(Result.Low, S.VMAddr);
    Result.High = std::max(Result.High, S.VMAddr + S.VMSize);
  }
  Result.Low &= ~(Page - 1);
  Result.High = (Result.High + Page - 1) & ~(Page - 1);
  if (Result.Low < (4ull << 30)) {
    Die("%s is linked below 4GB, where macOS allows no memory", Path);
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
  fexmac_thread* Thread {};
  std::thread Host;
  std::atomic<bool> Failed {};
};

std::mutex ThreadsLock;
std::vector<GuestThread*> Threads; // index = guest handle; nullptr once joined
std::atomic<bool> AnyFailure {};
std::atomic<bool> MainExited {};
int ExitStatus = 0;

void SetUserInteractive() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

// Runs a guest thread until it stops. Returns false (after reporting) for any stop that
// is not one of the benchmark's own exits.
bool RunUntilExit(fexmac_thread* Thread, const char* Who) {
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  const fexmac_result Result = fexmac_thread_run(Thread, &Stop);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "FEXCoreBench: %s: run failed: %s\n", Who, fexmac_result_string(Result));
    return false;
  }
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  fexmac_thread_get_state(Thread, &State);
  if (Stop.reason == FEXMAC_STOP_SYSCALL && (State.gpr[RAX] == SysExit || State.gpr[RAX] == SysThreadExit)) {
    return true;
  }
  fprintf(stderr, "FEXCoreBench: %s stopped unexpectedly: reason %d at 0x%llx (rax 0x%llx, signal %d, trap %u, fault address 0x%llx)\n",
          Who, Stop.reason, static_cast<unsigned long long>(Stop.instruction_address), static_cast<unsigned long long>(State.gpr[RAX]),
          Stop.signal, Stop.trap_number, static_cast<unsigned long long>(Stop.fault_address));
  return false;
}

fexmac_syscall_action OnSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  fexmac_context* Context = fexmac_thread_context(Thread);
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  if (fexmac_thread_get_state(Thread, &State) != FEXMAC_OK) {
    return FEXMAC_SYSCALL_STOP;
  }
  uint64_t& Ret = State.gpr[RAX];
  switch (State.gpr[RAX]) {
  case SysExit:
    ExitStatus = static_cast<int>(State.gpr[RDI]);
    MainExited = true;
    return FEXMAC_SYSCALL_STOP;
  case SysThreadExit: return FEXMAC_SYSCALL_STOP;
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
    if (fexmac_thread_create(Context, &G->Thread) != FEXMAC_OK) {
      delete G;
      Ret = ~0ull;
      break;
    }
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
      Ret = Threads.size();
      Threads.push_back(G);
    }
    G->Host = std::thread([G] {
      SetUserInteractive();
      if (!RunUntilExit(G->Thread, "guest thread")) {
        G->Failed = true;
        AnyFailure = true;
      }
    });
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
  default:
    fprintf(stderr, "FEXCoreBench: unknown system call 0x%llx at 0x%llx\n", static_cast<unsigned long long>(State.gpr[RAX]),
            static_cast<unsigned long long>(State.rip));
    AnyFailure = true;
    return FEXMAC_SYSCALL_STOP;
  }
  fexmac_thread_set_state(Thread, &State);
  return FEXMAC_SYSCALL_RESUME;
}

void Usage() {
  fprintf(stderr, "usage: FEXCoreBench [--option NAME=VALUE]... [--quick] [--reps N] [--only k1,k2] image\n");
  exit(ExitLoaderFailure);
}
} // namespace

int main(int argc, char** argv) {
  SetUserInteractive();
  std::vector<std::string> OptionText;
  uint64_t Quick = 0, Reps = 5;
  std::string Only;
  const char* ImagePath = nullptr;
  for (int i = 1; i < argc; ++i) {
    const std::string Arg = argv[i];
    if (Arg == "--option" && i + 1 < argc) {
      OptionText.push_back(argv[++i]);
    } else if (Arg == "--quick") {
      Quick = 1;
    } else if (Arg == "--reps" && i + 1 < argc) {
      Reps = strtoull(argv[++i], nullptr, 10);
    } else if (Arg == "--only" && i + 1 < argc) {
      Only = argv[++i];
    } else if (Arg[0] != '-' && !ImagePath) {
      ImagePath = argv[i];
    } else {
      Usage();
    }
  }
  if (!ImagePath) {
    Usage();
  }
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
  if (const fexmac_result R = fexmac_context_create(&Config, &Context); R != FEXMAC_OK) {
    fprintf(stderr, "FEXCoreBench: cannot create the context: %s\n", fexmac_result_string(R));
    return ExitLoaderFailure;
  }

  fexmac_thread* Main {};
  if (fexmac_thread_create(Context, &Main) != FEXMAC_OK) {
    Die("cannot create the main guest thread");
  }
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  fexmac_thread_get_state(Main, &State);
  State.rip = Img.Entry;
  State.gpr[RSP] = reinterpret_cast<uint64_t>(Stack) + MainStackSize - 64;
  State.gpr[RDI] = Quick;
  State.gpr[RSI] = Reps;
  State.gpr[RDX] = Only.empty() ? 0 : reinterpret_cast<uint64_t>(OnlyGuest);
  fexmac_thread_set_state(Main, &State);

  const bool Ok = RunUntilExit(Main, "main guest thread") && MainExited;
  fflush(stdout);

  // Threads the guest never joined (only after a failure).
  {
    std::lock_guard Lock(ThreadsLock);
    for (auto*& G : Threads) {
      if (G) {
        G->Host.detach();
      }
    }
  }
  if (!Ok || AnyFailure) {
    fprintf(stderr, "FEXCoreBench: the guest did not finish normally\n");
    _exit(ExitGuestFailure);
  }
  fexmac_thread_destroy(Main);
  fexmac_context_destroy(Context);
  return ExitStatus;
}
