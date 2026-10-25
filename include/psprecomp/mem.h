/* psprecomp — PSP memory map.
 *
 * The Allegrex has no TLB. Addresses are decoded by their top bits into a
 * handful of fixed regions, which makes the recompiled memory path cheap:
 * mask off the segment bits, index a flat host allocation.
 *
 *   0x00010000-0x00013FFF   scratchpad  (16 KB, fast on-chip)
 *   0x04000000-0x041FFFFF   VRAM        (2 MB)
 *   0x08000000-0x09FFFFFF   main RAM    (32 MB on PSP-2000+, 24 MB usable
 *                                        on PSP-1000 where it ends at 0x09FFFFFF
 *                                        but only 0x087FFFFF is user memory)
 *
 * Each region is mirrored at three segment bases, which differ only in cache
 * behaviour — irrelevant to us, so we fold them together:
 *   0x0.......  kernel, cached
 *   0x4.......  uncached mirror
 *   0x8.......  kernel-only, cached
 * We mask with PSP_ADDR_MASK to collapse all three onto one backing store.
 */
#ifndef PSPRECOMP_MEM_H
#define PSPRECOMP_MEM_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSP_ADDR_MASK       0x3FFFFFFFu  /* collapse the cache-mirror segments */

#define PSP_SCRATCH_BASE    0x00010000u
#define PSP_SCRATCH_SIZE    0x00004000u  /* 16 KB */
#define PSP_VRAM_BASE       0x04000000u
#define PSP_VRAM_SIZE       0x00200000u  /* 2 MB */
#define PSP_RAM_BASE        0x08000000u
#define PSP_RAM_SIZE        0x02000000u  /* 32 MB */

typedef struct {
    uint8_t *ram;        /* PSP_RAM_SIZE bytes */
    uint8_t *vram;       /* PSP_VRAM_SIZE bytes */
    uint8_t *scratch;    /* PSP_SCRATCH_SIZE bytes */
} psp_memory;

extern psp_memory psp_mem;

int  psp_mem_init(void);
void psp_mem_free(void);

/* Map the loaded module's image.
 *
 * A relocatable PRX links at address 0, and the recompiled C has those
 * addresses baked in as literals — `lui $v1, 0x9` becomes `0x00090000`. So the
 * module's own code and data live *outside* the console's RAM window
 * (0x08000000+) and need their own mapping at wherever the module was linked.
 * A statically linked module already sits inside RAM and needs none of this.
 *
 * Call once, before loading segments. Returns 0 on success. */
int psp_mem_map_module(uint32_t base, uint32_t size);

/* Resolve a guest address to a host pointer, or NULL if unmapped.
 * `size` is the access width; a read straddling the end of a region is
 * rejected rather than silently truncated. */
void *psp_mem_ptr(uint32_t addr, uint32_t size);

/* Called by psp_mem_ptr for every VRAM access, before the pointer is handed
 * out, with the VRAM offset and size. Every CPU load and store of the
 * recompiled code and every HLE copy goes through psp_mem_ptr, so a GPU
 * backend whose render targets run ahead of emulated VRAM can make exactly
 * the bytes being touched current here -- lazily, instead of flushing all of
 * VRAM at every GE sync. NULL (the default) = no hook. */
void psp_mem_set_vram_hook(void (*fn)(uint32_t off, uint32_t size));

/* The PSP is little-endian and so is every host we target, so these are plain
 * loads once the address is resolved. Unmapped accesses return 0 / are dropped
 * and bump psp_mem_bad_access — a recompiled game that starts faulting here is
 * telling you the analysis missed something, so it is counted, not ignored. */
extern uint64_t psp_mem_bad_access;
void psp_mem_watch_write(uint32_t addr);
/* Called once, at the first out-of-range access (a host can dump memory). */
void psp_mem_set_first_bad_hook(void (*fn)(uint32_t addr));
/* Called on every unmapped access (the host dedupes / logs). */
void psp_mem_set_bad_hook(void (*fn)(uint32_t addr, int write, int width));
int psp_mem_watch_hits(void);
/* Called on each reported watch hit (after the line is printed): a host can
 * add context, e.g. which recompiled functions are on the stack. */
void psp_mem_set_watch_hook(void (*fn)(uint32_t addr, uint32_t value));
/* Reads: up to four watched words (0 clears them all); the hook gets the
 * watched address and the address read, on every hit (the host dedupes). */
void psp_mem_watch_read(uint32_t addr);
/* Every guest write goes to fn while it is set (NULL = off). */
void psp_mem_trace_writes(void (*fn)(uint32_t addr, uint32_t width, uint32_t value));
void psp_mem_set_read_watch_hook(void (*fn)(uint32_t watched, uint32_t addr));

