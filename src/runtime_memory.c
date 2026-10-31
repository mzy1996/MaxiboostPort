/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include "platform.h"
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#else
#include <windows.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB sets it: the
 * resolution patches above 1080p need about 4 GiB more (run.sh sets it), and a machine
 * with little memory can ask for less (the launcher's "Game memory" setting does). The
 * pool is an address range, not memory the game has to fill: on a machine with little
 * memory a smaller pool means a smaller reservation. */
#define DMEM_MIN_MB 512
#define DMEM_MAX_MB 65536
/* The memory bookkeeping says what it does when it is asked to (BB_MEMORY_TRACE=1): the
 * game's own work fills the output otherwise. */
static int memory_trace(void) {
    static int trace=-1;
    if (trace<0) {
        const char *t=getenv("BB_MEMORY_TRACE");
        trace=t&&t[0]=='1';
    }
    return trace;
}
/* The bookkeeping of the memory goes to a file of its own: the runtime's output and
 * the game's are mixed up on the terminal, and the bookkeeping has to be read as it
 * is. */
static FILE *memory_log(void) {
    static FILE *log;
    if (!log) log=fopen("memory.txt","w");
    return log;
}
/* bbport: a window of the guest's memory is watched (diagnostics). The probe hands the registry's
 * node pool over once the game has made it (runtime_memory_watch_range): the pool is the game's
 * own operator-new block inside a direct-memory region the game itself owns, so the runtime's own
 * hand in its emptying has to be told apart from the game's allocator doing it. Every mapping
 * operation that lands on the window is said - a mapping placed over it, an unmapping, a release
 * of the physical memory behind it, a protection change, a write through the backing view. */
static uintptr_t watch_va_lo, watch_va_hi;
static uint64_t watch_phys_lo, watch_phys_hi;
static int watch_on;
/* bbport: a ring of every mapping operation (place / unmap / release / protect / write-backing),
 * with the guest call sites, so the pool watch can ask what touched the pool's range even after
 * the fact. watch_note's static phys-window is taken from the pool's ORIGINAL phys and misses a
 * remap to a different phys, so an unconditional log is needed to catch that case. */
#define MEMOP_RING 512
static struct { const char *what; int is_phys; uint64_t start, size; uint64_t sites[3]; } memop_ring[MEMOP_RING];
static _Atomic uint64_t memop_seq;
static void memop_record(const char *what, int is_phys, uint64_t start, uint64_t size, uint64_t *sites) {
    const uint64_t s=atomic_fetch_add(&memop_seq,1)%MEMOP_RING;
    memop_ring[s].what=what; memop_ring[s].is_phys=is_phys;
    memop_ring[s].start=start; memop_ring[s].size=size;
    memop_ring[s].sites[0]=sites?sites[0]:0; memop_ring[s].sites[1]=sites?sites[1]:0;
    memop_ring[s].sites[2]=sites?sites[2]:0;
}
int runtime_memory_op_dump(char *buf, size_t n) {
    uint64_t head=atomic_load(&memop_seq);
    int w=0;
    for (uint64_t k=0;k<MEMOP_RING && (size_t)w<n-1;++k) {
        const uint64_t idx=(head-1-k)%MEMOP_RING;
        const char *what=memop_ring[idx].what;
        if (!what) continue;
        w+=snprintf(buf+w,n-w,"  %s %s %#llx+%#llx from +%#llx < +%#llx < +%#llx\n",
                    what, memop_ring[idx].is_phys?"phys":"va",
                    (unsigned long long)memop_ring[idx].start,(unsigned long long)memop_ring[idx].size,
                    (unsigned long long)memop_ring[idx].sites[0],
                    (unsigned long long)memop_ring[idx].sites[1],
                    (unsigned long long)memop_ring[idx].sites[2]);
    }
    return w;
}
static void watch_note(const char *what, int is_phys, uint64_t start, uint64_t size) {
    if (!watch_on) return;
    uint64_t sites[3]={0,0,0};
    runtime_guest_call_sites(sites);
    memop_record(what,is_phys,start,size,sites);   /* unconditional: a remap to a different phys would miss the window */
    const uint64_t end=start+size;
    if (is_phys ? !(end>watch_phys_lo && start<watch_phys_hi)
                : !(end>watch_va_lo && start<watch_va_hi)) return;
    fprintf(stderr,"Runtime: WATCH %s %s %#llx+%#llx of %#llx..%#llx from +%#llx < +%#llx < +%#llx\n",
            what,is_phys?"phys":"va",(unsigned long long)start,(unsigned long long)size,
            (unsigned long long)(is_phys?watch_phys_lo:watch_va_lo),
            (unsigned long long)(is_phys?watch_phys_hi:watch_va_hi),
            (unsigned long long)sites[0],(unsigned long long)sites[1],(unsigned long long)sites[2]);
    fflush(stderr);
}
/* bbport: the probe is told where a mapping was placed (diagnostics): the pool's page must be
 * watched from the instant it exists, not from the instant the game publishes its base, or the
 * write that recycles it is over before the watch is on. The hook runs with the runtime's write
 * lock held, so it must not ask the runtime anything. */
static void (*hook_place)(uintptr_t address, uint64_t size, uint64_t phys);
void runtime_memory_set_place_hook(void (*hook)(uintptr_t address, uint64_t size, uint64_t phys)) {
    hook_place=hook;
}
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("BB_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < DMEM_MIN_MB || mb > DMEM_MAX_MB) mb = 6144;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
/* The host's sections are mapped at the host's granularity (64 KiB on Windows), so the
 * flexible memory is placed at such a boundary. */
#ifdef _WIN32
#define FLEX_GRAN (UINT64_C(64) * 1024)
#else
#define FLEX_GRAN PAGE
#endif
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
/* The blocks of the direct memory: the pool is 5056 MiB and the game takes it in blocks
 * of a megabyte (it holds them all and maps the ones it needs), so the table has to hold
 * as many blocks as the pool can be divided into. */
#define LIMIT 65536
/* The R1 pool's guest (==host) virtual address and the region around it, for the
 * "POOL HIT" diagnostic that flags any read/write into it. */
#define R1_POOL_VA_LO UINT64_C(0x800410000)
#define R1_POOL_VA_HI UINT64_C(0x800810000)
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
static void write_lock(void) {
    if (!exclusive_depth++) { pthread_rwlock_wrlock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
}
static void read_lock(void) { if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
static void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); }
/* The blocks of the direct memory, in order of their address (the game takes and gives
 * back a great many of them, and the table is looked at every time: the order keeps
 * the lookups short). Only the blocks that are in use are in the list. */
static Block blocks[LIMIT];
static int block_count;

/* The first block whose end is after the given address (the blocks are in order). */
static int block_first_ending_after(uint64_t phys) {
    int lo=0, hi=block_count;
    while (lo<hi) {
        const int mid=(lo+hi)/2;
        if (blocks[mid].start+blocks[mid].size>phys) hi=mid; else lo=mid+1;
    }
    return lo;
}
/* The block that holds the given address (the blocks are in order). */
static int block_holding(uint64_t phys) {
    const int i=block_first_ending_after(phys);
    if (i<block_count && blocks[i].start<=phys) return i;
    return -1;
}
static Vma *vmas; static size_t vma_count, vma_capacity;
static int pool_fd=-1;
static int flex_fd=-1;
/* bbport: direct memory in GPU-visible system memory the GPU library allocates (dma-buf chunks,
 * BB_GUEST_GPU_MEMORY=1): the GPU reads the game's data in place instead of the CPU copying it
 * into staging buffers first. Chunks are allocated when the game allocates direct memory in them;
 * a chunk the GPU library cannot provide stays in the memfd. */
#define CHUNK (UINT64_C(256) << 20)
typedef int (*GuestChunkAlloc)(uint64_t phys, uint64_t size);
static GuestChunkAlloc chunk_alloc;
static int *chunk_fds; /* per CHUNK of direct memory: -1 not decided, -2 memfd, -3 memfd the GPU
                        * imported (host memory: VK_EXT_external_memory_host), else a dma-buf fd */
#define CHUNK_IMPORTED (-3)
#define CHUNK_PENDING (-4) /* the GPU's memory for it is made when the GPU asks for it */
/* Released memory in a chunk the GPU uses is cleared, not punched out: the GPU holds those pages. */
#define GPU_CHUNK(fd) ((fd)>=0 || (fd)==CHUNK_IMPORTED)
/* Drivers whose dma-buf maps at offset 0 only (NVIDIA): a chunk per direct allocation instead of
 * the CHUNK grid. The game maps each of its allocations once, whole, so its mappings and the
 * backing view start at offset 0 of their chunk. */
#ifndef _WIN32
static unsigned char *backing_base; /* non-Windows: whole-pool host alias (second view) */
#endif
#ifdef _WIN32
/* Windows: the pool is host-aliased per 256 MiB chunk (backing_views[c]) because a single
 * multi-GiB MapViewOfFile intermittently fails to find a contiguous hole once the many
 * loaded DLLs fragment the address space (which made pool_fd creation fail -> libc heap abort). */
static unsigned char **backing_views; static size_t backing_nchunks;
#endif
/* Forward declaration: phys_backing_ptr is defined further below but used early (add_region, pool). */
static unsigned char *phys_backing_ptr(uint64_t phys);
static uint64_t chunk_size(uint64_t c);
/* bbport: the R1 registry node pool is moved into its own dedicated section (r1_pool_fd) so that
 * neither the GPU's in-place import of direct-memory chunks (HostImport aliases the shared pool's
 * physical pages in place) nor any discard/zero of that shared section can reach the live pool.
 * Detected once the game publishes the pool's base (image+0x161d080). See
 * runtime_memory_protect_r1_pool. */
static int r1_pool_fd=-1;
static unsigned char *r1_pool_base;
static uint64_t r1_pool_phys, r1_pool_size;
static uint64_t r1_pool_off; /* offset of the pool's first byte within r1_pool_fd (0x40: the pool
                                starts 0x40 past the aligned base of the direct region) */
static int r1_pool_active;
static int whole_chunks;
static int lazy_chunks; /* the GPU's memory for a chunk is made when the GPU asks for it */
typedef struct { uint64_t phys, size; int fd; } Region;
#define MAX_REGIONS 256
static Region regions[MAX_REGIONS];
static size_t region_count;
/* The region holding phys (sorted by phys), or NULL; *next: start of the next region above. */
static const Region *region_at(uint64_t phys, uint64_t *next) {
    *next=POOL_SIZE;
    for (size_t i=0;i<region_count;++i) {
        if (phys<regions[i].phys) { *next=regions[i].phys; return NULL; }
        if (phys<regions[i].phys+regions[i].size) return &regions[i];
    }
    return NULL;
}
static void add_region(uint64_t phys, uint64_t size) {
    if (region_count==MAX_REGIONS) return;
    int fd=chunk_alloc(phys,size);
    if (fd>=0 && !platform_section_map(fd,phys_backing_ptr(phys),size,0,BB_PROT_READ|BB_PROT_WRITE)) {
        platform_section_release(fd);
        fd=-1;
    }
    size_t i=region_count;
    while (i>0 && regions[i-1].phys>phys) { regions[i]=regions[i-1]; --i; }
    regions[i]=(Region){phys,size,fd>=0 || fd==CHUNK_IMPORTED ? fd : -2};
    ++region_count;
}
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
static GpuRange hook_note_write; /* bbport: data written into GPU memory (runtime_memory_note_write) */
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static uint64_t align_down(uint64_t n, uint64_t a) { return n & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? BB_PROT_READ : 0) | ((prot & 0x22) ? BB_PROT_WRITE|BB_PROT_READ : 0) | ((prot & 4) ? BB_PROT_EXEC|BB_PROT_READ : 0);
}
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    /* pool_fd backs direct memory only ([0,POOL_SIZE)); flexible memory is its own section
     * (flex_fd). Sizing it POOL_SIZE+FLEX_SPAN wastes a GiB of pagefile-backed commit charge. */
    if (platform_section_create(&pool_fd,(size_t)POOL_SIZE,"bb-guest-memory")) return -1;
#ifdef _WIN32
    /* Host-alias the pool per 256 MiB chunk: no multi-GiB contiguous view is needed, so pool_fd
     * creation no longer depends on finding a large free hole in the (DLL-fragmented) address space. */
    backing_nchunks=(POOL_SIZE+CHUNK-1)/CHUNK;
    backing_views=calloc(backing_nchunks,sizeof(*backing_views));
    if (!backing_views) return -1;
    for (size_t c=0;c<backing_nchunks;++c) {
        const uint64_t cs=chunk_size(c);
        if (!cs) continue;
        unsigned char *v=platform_section_map(pool_fd,NULL,(size_t)cs,c*CHUNK,BB_PROT_READ|BB_PROT_WRITE);
        if (!v) fprintf(stderr,"Runtime: backing chunk %zu (phys %#" PRIx64 ") map failed\n",c,c*CHUNK);
        backing_views[c]=v;
    }
#else
    void *view=platform_section_backing(pool_fd,(size_t)(POOL_SIZE+FLEX_SPAN));
    if (!view) return -1;
    backing_base=view;
#endif
    if (platform_section_create(&flex_fd,(size_t)FLEX_SPAN,"bb-guest-flexible")) return -1;
    chunk_fds=malloc((POOL_SIZE/CHUNK+1)*sizeof(int));
    if (!chunk_fds) return -1;
    for (uint64_t c=0;c<=POOL_SIZE/CHUNK;++c) chunk_fds[c]=-1;
    return 0;
}
/* Size of direct memory chunk c (the last one may be shorter). */
static uint64_t chunk_size(uint64_t c) {
    const uint64_t start=c*CHUNK;
    return POOL_SIZE-start<CHUNK ? POOL_SIZE-start : CHUNK;
}
/* Decides where the chunks of [phys, phys+size) of direct memory live (lock held). */
static void ensure_chunks(uint64_t phys, uint64_t size) {
    if (!chunk_fds || phys>=POOL_SIZE) return;
    if (whole_chunks) {
        if (!chunk_alloc) return;
        const uint64_t end=phys+size<POOL_SIZE ? phys+size : POOL_SIZE;
        for (uint64_t at=phys;at<end;) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            if (r) { at=r->phys+r->size; continue; }
            const uint64_t gap_end=next<end ? next : end;
            add_region(at,gap_end-at);
            at=gap_end;
        }
        return;
    }
    const uint64_t last=(phys+size-1<POOL_SIZE ? phys+size-1 : POOL_SIZE-1)/CHUNK;
    for (uint64_t c=phys/CHUNK;c<=last;++c) {
        if (chunk_fds[c]!=-1) continue;
        if (lazy_chunks) {
            /* The GPU's memory for this chunk is made when the GPU asks for it: the game
             * takes the whole pool at the start, and the GPU needs only the parts it
             * really uses (making the memory for all of them at once is slow and takes
             * the address space of the device for nothing). */
            chunk_fds[c]=CHUNK_PENDING;
            continue;
        }
        int fd=chunk_alloc ? chunk_alloc(c*CHUNK,chunk_size(c)) : -1;
        if (fd>=0 && !platform_section_map(fd,phys_backing_ptr(c*CHUNK),chunk_size(c),0,BB_PROT_READ|BB_PROT_WRITE)) {
            platform_section_release(fd);
            fd=-1;
        }
        chunk_fds[c]=fd>=0 || fd==CHUNK_IMPORTED ? fd : -2;
    }
}
/* mmap of direct or flexible memory at phys, split where it crosses chunks. */
static void *map_phys(uintptr_t address, uint64_t size, int prot, uint64_t phys) {
    /* bbport: the R1 registry pool lives in its own section (runtime_memory_protect_r1_pool
     * moved it there); map it from that section so the GPU's import of the shared chunk and any
     * discard of that chunk cannot reach the live pool. The pool is small and contiguous, so a
     * single view covers it. */
    if (r1_pool_active && phys>=r1_pool_phys && phys+size<=r1_pool_phys+r1_pool_size) {
        const uint64_t off=(phys-r1_pool_phys)+r1_pool_off;
        void *m=platform_section_map(r1_pool_fd,(void *)address,size,off,prot);
        if (m) return m;
        return platform_section_map(r1_pool_fd,NULL,size,off,prot);
    }
    /* The flexible memory is in the section of its own; the direct memory in the pool's.
     * The offset in the section is the place in the pool, not in the section. */
    const int section = phys>=POOL_SIZE ? flex_fd : pool_fd;
    const uint64_t offset = phys>=POOL_SIZE ? phys-POOL_SIZE : phys;
#ifdef _WIN32
    /* The address the game asked for comes first: the game puts its things at known
     * places, and a mapping elsewhere is a mapping it cannot use. Only when that
     * address cannot be mapped (it is full of the host's own mappings) does the host
     * pick one itself. */
    void *asked=platform_section_map(section,(void *)address,size,offset,prot);
    if (asked) return asked;
    return platform_section_map(section,NULL,size,offset,prot);
#else
    if (!chunk_fds || phys>=POOL_SIZE)
        return platform_section_map(section,(void *)address,size,offset,prot);
    ensure_chunks(phys,size);
    for (uint64_t done=0;whole_chunks && done<size;) {
        const uint64_t at=phys+done;
        uint64_t next;
        const Region *r=region_at(at,&next);
        const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
        void *p = r && r->fd>=0
            ? platform_section_map(r->fd,(void *)(address+done),n,at-r->phys,prot)
            : platform_section_map(pool_fd,(void *)(address+done),n,at,prot);
        if (!p) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,r && r->fd>=0 ? "a GPU-visible dma-buf (whole-allocation chunk)" : "the memfd",strerror(errno));
            return (void *)-1;
        }
        done+=n;
        if (done==size) return (void *)address;
    }
    uintptr_t mapped_at=0;
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done, c=at/CHUNK;
        const uint64_t room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        void *p = chunk_fds[c]>=0
            ? platform_section_map(chunk_fds[c],(void *)(address+done),n,at-c*CHUNK,prot)
            : platform_section_map(pool_fd,(void *)(address+done),n,at,prot);
