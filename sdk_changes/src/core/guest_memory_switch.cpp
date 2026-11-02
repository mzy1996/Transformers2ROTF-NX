/* See guest_memory_switch.h for the design and the reason behind each decision. */

#if defined(__SWITCH__)

#include "guest_memory_switch.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include <algorithm>
#include <map>
#include <set>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

namespace {

constexpr size_t kPageSize  = 0x1000;
/*
 * Large chunks are requested 2 MB aligned, so the kernel can use block descriptors instead of a
 * page table.
 */
constexpr size_t kBlockSize = 0x200000;
/*
 * Smallest commit (Torchlight/T2ROTF): each chunk costs the kernel a code alias plus one
 * mapping per view, and a game that commits its physical memory 4 KB at a time
 * runs out of kernel memory blocks (0xCE01, OutOfResource) with only ~300 MB
 * committed. Rounding commits to 64 KB divides the number of chunks by up to 16.
 * The extra pages are committed zeroed, which the guest cannot tell apart.
 *
 * 64 KB was still not enough: Torchlight's physical heap hands out small blocks
 * (8 to 160 KB) one after another, top down, and each one became its own chunk.
 * On another console the game hit 0xCE01 again after a town -> mine -> town trip,
 * with 1,135 chunks for 395 MB (left part of the town black, then the whole console
 * hung: the kernel memory blocks may be shared with the system). Committing whole
 * 2 MB blocks puts those neighbours in a single chunk (at most 256 for the 512 MB
 * of the 360) and lets the kernel use block descriptors. The cost is at most 2 MB
 * per committed run, and a chunk only partly decommitted stays committed, which
 * the 512 MB of guest physical memory bounds.
 */
constexpr size_t kCommitGranule = kBlockSize;

/* Direction B: pre-allocate backing in heap, shadow in code-memory via
 * virtmemFindCodeMemory / svcMapProcessCodeMemory / svcSetProcessMemoryPermission.
 * Done before NVK init to grab code-memory VA while it is still free. */
static void* g_prealloc_backing = nullptr;
static size_t g_prealloc_size = 0;
static void* g_prealloc_shadow = nullptr;      /* code-memory alias of the prealloc backing */
static VirtmemReservation* g_prealloc_shadow_res = nullptr;

/* Direction F: if svcSetMemoryAttribute can make heap pages MapProcessAllowed,
 * we map backing directly into the window without a code-memory shadow. */
static bool g_f_mode_enabled = false;

constexpr size_t AlignDown(size_t v, size_t a) { return v & ~(a - 1); }
constexpr size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/* A committed chunk: heap backing, its code alias (the shadow), and the span of the mapping it covers. */
struct Chunk {
    size_t offset;      /* offset in the mapping */
    size_t length;
    void*  backing;     /* from the heap. Inaccessible after mapping it as code */
    void*  shadow;      /* the codeAlias: always RW, a back door */
    VirtmemReservation* shadow_res; /* virtmem reservation of the shadow (nullptr for prealloc slices) */
};

/* A view: a span of the window that mirrors the mapping from `offset`. */
struct View {
    uint8_t* base;
    size_t   offset;
    size_t   length;
};

/* A span already mapped in this operation, so it can be undone if another fails. */
struct Tramo {
    size_t   win_off;
    uint8_t* src;
    size_t   len;
};

/*
 * The window size, without the lock
 *
 * InGuestWindow() is called on every fault (tens of thousands per second in a race) and only needs
 * to know whether the address falls inside the window. Reading it from State meant taking the global
 * lock, the same one the mappings use, and in the profile the thread that faulted most spent 22.8%
 * of its time in svcArbitrateLock because of this.
 *
 * base and size are set once in RexGmInit and only change again in RexGmShutdown, so an atomic copy
 * is enough and cannot get out of sync.
 */
std::atomic<size_t> g_size_rapido{0};

/*
 * The base goes in its own atomic and is published after the size. That way, if a thread sees the
 * base, the size is already set: the other way round (as it used to be) the window could be seen
 * half-initialized, with a valid base and size 0, and then InGuestWindow said no and a legitimate
 * guest fault ended in a fatal stop.
 */
std::atomic<uint8_t*> g_base_rapido{nullptr};

struct State {
    /*
     * Exclusive lock, and it cannot be replaced by a reader/writer one.
     *
     * shared_mutex was tried and it hung the game. The idea was sound on paper: the three calls of
     * the fault path only read, so they could run concurrently. But the performance profiler suspends
     * threads to sample them (svcSetThreadActivity in switch_perf.cpp) and then takes this same lock
     * to read the memory counters.
     *
     * With shared_mutex that is fatal: libstdc++ gives writers preference, so if a thread holding the
     * shared lock is suspended, the first writer to arrive queues up, and all following readers queue
     * behind it. The whole process stops. It hung exactly like that in a test: all 17 threads idle and
     * the profiler unable to write a single report.
     *
     * What did pay off from that attempt is in exception_handler_switch.cpp: asking once per fault
     * instead of five times. That reduces lock traffic without changing how it blocks.
     */
    std::mutex mutex;
    uint8_t* base = nullptr;
    size_t   size = 0;
    VirtmemReservation* reservation = nullptr;

