/* kernel/keymap.c -- keyboard layouts for the text terminals
 *
 * The PS/2 and USB keyboard drivers pass the key's position (its PC
 * scancode, set 1) and the modifiers here, and get the character back as
 * UTF-8. The text terminal decodes UTF-8, so a German keyboard types ü
 * and ß in a shell as it does on Linux. The layout comes from
 * kestrel.keymap= on the kernel command line, and /proc/sys/kernel/keymap
 * reads and changes it (the installer sets it while it runs, and
 * /etc/init.d/rcS from /etc/default/keyboard at each boot).
 *
 * Each layout lists, for each of the four rows of the main block, the
 * characters of its keys at four levels: plain, Shift, AltGr and
 * Shift+AltGr (a space: nothing there). Accent keys type the accent
 * itself: there are no dead keys yet. The desktop has its own layouts
 * (XKB); startx sets the same one there with setxkbmap. */
#include <kernel/keymap.h>
#include <kernel/keyboard.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/bootinfo.h>

/* The keys of each row, by scancode. Row 2 ends with the key above Enter
 * (\ on US keyboards, # on ISO ones); row 4 starts with the ISO key left
 * of Z (< > on most European keyboards). */
static const uint8_t row_keys[4][13] = {
    { 0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D },
    { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x2B },
    { 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28 },
    { 0x56, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35 },
};
static const uint8_t row_len[4] = { 13, 13, 11, 11 };

struct layout_src {
    const char *name, *desc;
    const char *rows[4][4];             /* [row][level] */
};

static const struct layout_src sources[] = {
    { "us", "English (US)", {
        { "`1234567890-=", "~!@#$%^&*()_+", "", "" },
        { "qwertyuiop[]\\", "QWERTYUIOP{}|", "", "" },
        { "asdfghjkl;'", "ASDFGHJKL:\"", "", "" },
        { "\\zxcvbnm,./", "|ZXCVBNM<>?", "", "" } } },
    { "gb", "English (UK)", {
        { "`1234567890-=", "¬!\"£$%^&*()_+", "¦   €", "" },
        { "qwertyuiop[]#", "QWERTYUIOP{}~", "", "" },
        { "asdfghjkl;'", "ASDFGHJKL:@", "", "" },
        { "\\zxcvbnm,./", "|ZXCVBNM<>?", "", "" } } },
    { "de", "German", {
        { "^1234567890ß´", "°!\"§$%&/()=?`", "  ²³   {[]}\\", "" },
        { "qwertzuiopü+#", "QWERTZUIOPÜ*'", "@ €        ~", "" },
        { "asdfghjklöä", "ASDFGHJKLÖÄ", "", "" },
        { "<yxcvbnm,.-", ">YXCVBNM;:_", "|      µ", "" } } },
    { "fr", "French (AZERTY)", {
        { "²&é\"'(-è_çà)=", " 1234567890°+", "  ~#{[|`\\^@]}", "" },
        { "azertyuiop^$*", "AZERTYUIOP¨£µ", "  €        ¤", "" },
        { "qsdfghjklmù", "QSDFGHJKLM%", "", "" },
        { "<wxcvbn,;:!", ">WXCVBN?./§", "", "" } } },
    { "es", "Spanish", {
        { "º1234567890'¡", "ª!\"·$%&/()=?¿", "\\|@#~€¬", "" },
        { "qwertyuiop`+ç", "QWERTYUIOP^*Ç", "  €       []}", "" },
        { "asdfghjklñ´", "ASDFGHJKLÑ¨", "          {", "" },
        { "<zxcvbnm,.-", ">ZXCVBNM;:_", "", "" } } },
    { "it", "Italian", {
        { "\\1234567890'ì", "|!\"£$%&/()=?^", "", "" },
        { "qwertyuiopè+ù", "QWERTYUIOPé*§", "  €       []", "" },
        { "asdfghjklòà", "ASDFGHJKLç°", "         @#", "" },
        { "<zxcvbnm,.-", ">ZXCVBNM;:_", "", "" } } },
};
#define NLAYOUTS (sizeof sources / sizeof *sources)