#ifdef _WIN32
        if (!p) {
            /* The range the game asks for is full of the host's own mappings (Windows):
             * the host picks the address itself. The whole range is mapped at once, so
             * that it is one range. */
            p=platform_section_map(pool_fd,NULL,size,at,prot);
            if (p) done=size;
        }
#endif
        if (!p) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,chunk_fds[c]>=0 ? "a GPU-visible dma-buf chunk" : "the memfd",strerror(errno));
            return (void *)-1;
        }
        if (!mapped_at) mapped_at=(uintptr_t)p;
        done+=n;
    }
    /* The address the mapping has: the one that was asked for, or the one the host gave
     * (Windows). */
    return (void *)mapped_at;
#endif
}
/* Returns released physical memory to zero: hole punching in the memfd, a clear in a chunk. */
static void zero_phys(uint64_t phys, uint64_t size) {
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done;
        if (!chunk_fds || at>=POOL_SIZE) {
            platform_section_punch_hole(pool_fd,at,(size_t)(size-done));
            return;
        }
        if (whole_chunks) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
            if (r && GPU_CHUNK(r->fd)) memset(phys_backing_ptr(at),0,n);
            else platform_section_punch_hole(pool_fd,at,(size_t)n);
            done+=n;
            continue;
        }
        const uint64_t c=at/CHUNK, room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        if (GPU_CHUNK(chunk_fds[c])) memset(phys_backing_ptr(at),0,n);
        else platform_section_punch_hole(pool_fd,at,(size_t)n);
        done+=n;
    }
}
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    const uint64_t units=(size+FLEX_GRAN-1)/FLEX_GRAN, total=FLEX_SPAN/FLEX_GRAN;
    uint64_t run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==units) { flex_set(p+1-units,units,1); return POOL_SIZE+(p+1-units)*FLEX_GRAN; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    const uint64_t units=(size+FLEX_GRAN-1)/FLEX_GRAN;
    flex_set((phys-POOL_SIZE)/FLEX_GRAN,units,0);
    platform_section_punch_hole(flex_fd,phys-POOL_SIZE,(size_t)(units*FLEX_GRAN));
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    /* Flexible memory too (bbport): its pages are released by offset (flex_free). */
    if (right.kind!=KIND_RESERVED) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in [from, limit). */
static uintptr_t find_free_in(uintptr_t from, uintptr_t limit, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(from,alignment);
    for (size_t i=vma_index(at); at+size<=limit; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
/* Where the PS4 puts a mapping the game does not place: below the user range, from SYSTEM_MIN
 * (shadPS4's DEFAULT_MAPPING_BASE), up to the host-owned memory at LOW_MIN. The user range from
 * USER_MIN stays the game's: GUNDAM VERSUS places its own blocks there with MAP_FIXED|NO_OVERWRITE
 * from 0x1000000000 up, and libc's first heap block or a flexible mapping there made them fail.
 * A hint inside the user range is searched from the hint, as before. */
#ifdef _WIN32
/* The host's own mappings fill the ranges below the user's range (Windows), so the
 * runtime's own pool goes above it (the game puts its own things in the user's range,
 * from 64 gigabytes up, and the two must not meet).
 * The pool must stay inside the range the game's library accepts: its mmap refuses
 * anything above 0xfbffc3fffff, and Windows cannot map a view above its own user
 * address space either (the host's choice then lands outside the game's range and the
 * library turns it into a failure). 0x800000000 (512 gigabytes) is inside both. */
#define SYSTEM_MIN UINT64_C(0x800000000)
#define LOW_MIN UINT64_C(0x40000000000)
#define FLEXIBLE_MIN UINT64_C(0x800000000)
#else
#define SYSTEM_MIN UINT64_C(0x0200000000)
#define LOW_MIN UINT64_C(0x0800000000)
#endif
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    if (hint<USER_MIN) {
        const uintptr_t at=find_free_in(hint<SYSTEM_MIN ? SYSTEM_MIN : hint,LOW_MIN,size,alignment);
        if (at) return at;
        hint=USER_MIN;
    }
    return find_free_in(hint,USER_MAX,size,alignment);
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* bbport: write traps (runtime_memory_trap, below), kept across the game's own changes. */
static void trap_reapply(uintptr_t start, uintptr_t end);
static void trap_forget(uintptr_t start, uintptr_t end);
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) {
            for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i)
                printf("Runtime: %#" PRIx64 "..%#" PRIx64 " is taken (kind %d, phys %#" PRIx64 ")\n",
                       (uint64_t)vmas[i].start,(uint64_t)vmas[i].end,vmas[i].kind,vmas[i].phys);
            return NO_MEMORY;
        }
    } else {
#ifdef _WIN32
        if (kind==KIND_FLEXIBLE && address<FLEXIBLE_MIN) address=FLEXIBLE_MIN;
#endif
        address=find_free(address,size,alignment);
        if (!address) {
#ifdef _WIN32
            /* The ranges the game asks for are full of the host's own mappings (Windows):
             * the host's choice of the address is used (the mapping is made where the
             * host can make it). */
            address=0;
#else
            return NO_MEMORY;
#endif
        }
    }
    void *mapped = kind!=KIND_RESERVED
        ? map_phys(address,size,host_prot(prot),phys)
        : platform_reserve_fixed((void *)address,size);
    if (!mapped) return NO_MEMORY;
    if ((uintptr_t)mapped!=address) {
        /* The host gave the address (Windows: the range the game asks for is full of the
         * host's own mappings), so the range is the one it gave. */
        address=align_down((uintptr_t)mapped,PAGE);
        size=align_up((uintptr_t)mapped+size-address,PAGE);
    }
    drop_range(address,address+size);
    trap_forget(address,address+size); /* a new mapping starts without traps */
    watch_note("place",0,address,size);
    if (phys) watch_note("place-phys",1,phys,size);
    if (kind==KIND_DIRECT && hook_place) hook_place(address,size,phys);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    const int i=block_holding(phys);
    return i<0 ? -1 : blocks[i].type;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    /* The blocks are in order, so the range is allocated when the blocks cover it
     * without a gap. */
    uint64_t at=phys;
    while (at<phys+size) {
        const int i=block_first_ending_after(at);
        if (i>=block_count || blocks[i].start>at) return 0;
        at=blocks[i].start+blocks[i].size;
    }
    return 1;
}