    std::vector<View> views;
    /* chunks sorted by offset: allows finding the one that covers an offset */
    std::map<size_t, Chunk> chunks;
    /*
     * Logical permissions per page of the window, keyed by offset from base.
     * Only pages that are not REX_GM_WRITE are stored; REX_GM_WRITE is the common
     * case, so the table stays small. A page with an entry here is unmapped from
     * the window, whether it is committed or not.
     */
    std::map<size_t, RexGmAccess> protection;
    /*
     * Which (view index, chunk offset) pairs are mapped. A chunk is not mapped into
     * all five views when it is committed: it enters each view the first time it is
     * accessed there. The real ceiling on Horizon is the process limit on mappable
     * memory, and mapping too much exhausts it.
     */
    std::set<std::pair<size_t, size_t>> view_mapped;

    size_t   committed = 0;
    size_t   mapped = 0;
    uint32_t last_result = 0;
};

State& S() {
    static State s;
    return s;
}

Handle Proc() { return envGetOwnProcessHandle(); }

bool Intersect(size_t a, size_t al, size_t b, size_t bl, size_t* lo, size_t* hi) {
    const size_t l = a > b ? a : b;
    const size_t ea = a + al, eb = b + bl;
    const size_t h = ea < eb ? ea : eb;
    if (l >= h) return false;
    *lo = l;
    *hi = h;
    return true;
}

/* Finds the chunk that contains an offset of the mapping. */
Chunk* ChunkAt(State& s, size_t offset) {
    auto it = s.chunks.upper_bound(offset);
    if (it == s.chunks.begin()) return nullptr;
    --it;
    Chunk& c = it->second;
    return (offset >= c.offset && offset < c.offset + c.length) ? &c : nullptr;
}

/* The view that contains a window address. */
const View* ViewFor(State& s, uint64_t addr) {
    for (const View& v : s.views) {
        const uint64_t lo = reinterpret_cast<uint64_t>(v.base);
        if (addr >= lo && addr < lo + v.length) return &v;
    }
    return nullptr;
}

RexGmAccess AccessOf(State& s, size_t page_off) {
    auto it = s.protection.find(page_off);
    return it == s.protection.end() ? REX_GM_WRITE : it->second;
}

/* Is there committed backing behind this window address? */
bool IsCommittedWindowAddress(State& s, uint64_t addr) {
    const View* v = ViewFor(s, addr);
    if (!v) return false;
    const size_t off = v->offset + static_cast<size_t>(addr - reinterpret_cast<uint64_t>(v->base));
    return ChunkAt(s, off) != nullptr;
}

/*
 * Unmaps [win_off, win_off+len) from the window, skipping pages with logical
 * protection, which are already unmapped. `src` is the shadow address that
 * corresponds to win_off.
 *
 * Walk: `it` always points to the first protected page >= cur. Keys are
 * page-aligned and increasing, so skipping a protected page and advancing the
 * iterator keeps that invariant, and the loop always makes progress.
 */
/*
 * Read-only page: try it, and if it is refused, go back to unmapping
 *
 * Why. Watching writes by unmapping the page has an expensive side effect: the
 * guest cannot read from it either, and every read is an exception. Measured in
 * a race: 108,000 emulated reads per second at about 9 us each, which is
 * practically the whole core of the main thread.
 *
 * With the page read-only, watching works the same (the write faults and the
 * handler services it) but reads are free.
 *
 * Why it is not a given. svcSetProcessMemoryPermission only accepts memory whose
 * state allows permission changes. The shadow is mapped with svcMapProcessMemory
 * from the heap backing, and whether Horizon allows permission changes there too
 * is not documented anywhere.
 *
 * So it is tried once. The first page that needs watching is tried with
 * permissions; if the kernel refuses, that is recorded and from then on pages
 * are unmapped as before. Nothing is left half done: if the call fails, it has
 * not changed anything.
 */
enum class SoloLectura { kSinProbar, kSi, kNo };
SoloLectura g_solo_lectura = SoloLectura::kSinProbar;

/* Returns true if the page was protected through permissions. */
bool ProtegerConPermisos(uint8_t* pagina, bool solo_lectura) {
    if (g_solo_lectura == SoloLectura::kNo) return false;

    const Result rc = svcSetProcessMemoryPermission(
        Proc(), reinterpret_cast<u64>(pagina), kPageSize, solo_lectura ? Perm_R : Perm_Rw);
    if (R_FAILED(rc)) {
        if (g_solo_lectura == SoloLectura::kSinProbar) {
            g_solo_lectura = SoloLectura::kNo;  // no se vuelve a intentar
        }
        return false;
    }
    g_solo_lectura = SoloLectura::kSi;
    return true;
}
void UnmapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_solo_lectura == SoloLectura::kSi) {
        /* No holes here: everything was mapped, watched or not. */
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + win_off, Proc(),
                                              reinterpret_cast<u64>(src), len))) {
            s.mapped -= len;
        }
        return;
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        if (R_SUCCEEDED(svcUnmapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur))) {
            s.mapped -= stop - cur;
        }
        cur = stop;
    }
}

/*
 * When watched pages are read-only instead of unmapped, the whole range is mapped
 * at once and the watched pages then get their permission lowered. Simpler and
 * faster than skipping them.
 */
