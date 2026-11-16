# runtime/platform

| File | What it is |
|---|---|
| `platform.h` | The operating system interface the rest of the runtime uses |
| `linux.c`, `linux_int.h` | Linux: threads, preemption through signals, fibers; the header is what the `linux_*.c` parts share |
| `linux_sync.c` | Linux: clock, locks and condition variables on futex |
| `linux_fs.c` | Linux: files and file system (atomic write, rename without replace) |
| `linux_mem.c` | Linux: virtual memory, write watching with userfaultfd (Linux 6.7+), fault handler; `plat_mem_poke` writes through the protection via `/proc/self/mem` |
| `linux_sys.c` | Linux: process, desktop, libraries, machine load (CPU, temperature, GPU) and desktop shortcut |
| `win32.c`, `win32_int.h` | Windows: threads, preemption (SuspendThread), fibers, clock, locks; the header is what the `win32_*.c` parts share |
| `win32_fs.c` | Windows: files and file system (UTF-16 paths) |
| `win32_mem.c` | Windows: virtual memory and write watching (`MEM_WRITE_WATCH`) |
| `win32_sys.c` | Windows: process, desktop and libraries |
| `win32_load.c` | Windows: machine load (CPU, PDH for temperature and GPU) and desktop shortcut |
| `ps5_memory.c`, `ps5_memory.h` | PS5: CPU-only direct backing for one owned 64 KiB block, used by the native smoke probe |
| `ps5_mixed.c`, `ps5_mixed.h` | PS5 prototype: 4 KiB guest permissions within one owned backing block, exercised by the native smoke title and synthetic host test |
| `ps5_guest_span.c`, `ps5_guest_span.h` | Game-free prototype for checked host access and mapping quiescence across two adjacent PS5 backing blocks |
| `ps5_guest_memory.c`, `ps5_guest_memory.h` | PS5 loader platform memory: exact high144 reservations, sparse direct backing, checked reads and shutdown; `MKW_PACK`'s packing of cold frozen units (zstd, `plat_mem_pack`) and their unpacking when backed again |
| `ps5_loader_io.c` | PS5 loader file reads, paths, clock and shell exit |
| `ps5_mutex.c` | PS5 loader reader and writer locks over pthreads |
| `ps5_threads.c`, `ps5_fiber.c`, `fiber_x86.h` | PS5 host threads, CPU timing, and System V fibers |
| `ps5_sys.c` | PS5 POSIX file writes, system/load stubs, and zstd tracer no-ops |
| `fault_guard.c` | Linux and PS5: `plat_mem_copy_guarded`, a copy that returns 0 when it faults instead of running the fault hooks, for copies made with a platform lock held (the PS5's `plat_mem_read`, which hung run 25 against the early-read watch) |
| `sigsample.c` | Linux and PS5: the runtime's threads in a registry; each one's CPU time read on the thread itself through SIGPROF (the PS5's `pthread_getcpuclockid` answers with the caller's clock), and the sampler thread of the profiler (`core/diag/prof_posix.c`) |
| `common.c`, `fp_x86.h` | Common parts, x86 floating point control |
| `prot_watch.c`, `prot_watch.h` | Page-protection write watch shared by PS5 (16 KiB native pages) and Linux (`MKW_PROT_WATCH=1`, 4 KiB pages) |
| `mkw.rc`, `mkw.manifest` | `mkw.exe`'s manifest (UTF-8 code page, from totk-pc-recomp); `build.py --target windows` |

The mixed-page prototype commits and decommits one 4 KiB guest page at a time. It keeps the native 64 KiB mapping
until the last guest page is decommitted. Each newly committed page reads zero, including when its neighbors retain
their bytes. A uniform 16 KiB group can use native protection. A mixed group stays writable in the native mapping
and the inline check validates the guest permission of every page the access touches.
If the first commit fails and cleanup leaves native backing behind, release the block before trying another commit.

The prototype runs in the game-free smoke title and host test. Mapping and permission changes require all accesses to the block to
be quiescent, including any access that already passed the check. A raw pointer dereference bypasses the check,
including pointers used by SDK, file, and GPU paths. The test does not implement those paths, a guest fault handler,
or write-watch.

The guest-span prototype checks every guest page before returning a pinned pointer or copying bytes. A mapping change
blocks new pins and drains existing pins on its own 64 KiB block. A deferred copy-out checks the current permissions,
the destination block generation, and the span lifetime. Destroying the span expires all its tickets, including after
reinitialization at the same address. A decommit attempt invalidates tickets for its entire block. Committing a
reserved page and changing permissions preserve existing backing identity. The synthetic Linux test exercises
pthread synchronization for synchronous host access and deferred copy-out.

The loader backend reserves native-aligned windows and records only the 64 KiB units it touches. Guest commits,
decommits, and permissions have 4 KiB granularity. A decommit that loses the exact reservation leaves a hole that
the backend excludes from future fixed maps and shutdown. It retains the subrange boundaries after reuse so
shutdown can unmap owned spans separately. Failed SDK batch operations report failure and preserve the states
actually reached before the failure. The loader stops on that result.

Host-owned allocations use native pages. Checked reads hold the memory lock through the copy, while the loader's
direct module writes rely on single-threaded execution before sealing. `plat_mem_peek` follows the same read
permissions. Protected `plat_mem_poke` is unavailable. Write tracking (`plat_mem_written`, shared via `prot_watch.c`)
protects queried committed RW pages read-only at the 16 KiB native granularity and reports written 4 KiB subpages;
`plat_mem_populated` reports committed 64 KiB backing units. The build requires the loader-only gate until translated
access leases, host pointer lifetimes, and GPU-visible mappings are integrated.