/* The direct memory the game may use: the whole pool. The real machine keeps a part of
 * the physical memory for itself, but that is a property of the console's memory, not of
 * the game's budget: the game is told the pool's size and may take all of it. */
static ABI uint64_t direct_size(void) {
    return POOL_SIZE;
}
/* What the guest asks for, by size (a power of two bucket). The game takes its own pools
 * in one megabyte blocks and the library's heap in four megabyte ones; when the two
 * compete for the same pool, this says which one gets the memory and which does not. */
static _Atomic uint64_t direct_asked[24], direct_failed[24], direct_live_by_bucket[24];
static int size_bucket(uint64_t size) {
    int bucket=0;
    while (size>1 && bucket<23) { size>>=1; ++bucket; }
    return bucket;
}
static void direct_note(uint64_t size, uint64_t alignment, int failed) {
    const int bucket=size_bucket(size);
    atomic_fetch_add(&direct_asked[bucket],1);
    if (failed) atomic_fetch_add(&direct_failed[bucket],1);
}
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10) {
        fprintf(stderr,"Runtime:   direct_allocate INVALID: out=%p low=%lld high=%lld size=%llu alignment=%llu (PAGE=%llu, valid_align=%d) type=%d\n",
                (void *)out,(long long)low,(long long)high,(unsigned long long)size,
                (unsigned long long)alignment,(unsigned long long)PAGE,(int)(alignment>=PAGE&&alignment<=POOL_SIZE&&!(alignment&(alignment-1))),
                type);
        direct_note(size,alignment,1);
        return INVALID;
    }
    /* The range the game asks for is where the block is looked for: the search never
     * leaves it (shadPS4's MemoryManager::Allocate: the block must end before the
     * search's end). The game asks for the whole direct memory and takes it in blocks;
     * its own heap is in it too. */
    const uint64_t end=(uint64_t)high<POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) {
        fprintf(stderr,"Runtime:   the allocation of %llu bytes did not fit (low=%lld high=%lld)\n",
                (unsigned long long)size,(long long)low,(long long)high);
        direct_note(size,alignment,1);
        return NO_SPACE;
    }
    write_lock();
    if (pool()) { fprintf(stderr,"Runtime:   direct_allocate NO_MEMORY: pool_fd not ready (pool() failed) for size=%llu low=%lld high=%lld\n",
                         (unsigned long long)size,(long long)low,(long long)high);
                  write_unlock(); direct_note(size,alignment,1); return NO_MEMORY; }
    if (block_count>=LIMIT) { fprintf(stderr,"Runtime:   direct_allocate NO_SPACE: block_count=%d>=LIMIT for size=%llu\n",
                                     block_count,(unsigned long long)size);
                              write_unlock(); direct_note(size,alignment,1); return NO_SPACE; }
    uint64_t start = align_up((uint64_t)low, alignment);
    /* The lowest free gap of the needed size at or after the given address: the blocks
     * are in order, so one pass over them says where the gaps are (the game asks for
     * a great many of these, and the pass is what makes that bearable). The gap a
     * block goes into is the one before the block that follows it, and the block has
     * to fit there after the alignment: a gap that is big enough but not at an
     * aligned place is passed over, not wasted. */
    {
        int i=block_first_ending_after((uint64_t)low);
        uint64_t gap_end = i<block_count ? blocks[i].start : end;
        for (;;) {
            const uint64_t aligned = align_up(start,alignment);
            if (aligned+size<=gap_end) { start=aligned; break; }
            if (i>=block_count) break;
            start=align_up(blocks[i].start+blocks[i].size,alignment);
            ++i;
            gap_end = i<block_count ? blocks[i].start : end;
            if (start+size>end) break;
        }
    }
    int taken=0;
    {   /* The place must be free: a block that is already there belongs to another
         * one, and the game would write over it. */
        for (int i=0;i<block_count;++i) {
            const Block b=blocks[i];
            if (start < b.start+b.size && b.start < start+size) {
                fprintf(stderr,"Runtime:   the place %#llx+%llu is taken by the block %#llx+%llu\n",
                        (unsigned long long)start,(unsigned long long)size,
                        (unsigned long long)b.start,(unsigned long long)b.size);
                taken=1;
                break;
            }
        }
    }
    if (start > end || size > end-start || taken) {
        /* No room in the range the game asked for: the real machine says "try again
         * later" (EAGAIN), not that there is no memory at all. */
        static int reported;
        if (reported++ < 4)
            fprintf(stderr,"Runtime:   no room for %llu bytes (start %#llx; asked low=%lld high=%lld align=%llu)\n",
                    (unsigned long long)size,(unsigned long long)start,
                    (long long)low,(long long)high,(unsigned long long)alignment);
        write_unlock(); direct_note(size,alignment,1); return NO_SPACE;
    }
    {   /* The block goes into the list, which keeps its order. */
        int at=block_first_ending_after(start);
        while (at>0 && blocks[at-1].start+blocks[at-1].size>start) --at;
        memmove(&blocks[at+1],&blocks[at],(size_t)(block_count-at)*sizeof(Block));
        blocks[at]=(Block){start, size, type, 1};
        ++block_count;
    }
    *out = (int64_t)start;
    ensure_chunks(start,size);
    ++allocations; live_bytes += size;
    direct_note(size,alignment,0);
    atomic_fetch_add(&direct_live_by_bucket[size_bucket(size)],size);
    write_unlock();
    if (memory_trace()) {
        /* The game calls this function itself, so the return address says where in the
         * game the call comes from. */
        const uintptr_t caller=(uintptr_t)__builtin_return_address(0);
        FILE *log=memory_log();
        if (log) {
            fprintf(log,"A %#" PRIx64 " %" PRIu64 " %d %p\n",start,size,type,(void *)caller);
            fflush(log);
        }
    }
    return 0;
}
/* The largest free direct memory in [low, high) at the alignment: where it starts and its size
 * (GUNDAM VERSUS sizes its pools by it). */
