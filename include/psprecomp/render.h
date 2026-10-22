/* psprecomp � render backend interface. See docs/RENDERER.md.
 *
 * Display-list *interpretation* has one implementation (src/hle/ge.c);
 * *presentation* is pluggable. The split matters because the software
 * rasterizer is the oracle every other backend is checked against, and it has
 * to keep working on a machine with no GPU.
 *
 * Primitives arrive assembled � vertex format decoding happens once, in the
 * interpreter, rather than being duplicated (and mis-duplicated) per backend.
 */
#ifndef PSPRECOMP_RENDER_H
#define PSPRECOMP_RENDER_H

#include <stdint.h>

/* A vertex after format decoding: screen space, colour resolved.
 * Texture coordinates extend this rather than replacing it. */
typedef struct {
    int      x, y;
    uint32_t rgba;
} psp_vertex;

/* GE primitive types, from the PRIM argument's type field. */
enum {
    PSP_PRIM_POINTS = 0,
    PSP_PRIM_LINES,
    PSP_PRIM_LINE_STRIP,
    PSP_PRIM_TRIANGLES,
    PSP_PRIM_TRIANGLE_STRIP,
    PSP_PRIM_TRIANGLE_FAN,
    PSP_PRIM_SPRITES
};

typedef struct {
    const char *name;

    int  (*init)(int width, int height);
    void (*shutdown)(void);

    /* GE_FBP / GE_FBW: the framebuffer being drawn into. */
    void (*set_target)(uint32_t addr, uint32_t stride, int fmt);

    /* One assembled primitive. `count` vertices, already in screen space. */
    void (*draw)(int prim, const psp_vertex *v, int count);

    /* End of a display list � a natural point to flush batched work. */
    void (*finish)(void);

    /* sceDisplaySetFrameBuf � show what has accumulated. */
    void (*present)(void);
} psp_render_backend;

/* Select a backend by name ("software", "null", ...). Returns 0 on success,
 * -1 if the name is unknown, leaving the current backend in place. */
int psp_render_select(const char *name);

/* The active backend. Never NULL � defaults to software. */
const psp_render_backend *psp_render_current(void);

/* Backends provided by the runtime. */
extern const psp_render_backend psp_render_software;
extern const psp_render_backend psp_render_null;

/* Pixels written by the active backend -- the proof of life during bring-up. */
uint64_t psp_render_pixels(void);
void     psp_render_reset_pixels(void);

/* ---- hardware rasterizer backend ---------------------------------------------
 *
 * The GE pipeline in src/hle/gpu.c always does vertex work on the CPU
 * (decoding, transforms, near-plane clipping, culling). With a hardware backend
 * installed, it hands each draw over as a triangle list in screen space plus
 * the decoded draw state, instead of rasterizing in software. The backend owns
 * any copy of VRAM it keeps on the GPU and must keep emulated VRAM coherent
 * through sync_vram / vram_written. */

typedef struct {
    float x, y;            /* screen pixels (pixel centres at +0.5) */
    float z;               /* depth 0..65535 */
    float rhw;             /* 1/w (1 in through mode) */
    float u, v;            /* texel coordinates */
    float r, g, b, a;      /* 0..255 */
} psp_gpu_vertex;

typedef struct {
    uint32_t fb_addr, fb_stride;      /* colour buffer (VRAM address, pixels per row) */
    int      fb_fmt;                  /* 0 5650, 1 5551, 2 4444, 3 8888 */
    uint32_t zb_addr, zb_stride;      /* depth buffer; zb_stride 0 = none */
    int      sx0, sy0, sx1, sy1;      /* scissor, inclusive */
    int      clear;                   /* clear mode */
    uint32_t clear_which;             /* 1 colour, 2 alpha/stencil, 4 depth */
    int      tex;                     /* texturing on */
    uint32_t tex_addr, tex_w, tex_h, tex_bufw;
    int      tex_psm, tex_swz;
    uint64_t tex_key;                 /* identifies format, size and CLUT state */
    int      tex_linear, tex_clamp_u, tex_clamp_v;
    int      tfunc, trgba, tdbl;
    uint32_t env;                     /* texture environment colour, 0xBBGGRR */
    int      atest; int afunc; uint32_t aref, amask;
    int      ztest; int zfunc; int zwrite;
    int      blend; int bsrc, bdst, beq; uint32_t fixa, fixb;
    uint32_t mask_rgb, mask_a;        /* write masks: set bits are preserved */
} psp_gpu_state;

