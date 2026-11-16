/*
 * hw/kestrel/kglx.c -- GLX for Xkestrel: direct rendering only
 *
 * Programs that draw with OpenGL through GLX (glxgears, SDL's default
 * OpenGL path, many games and toolkits) need the server's GLX extension
 * even when they render themselves: libGL asks the server for its version,
 * its fbconfigs and visuals, and has it create context and drawable ids.
 * Mesa then renders in the client, direct rendering, and shows each frame
 * by itself: with llvmpipe through MIT-SHM or PutImage (drisw), on a GPU
 * through DRI3 and Present (kdri3.c).
 *
 * The server renders nothing with OpenGL: it has no GL library (Xkestrel is
 * a static musl program), so indirect contexts, which XLibre refuses by
 * default anyway (+iglx turns them on elsewhere), cannot be made here. The
 * provider below offers the fbconfigs Mesa's drivers have (8-bit RGB with
 * and without alpha, single and double buffered, no depth or 16/24-bit
 * depth with or without an 8-bit stencil, sRGB-capable or not), and libGL
 * keeps those its driver can match.
 *
 * Kestrel's own code: MIT licence.
 */
#include <dix-config.h>

#ifdef GLXEXT
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>
#include <GL/glxtokens.h>

#include "include/extinit.h"
#include "Xext/glx/glxserver.h"
#include "Xext/glx/glxutil.h"
#include "Xext/glx/glxscreens.h"
#include "Xext/glx/glxdrawable.h"
#include "Xext/glx/glxcontext.h"
#include "include/glx_extinit.h"
#include "kglx.h"

#define KGLX_MAX_PBUFFER    8192

static __GLXconfig *
kglx_config(unsigned visualType, int alpha, int db, int depth, int stencil, int srgb, GLboolean comp)
{
    __GLXconfig *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->doubleBufferMode = db;
    c->redBits = c->greenBits = c->blueBits = 8;
    c->alphaBits = alpha;
    c->redMask = 0x00ff0000;
    c->greenMask = 0x0000ff00;
    c->blueMask = 0x000000ff;
    c->alphaMask = alpha ? 0xff000000 : 0;
    c->rgbBits = 24 + alpha;
    c->depthBits = depth;
    c->stencilBits = stencil;
    c->visualType = visualType;
    c->visualRating = GLX_NONE;
    c->transparentPixel = GLX_NONE;
    c->drawableType = GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT;
    c->renderType = GLX_RGBA_BIT;
    c->maxPbufferWidth = c->maxPbufferHeight = KGLX_MAX_PBUFFER;
    c->maxPbufferPixels = KGLX_MAX_PBUFFER * KGLX_MAX_PBUFFER;
    c->swapMethod = GLX_SWAP_UNDEFINED_OML;
    c->bindToTextureRgb = GL_TRUE;
    c->bindToTextureRgba = GL_TRUE;
    c->bindToMipmapTexture = GL_FALSE;
    c->bindToTextureTargets = GLX_TEXTURE_1D_BIT_EXT | GLX_TEXTURE_2D_BIT_EXT | GLX_TEXTURE_RECTANGLE_BIT_EXT;
    c->yInverted = GL_TRUE;
    c->sRGBCapable = srgb;
    c->duplicatedForComp = comp;
    return c;
}

/* The same set as glx_dri/glxdricommon.c makes from a driver's configs:
 * TrueColor, then DirectColor, then the copies for the 32-bit visual
 * Composite adds (8-bit alpha only). */
static __GLXconfig *
kglx_configs(void)
{
    static const int ds[][2] = { { 24, 8 }, { 0, 0 }, { 16, 0 }, { 24, 0 } };
    __GLXconfig head = { 0 }, *tail = &head;
    for (int pass = 0; pass < 3; pass++) {
        GLboolean comp = pass == 2;
        if (comp && noCompositeExtension)
            break;
        for (int alpha = 8; alpha >= 0; alpha -= 8)
            for (int db = 1; db >= 0; db--)
                for (unsigned i = 0; i < sizeof ds / sizeof ds[0]; i++)
                    for (int srgb = 0; srgb < 2; srgb++) {
                        if (comp && !alpha)
                            continue;
                        tail->next = kglx_config(pass == 1 ? GLX_DIRECT_COLOR : GLX_TRUE_COLOR, alpha, db,
                                                 ds[i][0], ds[i][1], srgb, comp);
                        if (!tail->next)
                            return head.next;
                        tail = tail->next;
                    }
    }
    return head.next;
}