bool MapWindowRangeSoloLectura(State& s, size_t win_off, uint8_t* src, size_t len) {
    const Result rc =
        svcMapProcessMemory(s.base + win_off, Proc(), reinterpret_cast<u64>(src), len);
    if (R_FAILED(rc)) {
        s.last_result = rc;
        return false;
    }
    s.mapped += len;
    for (auto it = s.protection.lower_bound(win_off);
         it != s.protection.end() && it->first < win_off + len; ++it) {
        ProtegerConPermisos(s.base + it->first, true);
    }
    return true;
}

/*
 * Maps [win_off, win_off+len) honoring the logical protection. If a call fails,
 * it undoes what this call had mapped and returns false: it never leaves a range
 * half mapped.
 */
bool MapWindowRange(State& s, size_t win_off, uint8_t* src, size_t len) {
    if (g_solo_lectura == SoloLectura::kSi) {
        return MapWindowRangeSoloLectura(s, win_off, src, len);
    }
    const size_t end = win_off + len;
    size_t cur = win_off;
    auto it = s.protection.lower_bound(cur);
    while (cur < end) {
        if (it != s.protection.end() && it->first == cur) {
            cur += kPageSize;
            ++it;
            continue;
        }
        const size_t stop = (it != s.protection.end() && it->first < end) ? it->first : end;
        const Result rc = svcMapProcessMemory(s.base + cur, Proc(),
                                              reinterpret_cast<u64>(src + (cur - win_off)),
                                              stop - cur);
        if (R_FAILED(rc)) {
            s.last_result = rc;
            /*
             * The unmap walks the same way as this loop, so it removes exactly what was
             * mapped, no more and no less.
             */
            UnmapWindowRange(s, win_off, src, cur - win_off);
            return false;
        }
        s.mapped += stop - cur;
        cur = stop;
    }
    return true;
}

/*
 * Maps the range [offset, offset+length) of a chunk into every view that covers
 * it. This is where the 360 mirrors come from.
 *
 * All or nothing: if it fails in one view, it undoes the previous ones. The
 * alternative (leaving a view unmapped and carrying on) would make every access
 * to it be emulated one by one by the exception handler: it would work, but at a
 * fraction of the speed and without any warning. That is exactly the bug the
 * first version had.
 */
bool ViewIndexFor(State& s, uint64_t addr, size_t* out) {
    for (size_t i = 0; i < s.views.size(); i++) {
        const uint64_t lo = reinterpret_cast<uint64_t>(s.views[i].base);
        if (addr >= lo && addr < lo + s.views[i].length) { *out = i; return true; }
    }
    return false;
}

/* Maps a chunk into one view: the view of the faulting access. */
bool MapChunkIntoView(State& s, size_t view_index, const Chunk& c) {
    const View& v = s.views[view_index];
    size_t lo, hi;
    if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) return false;
    const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
    uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
    if (!MapWindowRange(s, win_off, src, hi - lo)) return false;
    s.view_mapped.insert({view_index, c.offset});
    return true;
}

void UnmapChunkFromViews(State& s, const Chunk& c) {
    for (size_t i = 0; i < s.views.size(); i++) {
        auto it = s.view_mapped.find({i, c.offset});
        if (it == s.view_mapped.end()) continue;  /* never entered this view */
        const View& v = s.views[i];
        size_t lo, hi;
        if (!Intersect(c.offset, c.length, v.offset, v.length, &lo, &hi)) continue;
        const size_t win_off = static_cast<size_t>(v.base - s.base) + (lo - v.offset);
        uint8_t* src = static_cast<uint8_t*>(c.shadow) + (lo - c.offset);
        UnmapWindowRange(s, win_off, src, hi - lo);
        s.view_mapped.erase(it);
    }
}

void ReleaseChunk(Chunk& c) {
    /* Pre-allocated arena: released as a whole in RexGmShutdown. */
    if (g_prealloc_size > 0 && c.offset < g_prealloc_size) return;

    if (c.backing) {
        /* Direction B: undo the code alias before freeing the heap backing. */
        if (c.shadow && c.shadow != c.backing) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(c.shadow),
                                      reinterpret_cast<u64>(c.backing), c.length);
        }
        if (c.shadow_res) {
            virtmemLock();
            virtmemRemoveReservation(c.shadow_res);
            virtmemUnlock();
        }
        free(c.backing);
    }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory),
 * and hbloader pre-inflates the heap with ~3.2 GB, leaving almost nothing of
 * that limit for the shadow code mappings RexGmCommit creates: even committing
 * 8 KB failed with 0xCE01 (KERNELERROR_RESOURCE_EXHAUSTED) at ~500 MB mapped.
 *
 * Ask for 1 GB more of that limit before the first commit. Best effort only:
 * some launch configurations hand out a resource limit handle without
 * SetLimit permission (0xDC01). That failure is harmless (the original limit
 * stays) but is printed so `rex_perfil`'s "limite de mapeo" can be checked
 * against the real value.
 */