static ABI int32_t direct_available(int64_t low, int64_t high, uint64_t alignment, int64_t *phys_out, uint64_t *size_out) {
    if (memory_trace())
        fprintf(stderr,"Runtime:   available asked: low=%lld high=%lld align=%llu\n",
                (long long)low,(long long)high,(unsigned long long)alignment);
    if (!alignment) alignment=PAGE;
    if (!phys_out || !size_out || low<0 || high<=low || !valid_alignment(alignment)) return INVALID;
    const uint64_t end=(uint64_t)high<POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    uint64_t best_start=0, best=0;
    read_lock();
    {   /* The largest free stretch: the blocks are in order, so one pass over them
         * finds the gaps between them. */
        uint64_t at=align_up((uint64_t)low,alignment);
        for (int i=block_first_ending_after((uint64_t)low); i<=block_count; ++i) {
            const uint64_t next_used = i<block_count ? blocks[i].start : end;
            if (next_used>at && next_used-at>best) { best=next_used-at; best_start=at; }
            if (i>=block_count) break;
            const uint64_t used_end=blocks[i].start+blocks[i].size;
            at=align_up(used_end>at ? used_end : at,alignment);
            if (at>=end) break;
        }
    }
    read_unlock();
    if (!best) return NO_MEMORY;
    if (memory_trace())
        fprintf(stderr,"Runtime:   available answer: %llu bytes at %#llx (in use: %d blocks, %llu bytes)\n",
                (unsigned long long)best,(unsigned long long)best_start,block_count,
                (unsigned long long)live_bytes);
    *phys_out=(int64_t)best_start; *size_out=best;
    return 0;
}
static ABI int32_t flexible_available(uint64_t *size) {
    if (!size) return INVALID;
    *size=FLEXIBLE_SIZE-flexible_bytes;
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    /* The same physical memory at a second address (GPU write tracking sees one address only). */
    if (!result) {
        const uintptr_t at=(uintptr_t)*out;
        for (size_t i=0;i<vma_count;++i) {
            const Vma *v=&vmas[i];
            if (v->kind!=KIND_DIRECT || v->start==at) continue;
            const uint64_t vphys_end=v->phys+(v->end-v->start);
            if (v->phys<(uint64_t)physical+size && (uint64_t)physical<vphys_end)
                printf("Runtime: alias: direct memory phys %#" PRIx64 "+%#" PRIx64 " at %#" PRIx64
                       " (prot %#x) is also mapped at %#" PRIx64 "..%#" PRIx64 " (phys %#" PRIx64 ", prot %#x)\n",
                       (uint64_t)physical,size,(uint64_t)at,(unsigned)prot,(uint64_t)v->start,(uint64_t)v->end,
                       v->phys,(unsigned)v->prot);
        }
    }
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    void *const asked=out ? *out : NULL;
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) {
        if (memory_trace())
            printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    }
    else printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " at %p (flags %#x, alignment %#" PRIx64 ") failed (error %#x)\n",
                (uint64_t)physical, size, asked, (unsigned)flags, alignment, (unsigned)result);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        watch_note("unmap",0,a,b-a);
        platform_unmap((void *)a,b-a);
    }
    drop_range(start,end);
    trap_forget(start,end);
    return 0;
}
/* The guest's mappings: what the game sees of its own memory. */
void runtime_report_maps(FILE *out) {
    for (size_t i=0;i<vma_count;++i)
        fprintf(out,"Runtime: map %#" PRIx64 "..%#" PRIx64 " kind %d prot %#x phys %#" PRIx64 "\n",
                (uint64_t)vmas[i].start,(uint64_t)vmas[i].end,vmas[i].kind,vmas[i].prot,vmas[i].phys);
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static size_t released;
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    if (memory_trace()) {
        FILE *log=memory_log();
        if (log) {
            fprintf(log,"R %#" PRIx64 " %" PRIu64 "\n",(unsigned long long)start,(unsigned long long)size);
            fflush(log);
        }
    }
    watch_note("release",1,start,size);
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    {   /* The blocks that lie in the released range leave the list; the parts of the
         * first and the last of them that are outside the range stay (the blocks are
         * in order, so the ones that lie in the range are next to each other). */
        const int first=block_first_ending_after(start);
        int last=first;
        while (last<block_count && blocks[last].start<start+size) ++last;
        if (last>first) {
            uint64_t released_now=0;
            for (int i=first;i<last;++i) {
                const uint64_t a=blocks[i].start>start ? blocks[i].start : start;
                const uint64_t e=blocks[i].start+blocks[i].size<start+size ? blocks[i].start+blocks[i].size : start+size;
                released_now+=e-a;
            }
            live_bytes-=released_now;
            for (int i=first;i<last;++i) {
                const uint64_t a=blocks[i].start>start ? blocks[i].start : start;
                const uint64_t e=blocks[i].start+blocks[i].size<start+size ? blocks[i].start+blocks[i].size : start+size;
                atomic_fetch_sub(&direct_live_by_bucket[size_bucket(blocks[i].size)],e-a);
            }
            /* The parts that stay. */
            Block pieces[2]; int piece_count=0;
            if (blocks[first].start<start)
                pieces[piece_count++]=(Block){blocks[first].start,start-blocks[first].start,
                                              blocks[first].type,1};
            if (blocks[last-1].start+blocks[last-1].size>start+size)
                pieces[piece_count++]=(Block){start+size,
                                              blocks[last-1].start+blocks[last-1].size-(start+size),
                                              blocks[last-1].type,1};
            memmove(&blocks[first],&blocks[last],(size_t)(block_count-last)*sizeof(Block));
            block_count-=last-first;
            for (int k=0;k<piece_count;++k) {
                const int at=first+k;
                memmove(&blocks[at+1],&blocks[at],(size_t)(block_count-at)*sizeof(Block));
                blocks[at]=pieces[k];
                ++block_count;
            }
        }
    }
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) {
        /* The game's heap grows with these: there are many of them, so the message is
         * only for the ones that are asked for (BB_MEMORY_TRACE). */
        if (memory_trace())
            printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    }
    else {
        /* The game retries until the mapping works, so the reason is said a few times
         * and then the count is kept quiet. */
        static _Atomic unsigned failures;
        const unsigned n=atomic_fetch_add(&failures,1)+1;
        if (n<=4 || !(n&(n-1)))
            fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x): %s (in use %" PRIu64 ", %u so far)\n",
                    size, (unsigned)result,
                    result==INVALID?"the request was not valid":result==NO_MEMORY?"not enough memory":
                    result==NO_SPACE?"no room":"another reason", flexible_bytes, n);
    }
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
/* POSIX mmap: the game's libc grows its malloc heap with anonymous private mappings once the
 * heap it started with is full (seen after minutes of area streaming). They come from flexible
 * memory as sceKernelMapFlexibleMemory does; files are not mapped. MAP_FAILED (-1) and errno on
 * failure, as POSIX. */
