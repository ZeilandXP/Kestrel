/* hw/kestrel/kglx.h -- GLX for Xkestrel (kglx.c) */
#ifndef KGLX_H
#define KGLX_H

/* From InitOutput, before the extensions start: GLX for direct-rendering
 * clients (does nothing when the server is built without GLX) */
void kGlxInit(void);

#endif