void RaiseResourceLimit() {
    u64 v = 0;
    Result rc = svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc)) {
        fprintf(stderr, "RexGmInit: svcGetInfo(InfoType_ResourceLimit) failed rc=0x%08X\n",
                (unsigned)rc);
        return;
    }
    Handle reslimit = static_cast<Handle>(v);
    s64 tope = 0;
    rc = svcGetResourceLimitLimitValue(&tope, reslimit, LimitableResource_Memory);
    if (R_FAILED(rc)) {
        fprintf(stderr, "RexGmInit: svcGetResourceLimitLimitValue(Memory) failed rc=0x%08X\n",
                (unsigned)rc);
        svcCloseHandle(reslimit);
        return;
    }
    rc = svcSetResourceLimitLimitValue(reslimit, LimitableResource_Memory, tope + 0x40000000LL);
    if (R_FAILED(rc)) {
        fprintf(stderr,
                "RexGmInit: svcSetResourceLimitLimitValue(+1 GB) failed rc=0x%08X "
                "(limit was 0x%llX)\n",
                (unsigned)rc, (unsigned long long)tope);
    } else {
        fprintf(stderr,
                "RexGmInit: raised LimitableResource_Memory 0x%llX -> 0x%llX\n",
                (unsigned long long)tope, (unsigned long long)(tope + 0x40000000LL));
    }
    svcCloseHandle(reslimit);
}

/* NOTE: hbloader's pre-inflated heap (~3.2 GB) is shrunk down to 1 GB at the
 * process entry point (windowed_app_main_switch.cpp, main()), BEFORE the
 * libc malloc arena is set up via svcSetHeapSize. Doing it here (or anywhere
 * after malloc has initialized) would leave libc's internal arena pointers
 * stale and cause a Data Abort (EC 0x24) inside _malloc_r. Do not re-add a
 * heap shrink call in this translation unit.
 */

}  // namespace

extern "C" {

uint8_t* RexGmInit(size_t size) {
    {
        setvbuf(stderr, nullptr, _IONBF, 0);
        FILE* f = fopen("/sdmc/rex_diag_rexgminit.txt", "w");
        if (f) { fprintf(f, "RexGmInit reached, size=0x%zX\n", size); fflush(f); fclose(f); }
        fflush(nullptr);
    }
    fprintf(stderr, "[DIAG] RexGmInit entered, size=0x%zX\n", size);
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.base) return s.base;

    /* hbloader pre-inflates the heap: raise the mapping limit before the first
     * commit, see RaiseResourceLimit() for why and for the 0xDC01 fallback. */
    RaiseResourceLimit();
    /* ShrinkHeap() moved to main() entry point to avoid unmapping pages already
     * allocated by the C runtime (Data Abort at host heap addresses). */

    /*
     * Direction B: pre-allocate backing in the heap and alias it in the code
     * region. This must happen BEFORE NVK/Vulkan init: the driver consumes most
     * of the code region during setup, and once that happens there is no room
     * for a contiguous code alias of the arena (that was the failure of the
     * earlier all-code-memory design). With NVK out of the way first, a code
     * alias of the whole prealloc is still findable.
     *
     * The alias makes the backing acceptable to svcMapProcessMemory (heap
     * memory alone is refused with 0xD401 InvalidCurrentMemory).
     *
     * Retry with smaller sizes: both the heap allocation of the backing and the
     * code region reservation can run out. Whichever size succeeds, slices of
     * [0, g_prealloc_size) are carved from the arena in RexGmCommit and the
     * whole thing is released as one piece in RexGmShutdown.
     */
#if !defined(__SWITCH__)
    {
        static const size_t kFallbackSizes[] = {
            128ull * 1024 * 1024,       /* 128 MB */
            64ull  * 1024 * 1024,       /* 64  MB */
            32ull  * 1024 * 1024,       /* 32  MB */
            16ull  * 1024 * 1024,       /* 16  MB */
        };
        constexpr size_t kFallbackCount =
            sizeof(kFallbackSizes) / sizeof(kFallbackSizes[0]);
        size_t stage = 0;  /* stage 0 tries the full size, then the fallbacks in order */
        for (;;) {
            const size_t try_size = (stage == 0) ? size : kFallbackSizes[stage - 1];
            g_prealloc_backing = memalign(kPageSize, try_size);
            if (!g_prealloc_backing) {
                fprintf(stderr, "RexGmInit: memalign(%zu) failed\n", try_size);
            } else {
                memset(g_prealloc_backing, 0, try_size);
                virtmemLock();
                g_prealloc_shadow = virtmemFindCodeMemory(try_size, kPageSize);
                if (g_prealloc_shadow) {
                    g_prealloc_shadow_res = virtmemAddReservation(g_prealloc_shadow, try_size);
                }
                virtmemUnlock();

                Result rc = 0;
                if (g_prealloc_shadow && g_prealloc_shadow_res) {
                    rc = svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                                 reinterpret_cast<u64>(g_prealloc_backing), try_size);
                    if (R_SUCCEEDED(rc)) {
                        rc = svcSetProcessMemoryPermission(Proc(),
                                                           reinterpret_cast<u64>(g_prealloc_shadow),
                                                           try_size, Perm_Rw);
                    }
                    if (R_FAILED(rc)) {
                        svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                                  reinterpret_cast<u64>(g_prealloc_backing), try_size);
                    }
                }
                if (g_prealloc_shadow && g_prealloc_shadow_res && R_SUCCEEDED(rc)) {
                    g_prealloc_size = try_size;
                    fprintf(stderr, "RexGmInit: prealloc %zu bytes shadow=%p (code VA)\n",
                            try_size, g_prealloc_shadow);
                    break;
                }
                fprintf(stderr, "RexGmInit: prealloc %zu failed (code_va=%d) rc=0x%08X\n",
                        try_size, g_prealloc_shadow != nullptr, (unsigned)rc);
                if (g_prealloc_shadow_res) {
                    virtmemLock();
                    virtmemRemoveReservation(g_prealloc_shadow_res);
                    virtmemUnlock();
                    g_prealloc_shadow_res = nullptr;
                }
                g_prealloc_shadow = nullptr;
                free(g_prealloc_backing);
                g_prealloc_backing = nullptr;
            }
            ++stage;
            if (stage > kFallbackCount) {
                fprintf(stderr, "RexGmInit: prealloc failed for all sizes -- falling back to on-demand (B2)\n");
                g_prealloc_size = 0;
                break;
            }
        }
    }