static _Atomic uint64_t heap_growths;
uint64_t runtime_heap_growths(void) { return atomic_load(&heap_growths); }
static ABI void *posix_mmap(void *addr, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    atomic_fetch_add(&heap_growths,1);
    enum { MAP_ANON_FLAG=0x1000, POSIX_ENOMEM=12, POSIX_ENODEV=19, POSIX_EINVAL=22 };
    if (!(flags & MAP_ANON_FLAG) || fd!=-1 || offset) { *runtime_errno()=POSIX_ENODEV; return (void *)-1; }
    if (!size) { *runtime_errno()=POSIX_EINVAL; return (void *)-1; }
    void *out=(flags & MAP_FIXED_FLAG) ? addr : NULL;
    const int32_t result=map_flexible(&out,align_up(size,PAGE),prot & 0x37,flags & MAP_FIXED_FLAG);
    if (result) {
        /* The game asks for more heap as it needs it; the answer is the reason it stops
         * asking, so it is said (the game retries until it works). */
        *runtime_errno()=result==NO_MEMORY ? POSIX_ENOMEM : POSIX_EINVAL;
        char line[256];
        const int length=snprintf(line,sizeof(line),
            "Runtime: mmap %#" PRIx64 " bytes for the game failed (0x%x, %s; flexible in use %" PRIu64 " of %" PRIu64 ")\n",
            size,(unsigned)result,result==NO_MEMORY?"not enough memory":"the request was not valid",
            flexible_bytes,(uint64_t)FLEXIBLE_SIZE);
        if (length>0) platform_write_stdout(line,(size_t)(length<(int)sizeof(line)?length:(int)sizeof(line)-1));
        return (void *)-1;
    }
    if (memory_trace()) {
        uint64_t sites[3];
        runtime_guest_call_sites(sites);
        printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
               size,sites[0],sites[1],sites[2]);
    }
    return out;
}
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    watch_note("protect",0,start,end-start);
    if (platform_protect((void *)start,end-start,host_prot(prot))) return INVALID;
    trap_reapply(start,end); /* pages with write traps stay read-only */
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    const int i=block_holding(phys);
    if (i>=0) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    { static int shown;
      if (shown<3) { ++shown;
        fprintf(stderr,"Runtime:   batch map of %d entries (flags %#x):",count,flags);
        for (int i=0;i<count && i<4;++i)
            fprintf(stderr," [op %d start %p len %llu prot %#x off %#lld]",
                    entries[i].operation,(void *)entries[i].start,
                    (unsigned long long)entries[i].length,(unsigned)entries[i].prot,
                    (long long)entries[i].offset);
        fprintf(stderr,"\n"); } }
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static const RuntimeExport exports[]={
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelAvailableDirectMemorySize",direct_available}, {"sceKernelAvailableFlexibleMemorySize",flexible_available},
    {"sceKernelCheckedReleaseDirectMemory",direct_release},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
    {"mmap",posix_mmap},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (runtime_nid_is(name,"pO96TwzOm5E#p#J")) return (uintptr_t)direct_size;
    if (runtime_nid_is(name,"rTXw65xmLIA#p#J")) return (uintptr_t)direct_allocate;
    if (runtime_nid_is(name,"L-Q3LEjIbgA#p#J")) return (uintptr_t)direct_map;
    if (runtime_nid_is(name,"MBuItvba6z8#p#J")) return (uintptr_t)direct_release;
    if (runtime_nid_is(name,"cQke9UuBQOk#p#J")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
    void *p=NULL;
    while (low_next+size<=USER_MIN) {
        p=platform_reserve_fixed((void *)low_next,size);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p) { platform_protect(p,size,prot); break; }
    }
    write_unlock();
    return p ? p : (void *)-1;
}
/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),
 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */
uint64_t runtime_disabled_optimizations;
/* bbport: a second number in BB_TOGGLE_FILE: temporary experiment bits (BbToggle::Experiment). */
uint64_t runtime_experiment_bits;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
#ifdef _WIN32
__thread runtime_jmp_buf *runtime_fault_recover;
#else
__thread sigjmp_buf *runtime_fault_recover;
#endif
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0, experiment=0;
        if (f) { if (fscanf(f,"%llu %llu",&value,&experiment)<1) value=0; fclose(f); }
        __atomic_store_n(&runtime_experiment_bits,(uint64_t)experiment,__ATOMIC_RELEASE);
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
/* bbport: the GPU library provides direct memory chunks (see CHUNK). */
void runtime_memory_set_guest_chunk_whole(int whole) { whole_chunks=whole; }
/* bbport: the host view of direct memory phys, routed to the R1 pool's dedicated section when phys
 * falls inside it (the pool was moved there by runtime_memory_protect_r1_pool). */
static unsigned char *phys_backing_ptr(uint64_t phys) {
    if (r1_pool_active && phys>=r1_pool_phys && phys<r1_pool_phys+r1_pool_size)
        return r1_pool_base+r1_pool_off+(phys-r1_pool_phys);
#ifdef _WIN32
    if (phys>=POOL_SIZE) return NULL;
    const size_t c=(size_t)(phys/CHUNK);
    if (c>=backing_nchunks || !backing_views[c]) return NULL;
    return backing_views[c]+(phys-c*CHUNK);
#else
    return backing_base+phys;
#endif
}
/* bbport: the host address of direct memory phys in the backing view (host memory import). */
void *runtime_memory_backing_pointer(uint64_t phys) {
    return phys_backing_ptr(phys);
}
/* bbport: move the R1 registry node pool out of the shared pool section into its own dedicated
 * section, and re-point the guest mapping there. After this the pool's bytes live only in
 * r1_pool_fd. The GPU's in-place import (HostImport) of the shared chunk aliases the shared
 * section's physical pages, which now no longer hold the pool, and any discard/zero of that shared
 * section (a MEM_RESET or a replaced mapping) cannot reach the live pool. Idempotent.
 *
 * The pool is a 3 MiB allocation that begins 0x40 bytes into the direct region's
 * allocation-granularity-aligned block (guest 0x800410040, region base 0x800410000). A section
 * view's base AND offset must each be a multiple of the allocation granularity (65536), so the
 * pool's own guest range cannot be a standalone view. We therefore move the whole aligned block
 * that contains the pool (prefix + pool + granularity tail) out of the shared section into
 * r1_pool_fd, mapped at the aligned region base, and re-map the rest of the (1 GiB) direct region
 * back from pool_fd. All offsets below are derived from the covering VMA, never hard-coded. */