typedef struct {
    const char *name;
    /* Draw a triangle list. Returns 0 on success; nonzero makes the caller
     * rasterize this draw in software instead. */
    int  (*draw)(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
    /* Bring emulated VRAM up to date for [addr, addr+bytes) (CPU or display
     * is about to read it). */
    void (*sync_vram)(uint32_t addr, uint32_t bytes);
    /* Emulated VRAM in [addr, addr+bytes) was written outside the backend. */
    void (*vram_written)(uint32_t addr, uint32_t bytes);
    /* Nonzero: the backend keeps VRAM coherent on every CPU access itself
     * (psp_mem_set_vram_hook), so a GE sync needs no flush. */
    int  coherent_on_access;
    /* Optional (stereo, psp_gpu_set_stereo): the draw just handed to draw()
     * again for one eye (0 left, 1 right), with the same state and that eye's
     * vertices. Only after draw() accepted it. NULL = no stereo. */
    int  (*draw_eye)(int eye, const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
    /* Optional (stereo with eye_views): a HUD draw (see psp_gpu_stereo), as
     * the game made it, into the backend's own HUD picture instead of the
     * target (which never gets it). 0 if drawn. NULL = the HUD stays in the
     * eyes. */
    int  (*draw_hud)(const psp_gpu_state *st, const psp_gpu_vertex *v, int nverts);
} psp_gpu_backend;

/* Stereo for the host's VR: after each draw the backend accepted, the GE
 * draws it again once per eye (draw_eye). Perspective draws move per eye:
 * an eye at view-space x = -half_ipd (left) or +half_ipd (right), with an
 * off-axis shift so that points at view distance `converge` have no
 * disparity. Through-mode and orthographic draws are the same in both eyes.
 * Units are the game's view-space units. NULL or on = 0: off.
 * flat_after_clear: once a target that took moved (perspective) draws is
 * cleared again within a frame, the rest of the frame stays flat too (games
 * that draw their HUD in 3D after a second clear, as PSP2i does).
 * mono_is_left: the backend's own draw (draw()) is the left eye's and
 * draw_eye is called for the right eye only: two pictures instead of three,
 * at the price of VRAM and the flat picture showing the left eye.
 * eye_views (head tracking): instead of the IPD shift, each eye sees the
 * view-space vertices through its own rigid 4x4 eye_view[eye] (column-major,
 * the game's view space to the eye's, game units); half_ipd and converge are
 * then unused. on_frame, if set, is called at the first moved draw of each
 * frame (the draws between two flips) to fill eye_view and tag; the whole
 * frame keeps them, so one frame never mixes two head poses. tag is the
 * host's name for that pose (psp_gpu_stereo_tag).
 * fov_scale, fov_scale_y (with eye_views; <= 1: off, and fov_scale_y <= 1
 * takes fov_scale): the eyes see that many times the game's view across and
 * up (the tangents of its FOV).
 * With eye_views, the HUD pass (after the second clear of the 3D target,
 * flat_after_clear) is split: its 2D draws and the perspective ones whose
 * clip w the host's hud_panel accepts (a game's HUD and menu layers) go, as
 * the game drew them, to the backend's draw_hud for the host's HUD panel; the
 * other perspective ones (world labels already projected for the game
 * camera) are seen from the eyes along their own direction. Small
 * orthographic draws in the scene pass on the 3D target (screen-space
 * effects) are placed in the world fx_dist away (game units; <= 0: 50)
 * through the scene's projection. */
typedef struct psp_gpu_stereo {
    int   on;
    float half_ipd;
    float converge;
    int   flat_after_clear;
    int   mono_is_left;
    int   eye_views;
    float eye_view[2][16];
    float fov_scale, fov_scale_y;
    int   (*hud_panel)(float clip_w);
    float fx_dist;
    /* Set by on_frame: this frame is drawn as the game drew it, the same in
     * both eyes (a menu scene the host shows on a flat screen). */
    int   flat_frame;
    float proj[16];       /* on_frame: the PROJ of the frame's first moved draw (filled by the GE) */
    uint32_t tag;
    void (*on_frame)(struct psp_gpu_stereo *s);
} psp_gpu_stereo;
void psp_gpu_set_stereo(const psp_gpu_stereo *s);
/* The horizontal half-FOV tangent (1/PROJ[0]) of the last perspective draw
 * replayed for the eyes, 0 if none yet. */
float psp_gpu_stereo_tan_half_fov(void);
/* eye_views: the tag of the frame being drawn, 0 while it has had no moved
 * draw yet (or eye_views is off). Backends keep it with their eye pictures. */
uint32_t psp_gpu_stereo_tag(void);
/* A dev trace of one frame's stereo decisions into `out` (a FILE *): the
 * eye matrices, and each draw that isn't a moved 3D scene draw with what
 * the eyes do with it (screen box and scissor before and after, left eye).
 * Starts at the next flip, stops at the one after; the caller closes `out`
 * once psp_gpu_stereo_tracing() is 0. */
void psp_gpu_stereo_trace(void *out);
int  psp_gpu_stereo_tracing(void);

void psp_gpu_set_backend(const psp_gpu_backend *be);   /* NULL = software */
const psp_gpu_backend *psp_gpu_get_backend(void);

/* GE sync point (sceGeDrawSync/ListSync): bring emulated VRAM up to date with
 * the hardware backend's render targets, and have the targets re-check VRAM
 * for CPU writes before their next draw. No-op for the software rasterizer. */
void psp_gpu_cpu_sync(void);

/* Diagnostics: describe every draw of the next whole frame (target, vertex
 * format, state, first vertices) into the text file `path`. */
void psp_gpu_dump_next_frame(const char *path);

/* For backends: decode the current texture (level 0, as the draw state
 * describes it) into tex_w*tex_h RGBA8 pixels, 0xAABBGGRR. */
void psp_gpu_decode_texture(uint32_t *out);
/* For backends: a hash of the current texture's bytes (and CLUT). */
uint64_t psp_gpu_texture_hash(void);
/* For backends that load PPSSPP texture packs (textures.ini, hash = xxh64):
 * the current texture's key as PPSSPP computes it, so a pack's [hashes]
 * lines "AAAAAAAACCCCCCCCDDDDDDDD" match bit for bit. *addr is the address
 * (A, zero in packs with ignoreAddress), *clut_key is C (the texture size
 * register, XOR the CLUT hash for CLUT formats), and hashes[] gets D, the
 * hash of the texture bytes: usually one, two for a 512-tall texture in
 * through mode (PPSSPP's maxSeenV), most likely first. Returns how many.
 * Call it from the backend's draw, where the draw state is current. */
int psp_gpu_pack_key(uint32_t *addr, uint32_t *clut_key, uint32_t hashes[2]);

/* A draw observer (the host's VR and reverse-engineering tools): called for
 * every primitive batch the GE draws, after its vertices were transformed and
 * before the backend draws them, with the matrices it was drawn with. The
 * pointers are valid only during the call. Through-mode (2D) draws have
 * through = 1 and no meaningful matrices. NULL (the default) = none. */
typedef struct {
    const float *world;     /* 4x3, column-major as the GE loads it (12 floats) */
    const float *view;      /* 4x3 (12 floats) */
    const float *proj;      /* 4x4 (16 floats) */
    const float *bones;     /* nbones 4x3 bone matrices when skinned, else NULL */
    int      nbones;        /* weights per vertex (bones used), 0 if not skinned */
    int      through;       /* 2D: screen coordinates, no transform */
    int      prim;          /* GE primitive type (0 points .. 6 sprites) */
    int      nverts;
    uint32_t vaddr;         /* the vertex data's guest address */
    uint32_t vtype;         /* the vertex format (GE VTYPE) */
    uint32_t fb_addr;       /* the colour target (VRAM offset, as psp_gpu_state) */
    uint32_t tex_addr;      /* level-0 texture address, 0 when untextured */
} psp_gpu_draw_info;
void psp_gpu_set_draw_observer(void (*fn)(const psp_gpu_draw_info *));

#endif