#else
    g_prealloc_size = 0;
    fprintf(stderr, "RexGmInit: prealloc disabled on Switch (on-demand fault-in only)\n");
#endif

    /* Direction F: try converting heap pages to MapProcessAllowed
     * and mapping them directly with svcMapProcessMemory, bypassing the code-memory
     * shadow that NVK exhausts.
     *
     * MOD v6.27: probe unconditionally.  Even a small prealloc (e.g. 128 MB) is
     * quickly exhausted for a 4.5 GB arena and B2 fails with CE01 because NVK has
     * already consumed the code-memory mapping quota.  If F works we drop the B
     * prealloc entirely and let every commit go through heap+MapProcessAllowed. */
    {
        void* test_backing = memalign(kPageSize, kPageSize);
        if (test_backing) {
            memset(test_backing, 0, kPageSize);
            /* MemoryAttribute_MapProcessAllowed = 0x8 */
            Result rc = svcSetMemoryAttribute(test_backing, kPageSize, 0x8, 0);
            fprintf(stderr,
                    "RexGmInit: svcSetMemoryAttribute(MapProcessAllowed) rc=0x%08X\n",
                    (unsigned)rc);
            if (R_SUCCEEDED(rc)) {
                virtmemLock();
                void* test_dst = virtmemFindAslr(kPageSize, kPageSize);
                VirtmemReservation* test_res =
                    test_dst ? virtmemAddReservation(test_dst, kPageSize) : nullptr;
                virtmemUnlock();
                if (test_dst && test_res) {
                    rc = svcMapProcessMemory(test_dst, Proc(),
                                             reinterpret_cast<u64>(test_backing), kPageSize);
                    fprintf(stderr,
                            "RexGmInit: svcMapProcessMemory(test_dst=%p, proc=%lu, backing=%p) rc=0x%08X\n",
                            test_dst, (unsigned long)Proc(), test_backing, (unsigned)rc);
                    if (R_SUCCEEDED(rc)) {
                        g_f_mode_enabled = true;
                        fprintf(stderr,
                                "RexGmInit: Direction F enabled (heap+MapProcessAllowed)\n");
                        /* Release any Direction B prealloc -- F covers the whole arena. */
                        if (g_prealloc_size > 0) {
                            fprintf(stderr,
                                    "RexGmInit: releasing Direction B prealloc (%zu bytes)\n",
                                    g_prealloc_size);
                            svcUnmapProcessCodeMemory(Proc(),
                                                      reinterpret_cast<u64>(g_prealloc_shadow),
                                                      reinterpret_cast<u64>(g_prealloc_backing),
                                                      g_prealloc_size);
                            if (g_prealloc_shadow_res) {
                                virtmemLock();
                                virtmemRemoveReservation(g_prealloc_shadow_res);
                                virtmemUnlock();
                                g_prealloc_shadow_res = nullptr;
                            }
                            g_prealloc_shadow = nullptr;
                            free(g_prealloc_backing);
                            g_prealloc_backing = nullptr;
                            g_prealloc_size = 0;
                        }
                        svcUnmapProcessMemory(test_dst, Proc(),
                                              reinterpret_cast<u64>(test_backing), kPageSize);
                    } else {
                        fprintf(stderr,
                                "RexGmInit: Direction F map FAILED (test_dst=%p, proc=%lu, rc=0x%08X)\n",
                                test_dst, (unsigned long)Proc(), (unsigned)rc);
                    }
                    virtmemLock();
                    virtmemRemoveReservation(test_res);
                    virtmemUnlock();
                }
            }
            free(test_backing);
        }
        if (!g_f_mode_enabled) {
            fprintf(stderr,
                    "RexGmInit: Direction F probe failed, code-memory path unavailable\n");
            /* Fall through to window init; RexGmCommit will fail on first on-demand chunk. */
        }
    }

    virtmemLock();
    /* Reserve the guest window in regular virtual address space (not code memory).
     * The window itself only needs to be a reserved VA range; actual mappings are
     * injected via svcMapProcessMemory from the shadow (code memory) on demand.
     * Using virtmemFindAslr instead of virtmemFindCodeMemory frees ~4.5 GB of
     * code memory virtual space for the shadow allocations, which is critical on
     * native 4 GB Switch where code memory VA is tight after Mesa/NVK init. */
    s.base = static_cast<uint8_t*>(virtmemFindAslr(size, kPageSize));
    if (s.base) s.reservation = virtmemAddReservation(s.base, size);
    virtmemUnlock();

    if (!s.base || !s.reservation) {
        s.base = nullptr;
        /* Undo the code alias before the backing can be freed again. */
        if (g_prealloc_backing && g_prealloc_shadow && g_prealloc_size > 0) {
            svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                      reinterpret_cast<u64>(g_prealloc_backing), g_prealloc_size);
        }
        if (g_prealloc_shadow_res) {
            virtmemLock();
            virtmemRemoveReservation(g_prealloc_shadow_res);
            virtmemUnlock();
            g_prealloc_shadow_res = nullptr;
        }
        g_prealloc_shadow = nullptr;
        free(g_prealloc_backing);
        g_prealloc_backing = nullptr;
        g_prealloc_size = 0;
        fprintf(stderr, "[DIAG] RexGmInit: return nullptr (window alloc failed)\n");
        return nullptr;
    }

    s.size = size;
    g_size_rapido.store(size, std::memory_order_release);
    g_base_rapido.store(s.base, std::memory_order_release);
    fprintf(stderr, "[DIAG] RexGmInit: return base=%p size=0x%zX\n", (void*)s.base, s.size);
    return s.base;
}