int runtime_memory_protect_r1_pool(uint64_t phys, uint64_t size) {
    if (r1_pool_active) return 0;
    if (!size || phys>=POOL_SIZE || phys+size>POOL_SIZE) return -1;
    size=align_up(size,PAGE);
    const size_t gran=(size_t)platform_granularity();
    /* Find the direct-memory VMA that covers the pool (it is the whole 1 GiB region). */
    Vma *v=NULL;
    for (size_t i=0;i<vma_count;i++) {
        if (vmas[i].kind==KIND_DIRECT &&
            vmas[i].phys<=phys && phys+size<=vmas[i].phys+(vmas[i].end-vmas[i].start)) {
            v=&vmas[i]; break;
        }
    }
    if (!v) {
        fprintf(stderr,"Runtime: R1 pool protect: no direct VMA covers phys %#llx (nothing to relocate)\n",
                (unsigned long long)phys);
        fflush(stderr);
        return -1;
    }
    const uintptr_t region_start=v->start;
    const uint64_t region_phys=v->phys;
    const uintptr_t region_end=v->end;
    const uintptr_t pool_guest_va=region_start+(phys-region_phys);
    const uintptr_t aligned_base=region_start & ~((uintptr_t)gran-1);
    const uint64_t pool_off=pool_guest_va-aligned_base;            /* 0x40 */
    const uint64_t r1_size=align_up(pool_off+size,(uint64_t)gran); /* prefix+pool+gran-tail */
    if (aligned_base+pool_off!=pool_guest_va || pool_off>=r1_size) {
        fprintf(stderr,"Runtime: R1 pool protect: alignment inconsistency (base %#llx off %#llx size %#llx)\n",
                (unsigned long long)aligned_base,(unsigned long long)pool_off,(unsigned long long)r1_size);
        fflush(stderr);
        return -1;
    }
    /* Snapshot the whole aligned block (prefix + pool + granularity tail) out of the shared section
     * before it is unmapped. The shared section keeps its bytes, so this is the good state. The tail
     * past the pool is copied too so any allocation that landed in the slack reads its original bytes. */
    if (platform_section_create(&r1_pool_fd,(size_t)r1_size,"bb-r1-pool")) return -1;
    void *view=platform_section_backing(r1_pool_fd,(size_t)r1_size);
    if (!view) { platform_section_release(r1_pool_fd); r1_pool_fd=-1; return -1; }
    r1_pool_base=view;
    memcpy(r1_pool_base, phys_backing_ptr(region_phys), (size_t)r1_size);
    /* Move the mapping under the write lock so no fault lands in the gap while we swap the views. */
    write_lock();
    /* 1) Release the entire existing direct-region view (its allocation base is region_start;
     *    platform_unmap_full finds it via VirtualQuery even if region_start were unaligned). */
    int un=platform_unmap_full((void *)region_start);
    /* 2) Map the dedicated section across the pool's aligned block. */
    void *m=platform_section_map(r1_pool_fd,(void *)aligned_base,(size_t)r1_size,0,v->prot);
    int r1_ok=m?1:0;
    /* 3) Re-map the rest of the direct region from the shared section, preserving phys continuity. */
    int rest_ok=1;
    const uintptr_t remap_start=aligned_base+r1_size;
    if (region_end > remap_start) {
        const uint64_t remap_off=region_phys+(remap_start-region_start);
        void *r=platform_section_map(pool_fd,(void *)remap_start,(size_t)(region_end-remap_start),remap_off,v->prot);
        rest_ok=r?1:0;
        if (!r) fprintf(stderr,"Runtime:   R1 pool protect: remainder remap FAILED (end %#llx off %#llx err %d)\n",
                        (unsigned long long)region_end,(unsigned long long)remap_off,platform_last_error());
    }
    if (!r1_ok) fprintf(stderr,"Runtime:   R1 pool protect: r1 section map FAILED err %d\n",platform_last_error());
    /* 4) Decisive check: write a sentinel into r1_pool_fd and read it back through the guest VA.
     *    If the guest VA now reads the sentinel, the live mapping is on r1_pool_fd (isolated); if it
     *    reads the original pool head, the remap did not take and the pool is still shared. */
    uint64_t *base=(uint64_t*)r1_pool_base;
    const size_t pool_idx=(size_t)(pool_off/sizeof(uint64_t));
    uint64_t saved=base[pool_idx];
    base[pool_idx]=0xDEADBEEFCAFEBABEULL;
    uint64_t via_va=*(volatile uint64_t*)pool_guest_va;
    base[pool_idx]=saved;
    write_unlock();
    r1_pool_phys=phys; r1_pool_size=size; r1_pool_off=pool_off;
    if (r1_ok && rest_ok && via_va==0xDEADBEEFCAFEBABEULL) {
        r1_pool_active=1;
        fprintf(stderr,"Runtime: R1 pool relocated to its own section (guest %#llx phys %#llx size %#llx); "
                        "live view isolated (sentinel verified)\n",
                (unsigned long long)pool_guest_va,(unsigned long long)phys,(unsigned long long)size);
    } else {
        fprintf(stderr,"Runtime: R1 pool relocation INCOMPLETE: unmap=%d r1_ok=%d rest_ok=%d via_va=%#llx (pool still shared!)\n",
                un,r1_ok,rest_ok,(unsigned long long)via_va);
    }
    fflush(stderr);
    return 0;
}
/* bbport diagnostics: the index of the section the direct memory (the pool) lives in. The pool
 * watch asks for it so that the platform layer can say every mapping of that section out loud: a
 * mapping of it at an address the host picks is a third view of the pool's own bytes. */
int runtime_memory_pool_section(void) { return pool_fd; }
void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size)) {
    write_lock();
    chunk_alloc=alloc;
    write_unlock();
}
/* bbport: the GPU asks for the memory of a chunk of the direct memory (the game took the
 * whole pool at the start, and the GPU needs only the parts it really uses). The memory is
 * made then, not when the game allocated. */
int runtime_memory_guest_chunk_gpu(uint64_t phys, uint64_t size) {
    if (!chunk_fds || !chunk_alloc) return -1;
    const uint64_t c=phys/CHUNK;
    if (c*CHUNK+chunk_size(c)<phys+size) return -1;
    write_lock();
    int fd=chunk_fds[c];
    if (fd==CHUNK_PENDING) {
        fd=chunk_alloc(c*CHUNK,chunk_size(c));
        if (fd>=0 && !platform_section_map(fd,phys_backing_ptr(c*CHUNK),chunk_size(c),0,BB_PROT_READ|BB_PROT_WRITE)) {
            platform_section_release(fd);
            fd=-1;
        }
        chunk_fds[c]=fd>=0 || fd==CHUNK_IMPORTED ? fd : -2;
    }
    write_unlock();
    return fd>=0 || fd==CHUNK_IMPORTED ? fd : -1;
}
/* bbport: the chunks of the direct memory are made when the GPU asks for them. */
void runtime_memory_set_guest_chunk_lazy(int lazy) {
    write_lock();
    lazy_chunks=lazy;
    write_unlock();
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* bbport: the CPU is about to write [address, address+size) without the guest's code doing it
 * (a file read into it): the GPU side is told first (Rasterizer::InvalidateMemory), for the parts
 * the GPU can access, as a driver is told of a resource update. Its write tracking then has
 * nothing to catch there. */
void runtime_memory_set_note_write_hook(GpuRange note) { hook_note_write=note; }
void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    if (!hook_invalidate || !size) return;
    uintptr_t starts[8], ends[8];
    int n=0;
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size && n<8; ++i) {
        if (vmas[i].kind==KIND_RESERVED || !(vmas[i].prot & 0x30)) continue;
        starts[n]=vmas[i].start>address ? vmas[i].start : address;
        ends[n]=vmas[i].end<address+size ? vmas[i].end : address+size;
        ++n;
    }
    read_unlock();
    GpuRange hook=hook_note_write ? hook_note_write : hook_invalidate;
    for (int i=0;i<n;++i) hook(starts[i],ends[i]-starts[i]);
}
/* bbport BB_GUEST_IN_PLACE without page protection: pages whose CPU writes the GPU side must hear
 * of (a VRAM copy of them, a texture made from them), one bit per 4 KiB page below 2^40. The GPU
 * side sets and clears them where it used to protect; the writers it cannot see coming (libc
 * imports, file reads, hooked game code) check them after writing and tell it, as a driver is
 * told of a resource update. */
#define WATCH_LIMIT (1ull<<40)
static _Atomic(uint64_t) *write_watch;
static GpuRange hook_cpu_write;
void runtime_memory_set_cpu_write_hook(GpuRange hook) {
    if (!write_watch) {
        void *bits=platform_map_fixed(NULL,(size_t)(WATCH_LIMIT>>15),BB_PROT_READ|BB_PROT_WRITE);
        if (!bits) { perror("bbport: write watch bitmap"); return; }
        write_watch=bits;
    }
    hook_cpu_write=hook;
}
void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch) {
    if (!write_watch || !size || address>=WATCH_LIMIT) return;
    uint64_t first=address>>12, last=(address+size-1)>>12;
    if (last>=(WATCH_LIMIT>>12)) last=(WATCH_LIMIT>>12)-1;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (watch) atomic_fetch_or_explicit(&write_watch[word],mask,memory_order_release);
        else atomic_fetch_and_explicit(&write_watch[word],~mask,memory_order_release);
    }
}
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size) {
    if (!write_watch || !size || address>=WATCH_LIMIT || address+size>WATCH_LIMIT) return;
    const uint64_t first=address>>12, last=(address+size-1)>>12;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (atomic_load_explicit(&write_watch[word],memory_order_acquire) & mask) {
            hook_cpu_write(address,size);
            return;
        }
    }
}
/* bbport: write traps the GPU side keeps on guest pages, by reason (bits: a VRAM copy of the page,
 * an image made from it, ...). A page with any reason is read-only for the game even where its
 * own protection allows writes; the fault goes to the GPU library's handlers, and each clears
 * only its own reason. One owner for these protections: the layer's VRAM copies and the texture
 * cache trap the same pages, and one's unprotect must not drop the other's trap. Reasons from 16 up
 * trap reads too (no access: the page's data is newer elsewhere, a VRAM copy). Lock order: the
 * runtime's lock, then trap_mutex (as kernel_mprotect, which keeps the traps). */