/* ---- drawables: ids for the client; it draws them itself ------------------------------- */
static void
kglx_drawable_destroy(__GLXdrawable *d)
{
    free(d);
}

static GLboolean
kglx_swap_buffers(ClientPtr client, __GLXdrawable *d)
{
    return GL_TRUE;             /* only indirect contexts ask the server to swap */
}

static __GLXdrawable *
kglx_create_drawable(ClientPtr client, __GLXscreen *screen, DrawablePtr pDraw, XID drawId,
                     int type, XID glxDrawId, __GLXconfig *config)
{
    __GLXdrawable *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    if (!__glXDrawableInit(d, screen, pDraw, type, glxDrawId, config)) {
        free(d);
        return NULL;
    }
    d->destroy = kglx_drawable_destroy;
    d->swapBuffers = kglx_swap_buffers;
    return d;
}

/* Indirect contexts: none (no GL in the server) */
static __GLXcontext *
kglx_create_context(__GLXscreen *screen, __GLXconfig *config, __GLXcontext *share,
                    unsigned num_attribs, const uint32_t *attribs, int *error)
{
    *error = BadValue;
    return NULL;
}

static void
kglx_screen_destroy(__GLXscreen *screen)
{
    __GLXconfig *c = screen->fbconfigs;
    __glXScreenDestroy(screen);
    while (c) {
        __GLXconfig *next = c->next;
        free(c);
        c = next;
    }
    free(screen);
}

static __GLXscreen *
kglx_probe(ScreenPtr pScreen)
{
    __GLXscreen *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->destroy = kglx_screen_destroy;
    s->createContext = kglx_create_context;
    s->createDrawable = kglx_create_drawable;
    s->swapInterval = NULL;
    s->fbconfigs = kglx_configs();
    if (!s->fbconfigs) {
        free(s);
        return NULL;
    }
    /* what Mesa's drivers do for direct contexts; libGL also needs the
     * server to name these before it offers them */
    static const char *const ext[] = {
        "GLX_ARB_create_context", "GLX_ARB_create_context_no_error", "GLX_ARB_create_context_profile",
        "GLX_EXT_create_context_es_profile", "GLX_EXT_create_context_es2_profile",
        "GLX_ARB_create_context_robustness", "GLX_ARB_context_flush_control",
        "GLX_EXT_framebuffer_sRGB", "GLX_ARB_fbconfig_float", "GLX_EXT_fbconfig_packed_float",
        "GLX_EXT_texture_from_pixmap", "GLX_MESA_copy_sub_buffer", "GLX_EXT_no_config_context",
        "GLX_SGI_swap_control", "GLX_INTEL_swap_event",
    };
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        __glXEnableExtension(s->glx_enable_bits, ext[i]);
    s->glvnd = strdup("mesa");  /* libglvnd's libGL then loads Mesa's GLX */
    __glXScreenInit(s, pScreen);
    LogMessage(X_INFO, "kestrel: GLX for direct rendering, %d fbconfigs, %d visuals\n",
               s->numFBConfigs, s->numVisuals);
    return s;
}

static __GLXprovider kglx_provider = {
    kglx_probe,
    "Kestrel (direct rendering)",
    NULL
};

/* From InitOutput, every server generation: the callback that sets GLX up
 * on each screen goes away at a reset; the provider stays pushed. */
void
kGlxInit(void)
{
    static Bool pushed;
    if (!pushed) {
        GlxPushProvider(&kglx_provider);
        pushed = TRUE;
    }
    xorgGlxCreateVendor();
}
#else
#include "kglx.h"

void
kGlxInit(void)
{
}
#endif