void RexGmShutdown(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);

    for (auto& kv : s.chunks) {
        UnmapChunkFromViews(s, kv.second);
        ReleaseChunk(kv.second);
    }
    s.chunks.clear();
    s.views.clear();
    s.protection.clear();
    s.committed = 0;
    s.mapped = 0;

    if (s.reservation) {
        virtmemLock();
        virtmemRemoveReservation(s.reservation);
        virtmemUnlock();
        s.reservation = nullptr;
    }
    s.base = nullptr;
    s.size = 0;
    /*
     * Reverse order from startup: the base is cleared first, so nobody gets in with
     * a size that is no longer valid.
     */
    g_base_rapido.store(nullptr, std::memory_order_release);
    g_size_rapido.store(0, std::memory_order_release);

    /* Release the pre-allocated arena (Direction B): unmap its code alias first
     * (the backing becomes usable again), drop the virtmem reservation, then
     * free the heap backing. */
    if (g_prealloc_backing && g_prealloc_shadow && g_prealloc_size > 0) {
        svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(g_prealloc_shadow),
                                  reinterpret_cast<u64>(g_prealloc_backing), g_prealloc_size);
    }
    if (g_prealloc_shadow_res) {
        virtmemLock();
        virtmemRemoveReservation(g_prealloc_shadow_res);
        virtmemUnlock();
        g_prealloc_shadow_res = nullptr;
    }
    g_prealloc_shadow = nullptr;
    if (g_prealloc_backing) {
        free(g_prealloc_backing);
        g_prealloc_backing = nullptr;
    }
    g_prealloc_size = 0;
}

uint8_t* RexGmBase(void) { return g_base_rapido.load(std::memory_order_acquire); }

size_t RexGmSize(void) { return g_size_rapido.load(std::memory_order_acquire); }

bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    const View* v = ViewFor(s, window_address);
    if (!v) return false;
    if (offset_out) {
        *offset_out =
            v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    }
    return true;
}

size_t RexGmCommittedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.committed;
}

size_t RexGmMappedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.mapped;
}

size_t RexGmChunkCount(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.chunks.size();
}

size_t RexGmProtectedPages(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.protection.size();
}

int RexGmModoProteccion(void) {
    switch (g_solo_lectura) {
        case SoloLectura::kSi: return 1;
        case SoloLectura::kNo: return 2;
        default: return 0;
    }
}

uint32_t RexGmLastResult(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.last_result;
}