#define TRAP_LIMIT (1ull<<40)
#define TRAP_NO_ACCESS 0xf0u
static uint8_t *trap_reasons; /* one byte per 4 KiB page below TRAP_LIMIT (reserved, not committed) */
static pthread_mutex_t trap_mutex=PTHREAD_MUTEX_INITIALIZER;
/* A page's trap level: 0 none, 1 read-only, 2 no access. */
static int trap_level(uint8_t reasons) { return !reasons ? 0 : (reasons & TRAP_NO_ACCESS) ? 2 : 1; }
/* The pages [a, b) to the game's protection, without write at level 1, without access at 2
 * (runtime lock held). */
static void trap_apply(uintptr_t a, uintptr_t b, int level) {
    for (size_t i=vma_index(a); i<vma_count && vmas[i].start<b; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        const uintptr_t s=vmas[i].start>a ? vmas[i].start : a, e=vmas[i].end<b ? vmas[i].end : b;
        const int prot=host_prot(vmas[i].prot);
        platform_protect((void *)s,e-s,level==2 ? BB_PROT_NONE : level ? (prot & ~BB_PROT_WRITE) : prot);
    }
}
/* No traps in [start, end) any more: a mapping replaced or gone (runtime lock held). */
static void trap_forget(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || start>=TRAP_LIMIT) return;
    if (end>TRAP_LIMIT) end=TRAP_LIMIT;
    pthread_mutex_lock(&trap_mutex);
    memset(trap_reasons+(start>>12),0,((end+4095)>>12)-(start>>12));
    pthread_mutex_unlock(&trap_mutex);
}
/* Pages with traps in [start, end) read-only (or no access) again after the game changed their
 * protection (runtime lock held). */
static void trap_reapply(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || start>=TRAP_LIMIT) return;
    if (end>TRAP_LIMIT) end=TRAP_LIMIT;
    pthread_mutex_lock(&trap_mutex);
    uintptr_t run=0; int run_level=0;
    for (uintptr_t p=start>>12; p<(end+4095)>>12; ++p) {
        const int level=trap_level(trap_reasons[p]);
        if (run && level!=run_level) { trap_apply(run,p<<12,run_level); run=0; }
        if (level && !run) { run=p<<12; run_level=level; }
    }
    if (run) trap_apply(run,end,run_level);
    pthread_mutex_unlock(&trap_mutex);
}
void runtime_memory_trap(uintptr_t address, uint64_t size, unsigned reason, int on) {
    if (!size || address>=TRAP_LIMIT) return;
    if (!trap_reasons) {
        void *bytes=platform_map_fixed(NULL,(size_t)(TRAP_LIMIT>>12),BB_PROT_READ|BB_PROT_WRITE);
        if (!bytes) { perror("bbport: write trap table"); return; }
        uint8_t *expected=NULL;
        if (!__atomic_compare_exchange_n(&trap_reasons,&expected,(uint8_t *)bytes,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            platform_unmap(bytes,(size_t)(TRAP_LIMIT>>12));
    }
    uintptr_t end=address+size>TRAP_LIMIT ? TRAP_LIMIT : address+size;
    read_lock();
    pthread_mutex_lock(&trap_mutex);
    /* Runs of pages whose trap level changes, one mprotect each. */
    uintptr_t run=0; int run_level=0;
    const uintptr_t first=address>>12, last=(end-1)>>12;
    for (uintptr_t p=first; p<=last; ++p) {
        const uint8_t old=trap_reasons[p], now=on ? (uint8_t)(old|reason) : (uint8_t)(old & ~reason);
        if (old!=now) trap_reasons[p]=now;
        const int level=trap_level(now), change=trap_level(old)!=level;
        if (run && (!change || level!=run_level)) { trap_apply(run,p<<12,run_level); run=0; }
        if (change && !run) { run=p<<12; run_level=level; }
    }
    if (run) trap_apply(run,(last+1)<<12,run_level);
    pthread_mutex_unlock(&trap_mutex);
    read_unlock();
}
/* bbport: a libc write about to land on pages with write traps: the GPU side is told first and
 * lifts its traps, instead of the write faulting (a fault handler waits for the draws the GPU
 * thread has queued; libc copies into small textures every frame made the game's frames late).
 * Its announcement after the write (runtime_memory_note_cpu_write) stays as it was. */
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size) {
    const uint8_t *table=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    if (!table || !size || address>=TRAP_LIMIT || !hook_cpu_write) return;
    uint64_t last=(address+size-1)>>12;
    if (last>=(TRAP_LIMIT>>12)) last=(TRAP_LIMIT>>12)-1;
    for (uint64_t p=address>>12; p<=last; ++p)
        if (__atomic_load_n(&table[p],__ATOMIC_RELAXED)) { hook_cpu_write(address,size); return; }
}
unsigned runtime_memory_trap_reasons(uintptr_t address) {
    const uint8_t *table=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    return table && address<TRAP_LIMIT ? __atomic_load_n(&table[address>>12],__ATOMIC_ACQUIRE) : 0;
}
/* bbport: where direct memory at a guest address lives (BB_GUEST_IN_PLACE: the GPU uses the game's
 * memory where it is): its offset in direct memory and the end of the mapping, which is contiguous
 * there. 0 for anything else (flexible memory, reserved or unmapped addresses). */
int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end) {
    read_lock();
    const size_t i=vma_index(address);
    const int ok = i<vma_count && vmas[i].start<=address && address<vmas[i].end && vmas[i].kind==KIND_DIRECT;
    if (ok) {
        *phys=vmas[i].phys+(address-vmas[i].start);
        *end=vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* bbport: watch the window at a guest address (diagnostics): the probe hands the registry's node
 * pool over once the game has made it. The physical range behind it is derived here, so a release
 * of that physical memory is seen as well. */
void runtime_memory_watch_range(uintptr_t address, uint64_t size) {
    uint64_t phys=0; uintptr_t end=0;
    watch_va_lo=address; watch_va_hi=address+size;
    watch_phys_lo=watch_phys_hi=0;
    if (size && runtime_memory_direct_phys(address,&phys,&end) && end>=address+size) {
        watch_phys_lo=phys; watch_phys_hi=phys+size;
    }
    watch_on=1;
    fprintf(stderr,"Runtime: watching the window %#llx+%#llx (phys %#llx)\n",
            (unsigned long long)address,(unsigned long long)size,(unsigned long long)watch_phys_lo);
    fflush(stderr);
}
/* bbport: the guest ranges that show the same direct memory as [address, address+size) - its
 * aliases (diagnostics). The game maps one physical allocation at more than one address when it
 * wants two views of it, and a write through either view is a write into the same memory, so the
 * pool watch has to guard every one of them. Returns how many were written, at most max. */
int runtime_memory_aliases(uintptr_t address, uint64_t size, uintptr_t *starts, uintptr_t *ends,
                           int max) {
    int n=0;
    read_lock();
    const size_t i=vma_index(address);
    if (i<vma_count && vmas[i].start<=address && vmas[i].kind==KIND_DIRECT && size) {
        const uint64_t phys=vmas[i].phys+(address-vmas[i].start);
        for (size_t j=0;j<vma_count && n<max;++j) {
            const uint64_t length=vmas[j].end-vmas[j].start;
            if (vmas[j].kind!=KIND_DIRECT || vmas[j].phys+length<=phys || phys+size<=vmas[j].phys)
                continue;
            starts[n]=vmas[j].start; ends[n]=vmas[j].end; ++n;
        }
    }
    read_unlock();
    return n;
}
/* bbport: protection and direct memory type of the mapping at address (type -1: not direct
 * memory); 0 when nothing is mapped there. */
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    read_lock();
    const size_t i=vma_index(address);
    const int ok = i<vma_count && vmas[i].start<=address && vmas[i].kind!=KIND_RESERVED;
    if (ok) {
        *prot=vmas[i].prot;
        *type=vmas[i].kind==KIND_DIRECT ? vmas[i].type : -1;
        *end=vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* Writes through the backing view; 0 when part of the range has no backing. */
/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): every byte is
 * stored once. glibc's memcpy copies 4-7 bytes as two overlapping 4-byte stores (the same
 * 4 bytes twice for a 4-byte label): a thread preempted between them stored the label again
 * after the guest had seen it and freed the block that held it (a free-list link ending in
 * 0x00000004: guest fault 0x263b8e7). Aligned 2, 4 and 8-byte pieces are single stores. */
static void store_once(unsigned char *dst, const unsigned char *src, uint64_t n) {
    if (n > 64) { memcpy(dst,src,n); return; }
    while (n) {
        const uintptr_t at=(uintptr_t)dst;
        if (n>=8 && !(at&7)) { uint64_t v; memcpy(&v,src,8); __atomic_store_n((uint64_t *)dst,v,__ATOMIC_RELEASE); dst+=8; src+=8; n-=8; }
        else if (n>=4 && !(at&3)) { uint32_t v; memcpy(&v,src,4); __atomic_store_n((uint32_t *)dst,v,__ATOMIC_RELEASE); dst+=4; src+=4; n-=4; }
        else if (n>=2 && !(at&1)) { uint16_t v; memcpy(&v,src,2); __atomic_store_n((uint16_t *)dst,v,__ATOMIC_RELEASE); dst+=2; src+=2; n-=2; }
        else { __atomic_store_n(dst,*src,__ATOMIC_RELEASE); ++dst; ++src; --n; }
    }
}
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    /* bbport diagnostic: flag host-side writes (GPU readbacks, etc.) that land inside
     * the R1 registry node pool (guest 0x800410000..0x800410000+0x400000). */
    if (size && address < R1_POOL_VA_HI && address+size > R1_POOL_VA_LO) {
        char line[200];
        const int m=snprintf(line,sizeof(line),
            "POOL HIT: write_backing %" PRIu64 " bytes into R1 pool region addr=%p\n",
            (unsigned long long)size,(void *)(uintptr_t)address);
        if (m>0) platform_write_stdout(line,(size_t)m);
    }
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        watch_note("write-backing",0,at,n);
        store_once(phys_backing_ptr(vmas[i].phys+(at-vmas[i].start)),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Reads through the backing view: the host's copies of guest memory (uploads), which the GPU's
 * read protection (BB_READBACKS=2: GPU-written pages) must not stop. A fault there would ask the
 * GPU thread for a readback while it waits for these very copies (a hang at start). Gaps read
 * as zeros. */
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    unsigned char *out=data;
    read_lock();
    for (uintptr_t at=address, end=address+size; at<end;) {
        const size_t i=vma_index(at);
        uint64_t n;
        if (i<vma_count && vmas[i].start<=at && vmas[i].kind!=KIND_RESERVED) {
            n=(vmas[i].end<end ? vmas[i].end : end)-at;
            memcpy(out+(at-address),phys_backing_ptr(vmas[i].phys+(at-vmas[i].start)),n);
        } else {
            const uintptr_t next=i==vma_count ? end : vmas[i].start>at ? vmas[i].start : vmas[i].end;
            n=(next<end ? next : end)-at;
            memset(out+(at-address),0,n);
        }
        at+=n;
    }
    read_unlock();
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
static const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind};
}
/* The mapping table's generation (odd while it changes): callers cache regions against it. */
const uint64_t *runtime_memory_generation(void) { return &table_generation; }
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? BB_PROT_READ : 0)|(write ? BB_PROT_WRITE|BB_PROT_READ : 0)|(guest & BB_PROT_EXEC);
        platform_protect((void *)a,b-a,guest & want);
    }
    read_unlock();
}
/* bbport: the fault handler's last resort (bbgpu_handle_fault). An access the guest's own
 * protection allows faulted on a page GPU tracking protected, and the tracking did not resolve
 * it (repeated: it claimed the fault, yet the same access keeps faulting; else: no record of the
 * page). An unhandled fault ends the game without a trace, so the page gets the guest's
 * protection back: the GPU may miss writes there until it protects the page again. No locks
 * beyond the table's (nested per thread) and no allocation: this runs in the fault handler.
 * From Awaken's BloodBorneButPc. */
