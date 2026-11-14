/* include/kernel/keymap.h -- keyboard layouts for the text terminals
 * (kernel/keymap.c) */
#ifndef KESTREL_KEYMAP_H
#define KESTREL_KEYMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void   keymap_init(void);                       /* kestrel.keymap= */
/* The character of the key at `pos` (PC scancode, set 1) with MOD_* mods,
 * as UTF-8 in out: its length, 0 for a key the layout leaves out. */
size_t keymap_translate(uint8_t pos, uint8_t mods, char out[4]);
bool   keymap_handles(uint8_t pos);             /* a key of the main block */
bool   keymap_has_altgr(void);                  /* right Alt is AltGr, not Alt */
uint8_t keymap_hid_pos(uint8_t usage);          /* USB usage -> position, 0 if none */
const char *keymap_name(void);
int    keymap_set(const char *name);            /* 0, or -1 if unknown */
size_t keymap_list(char *buf, size_t cap);      /* "de German\n"... */

#endif