bool RexGmAddView(uint8_t* base, size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    if (base < s.base || static_cast<size_t>(base - s.base) + length > s.size)
        return false;  /* outside the reserved window */

    const View v{base, offset, length};

    /*
     * Nothing is mapped here: what is already committed enters this view the first
     * time it is touched.
     */
    s.views.push_back(v);
    return true;
}

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {
    /*
     * Memory is always committed accessible. Restricting permissions is the job of
     * RexGmProtect, which works on window addresses and not on mapping offsets (and
     * it has to, because the same offset can be mirrored in several windows with
     * different permissions: the 360 does exactly that).
     *
     * `access` stays in the signature because the natural caller is
     * AllocFixed(kCommit, access), and removing it would invite forgetting to apply
     * it. Pages that already had logical protection keep it.
     */
    (void)access;
    if (length == 0) return true;

    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const size_t begin = AlignDown(offset, kCommitGranule);
    const size_t end   = std::min(AlignUp(offset + length, kCommitGranule), s.size);

size_t cur = begin;
    while (cur < end) {
        if (Chunk* existente = ChunkAt(s, cur)) {
            cur = existente->offset + existente->length;
            continue;
        }

        /* Where the gap ends: the next chunk, or the end. */
        size_t hueco_fin = end;
        auto sig = s.chunks.upper_bound(cur);
        if (sig != s.chunks.end() && sig->second.offset < hueco_fin)
            hueco_fin = sig->second.offset;

        /*
         * Grow the commit window to the next block boundary unconditionally.
         *
         * Horizon charges the process mapping limit (LimitableResource_Memory)
         * per mapping, not per byte, so a stream of 8 KB requests was paying a
         * full mapping each time and ran out of limit with 0xCE01 even though
         * plenty of physical memory remained. Committing a whole block instead
         * means one svcMapProcessMemory covers many future small commits;
         * they hit ChunkAt above and skip the syscall.
         *
         * The window only ever extends into a gap that has no committed chunk
         * and never crosses the next existing chunk (hueco_fin already stops
         * at it), so no previous mapping is duplicated and s.chunks keeps a
         * single entry for the whole block.
         */
        size_t tam = hueco_fin - cur;
        size_t hueco_ext = AlignUp(hueco_fin, kBlockSize);
        size_t siguiente = (sig != s.chunks.end()) ? sig->second.offset
                                                   : ~size_t(0);
        if (hueco_ext > siguiente) hueco_ext = siguiente;
        if (hueco_ext > hueco_fin) {
            hueco_fin = hueco_ext;
            tam = hueco_fin - cur;
        }

        if (cur + tam <= g_prealloc_size) {
            /* Slice from the pre-allocated arena: its backing and code alias are
             * released as a whole in RexGmShutdown, so slabs carry no resources
             * of their own. */
            void* shadow = static_cast<uint8_t*>(g_prealloc_shadow) + cur;

            const Chunk c{cur, tam, nullptr, shadow, nullptr};
            s.chunks[cur] = c;
            s.committed += tam;
            cur = hueco_fin;
        } else {
            size_t block = tam;
            if (block < kPageSize) block = kPageSize;

            void* backing = memalign(kPageSize, block);
            if (!backing) {
                fprintf(stderr, "RexGmCommit: memalign(%zu) failed at offset %zu\n", block, cur);
                return false;
            }
            memset(backing, 0, block);

            if (g_f_mode_enabled) {
                /* Direction F: heap page with MapProcessAllowed, mapped directly
                 * into the window by RexGmFaultIn/MapChunkIntoView. */
                Result rc = svcSetMemoryAttribute(backing, block, 0x8, 0);
                if (R_FAILED(rc)) {
                    free(backing);
                    fprintf(stderr,
                            "RexGmCommit: svcSetMemoryAttribute failed rc=0x%08X at offset %zu\n",
                            (unsigned)rc, cur);
                    return false;
                }
                const Chunk c{cur, block, backing, backing, nullptr};
                s.chunks[cur] = c;
                s.committed += block;
                cur += block;
            } else {
                /* Direction B2: fresh code-memory shadow alias. */
                virtmemLock();
                void* shadow = virtmemFindCodeMemory(block, kPageSize);
                VirtmemReservation* shadow_res =
                    shadow ? virtmemAddReservation(shadow, block) : nullptr;
                virtmemUnlock();

                Result rc = 0;
                if (shadow && shadow_res) {
                    rc = svcMapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                                 reinterpret_cast<u64>(backing), block);
                    if (R_SUCCEEDED(rc)) {
                        rc = svcSetProcessMemoryPermission(Proc(), reinterpret_cast<u64>(shadow),
                                                           block, Perm_Rw);
                    }
                    if (R_FAILED(rc)) {
                        svcUnmapProcessCodeMemory(Proc(), reinterpret_cast<u64>(shadow),
                                                  reinterpret_cast<u64>(backing), block);
                    }
                }
                if (!shadow || !shadow_res || R_FAILED(rc)) {
                    if (shadow_res) {
                        virtmemLock();
                        virtmemRemoveReservation(shadow_res);
                        virtmemUnlock();
                    }
                    free(backing);
                    fprintf(stderr, "RexGmCommit: code alias failed rc=0x%08X at offset %zu\n",
                            (unsigned)rc, cur);
                    return false;
                }

                const Chunk c{cur, block, backing, shadow, shadow_res};
                s.chunks[cur] = c;
                s.committed += block;
                cur += block;
            }
        }
    }
    return true;
}

bool RexGmFaultIn(uint64_t window_address) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    size_t vi = 0;
    if (!ViewIndexFor(s, window_address, &vi)) return false;

    /*
     * Page with logical protection: it is unmapped on purpose (write watching, MMIO,
     * read-only). Mapping the chunk would not bring it in (protected pages are
     * skipped), so answering yes would leave the fault repeating forever. The next
     * steps of the handler deal with it.
     */
    const size_t win_off =
        static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(s.base));
    if (s.protection.find(AlignDown(win_off, kPageSize)) != s.protection.end()) return false;

    const size_t off = s.views[vi].offset +
                       static_cast<size_t>(window_address -
                                           reinterpret_cast<uint64_t>(s.views[vi].base));
    Chunk* c = ChunkAt(s, off);
    if (!c) return false;                                       /* sin confirmar */
    if (s.view_mapped.count({vi, c->offset}) != 0) return false; /* ya estaba */
    return MapChunkIntoView(s, vi, *c);
}