int runtime_memory_restore_access(void *context, uintptr_t address, int repeated) {
    /* The fault's kind comes from the host's context (Linux's error code, Windows's
     * exception information): an instruction fetch is never a tracking fault.
     * The context is what the GPU library passes on: on Linux the ucontext, on
     * Windows the exception's pointers. */
#ifdef _WIN32
    PlatformContext uc={(EXCEPTION_POINTERS *)context,NULL};
    const PlatformContext *view=&uc;
#else
    const PlatformContext *view=(const PlatformContext *)context;
#endif
    const int is_write=platform_context_is_write(view);
    if (platform_context_is_fetch(view)) return 0; /* instruction fetch */
    const uintptr_t page=address&~(uintptr_t)4095;
    int restored=0;
    /* Why the access was not restored: the report says it, so a fault that ends the game
     * can be told apart from a page the guest itself asked to be read-only. */
    const char *why="restored";
    unsigned vma_prot=0;
    int vma_kind=-1;
    read_lock();
    const size_t i=vma_index(address);
    if (i>=vma_count || vmas[i].start>address) {
        why="no region of the guest's own";
    } else {
        vma_prot=(unsigned)vmas[i].prot;
        vma_kind=vmas[i].kind;
        if (vmas[i].kind==KIND_RESERVED) {
            why="a region the guest reserved";
        } else {
            const int guest=host_prot(vmas[i].prot);
            if (!(guest & (is_write ? BB_PROT_WRITE : BB_PROT_READ))) {
                why="the guest's own protection forbids it";
            } else {
                restored=!platform_protect((void *)page,4096,guest);
                if (!restored) why="the protection could not be restored";
            }
        }
    }
    read_unlock();
    if (restored) {
        static _Atomic unsigned notes;
        const unsigned n=atomic_fetch_add(&notes,1)+1;
        if (n<=8 || !(n&(n-1))) { /* the first ones, then at powers of two */
            char line[256];
            int length=snprintf(line,sizeof(line),
                "Runtime: %s fault at %#llx: guest page left protected by GPU tracking (%s); "
                "access restored (%u so far)\n", is_write ? "write" : "read", (unsigned long long)address,
                repeated ? "the fault repeats" : "not handled", n);
            if (length<0) length=0;
            if (length>=(int)sizeof(line)) length=(int)sizeof(line)-1;
            platform_write_stdout(line,(size_t)length);
        }
    } else {
        static _Atomic unsigned notes;
        const unsigned n=atomic_fetch_add(&notes,1)+1;
        if (n<=8 || !(n&(n-1))) {
            char line[256];
            int length=snprintf(line,sizeof(line),
                "Runtime: %s fault at %#llx not restored: %s (region prot %#x kind %d, %u so far)\n",
                is_write ? "write" : "read", (unsigned long long)address, why, vma_prot, vma_kind, n);
            if (length<0) length=0;
            if (length>=(int)sizeof(line)) length=(int)sizeof(line)-1;
            platform_write_stdout(line,(size_t)length);
        }
    }
    return restored;
}
/* The guest's own view of an address: the region it is in, its kind and its protection.
 * The fault report uses it to tell a page the GPU left protected from a page the guest
 * asked to be read-only. */
int runtime_memory_describe(uintptr_t address, char *buffer, size_t size) {
    if (!buffer || !size) return 0;
    buffer[0]=0;
    read_lock();
    const size_t i=vma_index(address);
    if (i<vma_count && vmas[i].start<=address && address<vmas[i].end) {
        snprintf(buffer,size,"region %#" PRIx64 "-%#" PRIx64 " kind %d prot %#x phys %#" PRIx64,
                 (uint64_t)vmas[i].start,(uint64_t)vmas[i].end,vmas[i].kind,(unsigned)vmas[i].prot,
                 (uint64_t)vmas[i].phys);
    } else {
        snprintf(buffer,size,"no region");
    }
    read_unlock();
    return 1;
}
/* bbport: the guest's own memory regions, for the crash diagnostics: where a value that should
 * not be there lives. The regions are the guest's address space, which is the host's, so their
 * contents can be read directly (the physical address is given too: a region that is backed by
 * the pool can be looked at in the pool's own view as well). */
int runtime_memory_regions(uintptr_t *starts, uintptr_t *ends, uintptr_t *phys, int *kinds, int max) {
    if (!starts || !ends || max <= 0) return 0;
    int count=0;
    read_lock();
    for (size_t i=0;i<vma_count && count<max;++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        starts[count]=vmas[i].start;
        ends[count]=vmas[i].end;
        if (phys) phys[count]=(uintptr_t)vmas[i].phys;
        if (kinds) kinds[count]=vmas[i].kind;
        ++count;
    }
    read_unlock();
    return count;
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    read_unlock();
    return result;
}
void runtime_memory_buckets_report(void) {
    /* What the guest asked for, by size: the game's pools and the library's heap. */
    for (int bucket=10;bucket<24;++bucket) {
        const uint64_t asked=atomic_load(&direct_asked[bucket]);
        if (!asked) continue;
        const uint64_t failed=atomic_load(&direct_failed[bucket]);
        const uint64_t live=atomic_load(&direct_live_by_bucket[bucket]);
        printf("Runtime: direct %llu MiB blocks: asked=%llu, failed=%llu, live=%llu MiB\n",
               1ull<<(bucket-20), (unsigned long long)asked, (unsigned long long)failed,
               (unsigned long long)(live/1048576));
    }
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
    runtime_memory_buckets_report();
}
