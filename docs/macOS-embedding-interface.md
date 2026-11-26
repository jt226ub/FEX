# macOS: what an embedder of the core must provide

The translator core (`FEXCore`) does not run programs on its own. Something has to load
the guest program, give it memory, answer its system calls and catch its faults. On Linux
that is FEX's process emulation layer, which this port does not build. On macOS the
embedder is whatever wants x86-64 code translated: a test tool today, a console
compatibility layer such as AnyPS5 later.

This page records what the embedder must do, learned by writing the test tools. It is the
basis for a small, stable C interface, which does not exist yet.

## Duties found so far

| Duty | Why | Where it is done today |
|---|---|---|
| Create the context, set a system call handler and a signal delegator, call `InitCore` | Required by the core | every tool |
| Create a thread state per host thread | Guest registers live there | every tool |
| Allocate the call and return stack for each thread | Guest `call` and `ret` crash without it | `CoreThreadSetup.h` |
| Provide descriptor tables (GDT and LDT) and a 64-bit code segment | Segment loads read them | `CoreThreadSetup.h` |
| Set `SynchronousExceptionHandler` | The core calls it when guest code faults by design, for example at `hlt` | each tool |
| Catch host alignment faults and pass them to the core | Misaligned atomic operations fault on ARM64 and the core rewrites them | `MacOSHostFaults.h` |
| Map guest memory at the addresses the guest expects | The core uses guest addresses as host addresses | each tool |
| Call the allocator's per-thread initialisation | Required before a thread allocates | `CoreThreadTest` |

## Duties known but not done yet

- Catch write faults for self-modifying code detection and pass them to the core.
- Arm and service the interrupt fault page used to stop a thread from another thread.
- Guest signals and exceptions that the guest program handles itself.
- Reconstructing guest state when a fault lands in the middle of translated code.

## Constraints specific to macOS on Apple silicon

- **No guest memory below 4GB.** The system reserves the first 4GB of every 64-bit
  process and refuses to start a program that asks for less. A guest that needs low
  addresses (32-bit code, or a binary linked at a low fixed address) cannot be given them
  directly. Relocatable guest code is unaffected.
- **A large range is reserved for the GPU**, seen from 64GB to 448GB on the hosted Mac.
- **Pages are 16K.** Guest programs assume 4K. Protections and mappings can only be set
  per 16K page.
- **Translated code needs the JIT entitlement** when the program uses the hardened
  runtime (`Data/macOS/jit.entitlements`).

## Proposed interface

A C header with opaque handles, so callers do not depend on the core's C++ types and the
core can change underneath. Roughly:

```c
fexmac_context* fexmac_context_create(const fexmac_config*);
void            fexmac_context_destroy(fexmac_context*);

fexmac_thread*  fexmac_thread_create(fexmac_context*);   // does every per-thread duty above
void            fexmac_thread_destroy(fexmac_thread*);

void fexmac_thread_get_state(fexmac_thread*, fexmac_x86_state*);
void fexmac_thread_set_state(fexmac_thread*, const fexmac_x86_state*);

// Runs guest code at the thread's instruction pointer until it stops, and says why:
// system call, halt, guest fault, or a request from another thread.
fexmac_stop fexmac_thread_run(fexmac_thread*);

// Tells the core that guest code in a range changed or was unmapped.
void fexmac_invalidate(fexmac_context*, uint64_t address, uint64_t size);

// Installs the host fault handlers described above. Once per process.
void fexmac_install_fault_handlers(void);
```

The key choice is that `fexmac_thread_run` returns to the caller for a system call instead
of calling back into it. That keeps the caller's control flow ordinary and avoids
`longjmp` out of translated code, which the test tools currently rely on.

Not decided: the name, whether memory mapping goes through the interface, and how guest
signals are delivered.