uint8_t  psp_read8 (uint32_t addr);
uint16_t psp_read16(uint32_t addr);
uint32_t psp_read32(uint32_t addr);
float    psp_read_f32(uint32_t addr);

void psp_write8 (uint32_t addr, uint8_t  val);
void psp_write16(uint32_t addr, uint16_t val);
void psp_write32(uint32_t addr, uint32_t val);
void psp_write_f32(uint32_t addr, float val);

/* PSPRECOMP_INLINE_MEM (defined by a host for the generated code only): the
 * main-RAM case of each load and store inline, the rest (VRAM, scratchpad,
 * watches, traces, a separately mapped module, bad addresses) through the
 * functions above. The generated code keeps the MIPS registers in psp_cpu,
 * and an opaque call per load/store makes the compiler write every live one
 * back before it and reload it after; inline, they stay in host registers
 * across loads. Measured on the 3DS port, where that cost dominated the
 * game's own time (its port/docs/3DS_PROGRESS.md, T4).
 * psp_mem_fast_rd / _wr are psp_mem.ram while nothing needs the slow path for
 * reads / writes, else NULL (mem.c keeps them current). */
extern uint8_t *psp_mem_fast_rd, *psp_mem_fast_wr;
#if defined(PSPRECOMP_INLINE_MEM)
#  if defined(__GNUC__)
#    define PSP_MEM_INLINE static inline __attribute__((always_inline))
#    define PSP_MEM_LIKELY(x) __builtin_expect(!!(x), 1)
#  else
#    define PSP_MEM_INLINE static __forceinline
#    define PSP_MEM_LIKELY(x) (x)
#  endif
#  define PSP_MEM_RD(NAME, TYPE)                                                     \
    PSP_MEM_INLINE TYPE NAME##_inl(uint32_t addr) {                                  \
        const uint32_t off = (addr & PSP_ADDR_MASK) - PSP_RAM_BASE;                  \
        uint8_t *const b = psp_mem_fast_rd;                                          \
        if (PSP_MEM_LIKELY(b && off <= PSP_RAM_SIZE - (uint32_t)sizeof(TYPE))) {     \
            TYPE v; memcpy(&v, b + off, sizeof v); return v;                         \
        }                                                                            \
        return NAME(addr);                                                           \
    }
#  define PSP_MEM_WR(NAME, TYPE)                                                     \
    PSP_MEM_INLINE void NAME##_inl(uint32_t addr, TYPE val) {                        \
        const uint32_t off = (addr & PSP_ADDR_MASK) - PSP_RAM_BASE;                  \
        uint8_t *const b = psp_mem_fast_wr;                                          \
        if (PSP_MEM_LIKELY(b && off <= PSP_RAM_SIZE - (uint32_t)sizeof(TYPE))) {     \
            memcpy(b + off, &val, sizeof val); return;                               \
        }                                                                            \
        NAME(addr, val);                                                             \
    }
PSP_MEM_RD(psp_read8, uint8_t)
PSP_MEM_RD(psp_read16, uint16_t)
PSP_MEM_RD(psp_read32, uint32_t)
PSP_MEM_RD(psp_read_f32, float)
PSP_MEM_WR(psp_write8, uint8_t)
PSP_MEM_WR(psp_write16, uint16_t)
PSP_MEM_WR(psp_write32, uint32_t)
PSP_MEM_WR(psp_write_f32, float)
#  define psp_read8(a)        psp_read8_inl(a)
#  define psp_read16(a)       psp_read16_inl(a)
#  define psp_read32(a)       psp_read32_inl(a)
#  define psp_read_f32(a)     psp_read_f32_inl(a)
#  define psp_write8(a, v)    psp_write8_inl(a, v)
#  define psp_write16(a, v)   psp_write16_inl(a, v)
#  define psp_write32(a, v)   psp_write32_inl(a, v)
#  define psp_write_f32(a, v) psp_write_f32_inl(a, v)
#endif

/* Bulk copy into guest memory — used by the loader to place PRX segments. */
int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len);
/* How many psp_mem_write_block copies so far (file reads land in memory
 * this way): a host's sign that the game is loading, e.g. to re-check cached
 * textures every frame while it does. */
uint32_t psp_mem_block_writes(void);
/* Of those, the ones that look like loading rather than a stream refilling
 * one buffer (mem.c says how they are told apart). */
uint32_t psp_mem_load_writes(void);
int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECOMP_MEM_H */