bool RexGmDecommit(size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const size_t begin = AlignDown(offset, kPageSize);
    const size_t end   = AlignUp(offset + length, kPageSize);

    for (auto it = s.chunks.begin(); it != s.chunks.end();) {
        Chunk c = it->second;
        /*
         * Only chunks entirely contained in the range are freed: splitting a chunk
         * would also mean splitting its backing, and the backing is a single heap
         * allocation. Partial chunks stay committed, which is conservative but correct.
         */
        if (c.offset >= begin && c.offset + c.length <= end) {
            UnmapChunkFromViews(s, c);
            ReleaseChunk(c);
            s.committed -= c.length;
            it = s.chunks.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RexGmProtect(uint8_t* address, size_t length, RexGmAccess access, RexGmAccess* out_old) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr + length > wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);
    const size_t end   = AlignUp(static_cast<size_t>(addr - wbase) + length, kPageSize);

    if (out_old) *out_old = AccessOf(s, begin);

    const bool debe_estar = (access == REX_GM_WRITE);

    /*
     * Fast path, and the common one: asking for full access on a range that has no
     * protected page. AllocFixed(kCommit) does it for every allocation of the game
     * heap, and without this it would walk the range page by page (131,072
     * iterations for the 512 MB of physical memory) to do nothing.
     */
    if (debe_estar) {
        auto it = s.protection.lower_bound(begin);
        if (it == s.protection.end() || it->first >= end) return true;
    }

    for (size_t p = begin; p < end; p += kPageSize) {
        const bool estaba = (AccessOf(s, p) == REX_GM_WRITE);

        if (estaba != debe_estar) {
            /*
             * The view is looked up per page: a range can cross the boundary
             * between two views with different offsets.
             */
            if (const View* v = ViewFor(s, wbase + p)) {
                const size_t off_mapeo =
                    v->offset + (p - static_cast<size_t>(v->base - s.base));
                size_t vi = 0;
                Chunk* c = ChunkAt(s, off_mapeo);
                /*
                 * If the chunk has not entered this view yet, there is nothing to map
                 * or unmap: the permission is recorded and the first access will bring
                 * the chunk in with the right permission already.
                 */
                const bool en_vista = c != nullptr && ViewIndexFor(s, wbase + p, &vi) &&
                                      s.view_mapped.count({vi, c->offset}) != 0;
                if (en_vista) {
                    const u64 src = reinterpret_cast<u64>(
                        static_cast<uint8_t*>(c->shadow) + (off_mapeo - c->offset));
                    /*
                     * Permissions are tried first, which keeps the page readable. If the
                     * kernel refuses, it falls back to the usual unmap. ProtegerConPermisos
                     * remembers whether it works.
                     */
                    if (!ProtegerConPermisos(s.base + p, estaba)) {
                        const Result rc =
                            estaba ? svcUnmapProcessMemory(s.base + p, Proc(), src, kPageSize)
                                   : svcMapProcessMemory(s.base + p, Proc(), src, kPageSize);
                        if (R_FAILED(rc)) {
                            s.last_result = rc;
                            return false;
                        }
                        if (estaba) s.mapped -= kPageSize;
                        else s.mapped += kPageSize;
                    }
                }
                /*
                 * Not committed: nothing to map, but the permission is still recorded
                 * and RexGmCommit will honor it on commit.
                 */
            }
        }

        if (debe_estar) s.protection.erase(p);
        else s.protection[p] = access;
    }
    return true;
}

bool RexGmQueryProtect(uint8_t* address, size_t* length, RexGmAccess* out_access) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;

    const uint64_t addr  = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr >= wbase + s.size) return false;

    const size_t begin = AlignDown(static_cast<size_t>(addr - wbase), kPageSize);

    /*
     * An uncommitted page is reported as having no access, even if its logical
     * permission is the default one.
     *
     * This matters a lot: after a fault, the SDK's MMIO handler queries this, and if
     * it sees the page as accessible it concludes that another thread already
     * unprotected it and retries the instruction. If the page does not really exist,
     * the retry faults again... forever. A silent hang.
     */
    if (!IsCommittedWindowAddress(s, wbase + begin)) {
        if (out_access) *out_access = REX_GM_NONE;
        if (length) *length = kPageSize;
        return true;
    }

    const RexGmAccess a = AccessOf(s, begin);
    if (out_access) *out_access = a;

    /*
     * On input *length is the maximum to walk (0 = one page); on output, the size of
     * the committed range with the same permission.
     */
    size_t maximo = (length && *length) ? *length : kPageSize;
    if (begin + maximo > s.size) maximo = s.size - begin;

    size_t p = begin;
    const size_t tope = begin + maximo;
    while (p < tope && AccessOf(s, p) == a && IsCommittedWindowAddress(s, wbase + p))
        p += kPageSize;
    if (length) *length = p - begin;
    return true;
}

void* RexGmShadowFor(uint64_t window_address) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return nullptr;

    const View* v = ViewFor(s, window_address);
    if (!v) return nullptr;

    const size_t off_mapeo =
        v->offset + static_cast<size_t>(window_address - reinterpret_cast<uint64_t>(v->base));
    Chunk* c = ChunkAt(s, off_mapeo);
    if (!c) return nullptr;

    return static_cast<uint8_t*>(c->shadow) + (off_mapeo - c->offset);
}

}  // extern "C"

#endif /* __SWITCH__ */