#define NPOS 0x58
static uint16_t table[NLAYOUTS][NPOS][4];       /* Unicode (BMP), 0: nothing */
static int current;                             /* index into sources */
static bool built;

static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3Fu >> more;
    while (more-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3Fu);
    *s = (const char *)p;
    return c;
}

static void build(void)
{
    for (size_t l = 0; l < NLAYOUTS; l++)
        for (int r = 0; r < 4; r++)
            for (int lv = 0; lv < 4; lv++) {
                const char *s = sources[l].rows[r][lv];
                for (int k = 0; k < row_len[r] && *s; k++) {
                    uint32_t c = utf8_next(&s);
                    if (c != ' ' && c < 0x10000) table[l][row_keys[r][k]][lv] = (uint16_t)c;
                }
            }
    built = true;
}

static size_t put_utf8(uint32_t c, char out[4])
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

/* Caps Lock acts on a key whose Shift character is the plain one in
 * capitals (a-z, and the accented letters of Latin-1). */
static bool is_letter(uint16_t plain, uint16_t shifted)
{
    if (plain >= 'a' && plain <= 'z') return shifted == plain - 32;
    return plain >= 0xE0 && plain <= 0xFE && plain != 0xF7 && shifted == plain - 32;
}

size_t keymap_translate(uint8_t pos, uint8_t mods, char out[4])
{
    if (!built) build();
    if (pos >= NPOS) return 0;
    const uint16_t *k = table[current][pos];
    if (!k[0] && !k[1] && !k[2]) return 0;
    bool shift = mods & MOD_SHIFT;
    if ((mods & MOD_CAPS) && is_letter(k[0], k[1])) shift = !shift;
    uint16_t c;
    if (mods & MOD_ALTGR) c = k[shift ? 3 : 2] ? k[shift ? 3 : 2] : k[2];
    else c = k[shift ? 1 : 0];
    if (!c) return 0;
    if ((mods & MOD_CTRL) && c < 0x80 && ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
        c = (uint16_t)((c | 0x20) - 'a' + 1);   /* Ctrl+letter -> 0x01..0x1A */
    return put_utf8(c, out);
}

bool keymap_handles(uint8_t pos)
{
    if (!built) build();
    return pos < NPOS && (table[current][pos][0] || table[current][pos][1]);
}

bool keymap_has_altgr(void) { return current != 0; }

const char *keymap_name(void) { return sources[current].name; }

int keymap_set(const char *name)
{
    for (size_t i = 0; i < NLAYOUTS; i++)
        if (!strcmp(sources[i].name, name)) {
            if ((int)i != current) kprintf("keymap: %s (%s)\n", sources[i].name, sources[i].desc);
            current = (int)i;
            return 0;
        }
    return -1;
}

size_t keymap_list(char *buf, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < NLAYOUTS; i++)
        n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0, "%s %s\n", sources[i].name, sources[i].desc);
    return n;
}

void keymap_init(void)
{
    const char *s = strstr(g_boot.cmdline, "kestrel.keymap=");
    if (!s || (s != g_boot.cmdline && s[-1] != ' ')) return;
    char name[8];
    size_t n = 0;
    for (s += 15; *s && *s != ' ' && n + 1 < sizeof name; s++) name[n++] = *s;
    name[n] = 0;
    if (keymap_set(name) < 0) kprintf("keymap: unknown kestrel.keymap=%s, keeping us\n", name);
}

/* USB keyboards report HID usages: the main block's keys, by position. */
uint8_t keymap_hid_pos(uint8_t usage)
{
    static const uint8_t letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    };
    if (usage >= 0x04 && usage <= 0x1D) return letters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x27) return (uint8_t)(0x02 + usage - 0x1E);
    switch (usage) {
    case 0x2D: return 0x0C; case 0x2E: return 0x0D; case 0x2F: return 0x1A; case 0x30: return 0x1B;
    case 0x31: case 0x32: return 0x2B;
    case 0x33: return 0x27; case 0x34: return 0x28; case 0x35: return 0x29;
    case 0x36: return 0x33; case 0x37: return 0x34; case 0x38: return 0x35;
    case 0x64: return 0x56;
    }
    return 0;
}
