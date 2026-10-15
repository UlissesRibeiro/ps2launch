/*
 * PS2 XMB Launcher - launcher de .ELF com visual estilo XMB (PS3), leve para o GS do PS2.
 * USB via stack BDM (bdm + bdmfs_fatfs + usbmass_bd): FAT32 e exFAT,
 * varios dispositivos ao mesmo tempo (mass0:, mass1:, ...).
 *
 * Otimizacoes: framebuffer 16 bits, fundo opaco (sem blending), escurecimento do PNG
 * por modulacao de cor (sem camada translucida), blending so em icones/texto,
 * poucos desenhos por frame e strings em cache.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <libpad.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <elf-loader.h>
#include "config.h"

#define IRX(n) extern unsigned char n##_irx[]; extern unsigned int size_##n##_irx
IRX(iomanX); IRX(fileXio); IRX(bdm); IRX(bdmfs_fatfs); IRX(usbd); IRX(usbmass_bd);

#define MAX_MASS 8
#define FONT_W   13.0f   /* largura aproximada por caractere da FONTM (escala 1.0) */

static GSGLOBAL *gs;
static GSFONTM  *font;
static Config    cfg;
static GSTEXTURE bgTex, iconTex[MAX_ENTRIES], catTex[MAX_CATS];
static int       hasBg, hasIcon[MAX_ENTRIES], hasCat[MAX_CATS];
static int       catItems[MAX_CATS][MAX_ENTRIES], catCnt[MAX_CATS];
static char      padBuf[256] __attribute__((aligned(64)));

/* ---------- utilidades ---------- */
static u64 C(u32 rgb, int a)
{
    return GS_SETREG_RGBAQ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a, 0);
}

static void blend(int on)
{
    gs->PrimAlphaEnable = on ? GS_SETTING_ON : GS_SETTING_OFF;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int mass_present(int n)
{
    char p[16];
    snprintf(p, sizeof(p), "mass%d:/", n);
    DIR *d = opendir(p);
    if (!d) return 0;
    closedir(d);
    return 1;
}

static int mass_mask(void)
{
    int m = 0;
    for (int i = 0; i < MAX_MASS; i++)
        if (mass_present(i)) m |= 1 << i;
    return m;
}

/* "mass:/X" ou "mass*:/X" = procura em TODOS os massN (FAT32 e exFAT juntos).
 * Varios candidatos separados por '|' sao testados em ordem. */
static int resolve_path(const char *spec, char *out, int sz)
{
    char tmp[512];
    strncpy(tmp, spec, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
        while (*t == ' ') t++;
        const char *rest = NULL;
        if (!strncmp(t, "mass*:", 6)) rest = t + 6;
        else if (!strncmp(t, "mass:", 5)) rest = t + 5;
        if (rest) {
            for (int n = 0; n < MAX_MASS; n++) {
                snprintf(out, sz, "mass%d:%s", n, rest);
                if (file_exists(out)) return 1;
            }
        } else {
            snprintf(out, sz, "%s", t);
            if (file_exists(out)) return 1;
        }
    }
    return 0;
}

/* ---------- IOP / modulos ---------- */
static void load_buf(void *irx, unsigned int size)
{
    int ret;
    SifExecModuleBuffer(irx, size, 0, NULL, &ret);
}

static void init_iop(void)
{
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    SifLoadModule("rom0:MCMAN", 0, NULL);
    SifLoadModule("rom0:MCSERV", 0, NULL);

    load_buf(iomanX_irx, size_iomanX_irx);
    load_buf(fileXio_irx, size_fileXio_irx);
    load_buf(bdm_irx, size_bdm_irx);
    load_buf(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx);   /* FAT12/16/32 + exFAT */
    load_buf(usbd_irx, size_usbd_irx);
    load_buf(usbmass_bd_irx, size_usbmass_bd_irx);
}

/* ---------- graficos ---------- */
static void init_gfx(void)
{
    gs = gsKit_init_global();
    gs->PSM = GS_PSM_CT16;            /* 16 bits: metade da banda de memoria do GS */
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gs->Dithering = GS_SETTING_ON;    /* disfarca o banding do degrade em 16 bits */

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    font = gsKit_init_fontm();
    gsKit_fontm_upload(gs, font);
}

static int load_tex(GSTEXTURE *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    if (gsKit_texture_png(gs, t, (char *)path) < 0) return 0;
    t->Filter = GS_FILTER_LINEAR;
    t->Vram = gsKit_vram_alloc(gs, gsKit_texture_size(t->Width, t->Height, t->PSM), GSKIT_ALLOC_USERBUFFER);
    if (t->Vram == GSKIT_ALLOC_ERROR) { free(t->Mem); return 0; }
    gsKit_texture_upload(gs, t);
    free(t->Mem);
    t->Mem = NULL;
    return 1;
}

static void load_theme(void)
{
    char p[256];
    gsKit_vram_clear(gs);
    hasBg = 0;
    if (cfg.background[0]) {
        config_rel(&cfg, cfg.background, p, sizeof(p));
        hasBg = load_tex(&bgTex, p);
    }
    for (int c = 0; c < cfg.ncat; c++) {
        hasCat[c] = 0;
        if (cfg.cats[c].icon[0]) {
            config_rel(&cfg, cfg.cats[c].icon, p, sizeof(p));
            hasCat[c] = load_tex(&catTex[c], p);
        }
        catCnt[c] = 0;
    }
    for (int i = 0; i < cfg.count; i++) {
        hasIcon[i] = 0;
        if (cfg.entries[i].icon[0]) {
            config_rel(&cfg, cfg.entries[i].icon, p, sizeof(p));
            hasIcon[i] = load_tex(&iconTex[i], p);
        }
        int c = cfg.entries[i].cat;
        catItems[c][catCnt[c]++] = i;
    }
}

static void text(float x, float y, float scale, u32 rgb, const char *s)
{
    gsKit_fontm_print_scaled(gs, font, x, y, 3, scale, C(rgb, 0x80), (char *)s);
}

static void text_c(float cx, float y, float scale, u32 rgb, const char *s)
{
    text(cx - strlen(s) * FONT_W * scale * 0.5f, y, scale, rgb, s);
}

static void rect(float x, float y, float w, float h, u64 c)
{
    gsKit_prim_sprite(gs, x, y, x + w, y + h, 2, c);
}

/* fundo: degrade opaco (1 quad) + PNG opcional escurecido por modulacao de cor */
static void draw_background(float t)
{
    float W = gs->Width, H = gs->Height;
    blend(0);
    gsKit_prim_quad_gouraud(gs, 0, 0, W, 0, 0, H, W, H, 1,
                            C(cfg.bg_top, 0x80), C(cfg.bg_top, 0x80),
                            C(cfg.bg_bottom, 0x80), C(cfg.bg_bottom, 0x80));
    if (hasBg) {
        int m = 128 - cfg.bg_dim;
        gsKit_prim_sprite_texture(gs, &bgTex, 0, 0, 0, 0, W, H, bgTex.Width, bgTex.Height, 1,
                                  GS_SETREG_RGBAQ(m, m, m, 0x80, 0));
    }
    if (cfg.waves) {                       /* opcional: 2 faixas de onda (~32 sprites) */
        blend(1);
        for (int w = 0; w < 2; w++) {
            float base = H * (0.62f + 0.12f * w), amp = 16.0f + 8.0f * w;
            for (float x = 0; x < W; x += 40) {
                float y = base + sinf(x * 0.012f + t * (0.6f + 0.3f * w) + w * 2.0f) * amp;
                rect(x, y, 40, 10 + 6 * w, C(cfg.accent, 0x10 + 0x08 * w));
            }
        }
    }
    blend(1);
}

static void draw_icon(GSTEXTURE *tex, int has, float x, float y, float size, int selected, const char *title)
{
    float h = size * 0.5f;
    if (selected) {                        /* moldura simples na cor de destaque (sem glow) */
        blend(0);
        rect(x - h - 2, y - h - 2, size + 4, size + 4, C(cfg.accent, 0x80));
        blend(1);
    }
    if (has) {
        gsKit_prim_sprite_texture(gs, tex, x - h, y - h, 0, 0, x + h, y + h,
                                  tex->Width, tex->Height, 3,
                                  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
    } else {
        blend(0);
        rect(x - h, y - h, size, size, C(0x1F2638, 0x80));
        blend(1);
        char ch[2] = { title[0] ? title[0] : '?', 0 };
        text_c(x, y - 8 * (size / 48.0f), size / 40.0f, cfg.accent, ch);
    }
}

/* ---------- pad ---------- */
static void init_pad(void)
{
    padInit(0);
    padPortOpen(0, 0, padBuf);
}

static u32 read_pad(void)
{
    static u32 old = 0;
    struct padButtonStatus b;
    int st = padGetState(0, 0);
    if (st != PAD_STATE_STABLE && st != PAD_STATE_FINDCTP1) return 0;
    if (!padRead(0, 0, &b)) return 0;
    u32 now = 0xFFFF ^ b.btns;
    u32 pressed = now & ~old;
    old = now;
    return pressed;
}

/* ---------- config ---------- */
static int find_config(const char *boot, char *out, int sz)
{
    static const char *names[] = { "PS5LAUNCH/launcher.ini", "launcher.ini", "PS5LAUNCH/launcher.cfg", "launcher.cfg" };
    char dir[128] = "";
    if (boot) {
        strncpy(dir, boot, sizeof(dir) - 1);
        char *s = strrchr(dir, '/');
        if (s) s[1] = 0; else dir[0] = 0;
    }
    if (dir[0]) {
        for (int n = 0; n < 4; n++) {
            snprintf(out, sz, "%s%s", dir, names[n]);
            if (file_exists(out)) return 1;
        }
    }
    for (int m = 0; m < MAX_MASS; m++)
        for (int n = 0; n < 4; n++) {
            snprintf(out, sz, "mass%d:/%s", m, names[n]);
            if (file_exists(out)) return 1;
        }
    for (int m = 0; m < 2; m++)
        for (int n = 0; n < 4; n++) {
            snprintf(out, sz, "mc%d:/%s", m, names[n]);
            if (file_exists(out)) return 1;
        }
    return 0;
}

static void make_status(char *st, int sz, int mask)
{
    snprintf(st, sz, "USB:");
    for (int i = 0; i < MAX_MASS; i++)
        if (mask & (1 << i)) { char b[8]; snprintf(b, sizeof(b), " %d", i); strncat(st, b, sz - strlen(st) - 1); }
    if (!mask) strncat(st, " --", sz - strlen(st) - 1);
}

/* ---------- main ---------- */
enum { ST_SCAN, ST_MENU };

int main(int argc, char *argv[])
{
    init_iop();
    init_gfx();
    init_pad();
    config_defaults(&cfg);

    int state = ST_SCAN, frame = 0, mask = 0, settle = -1;
    int cfgOk = 0, selCat = 0, selItem[MAX_CATS] = { 0 }, msgTimer = 0;
    float fCat = 0, fItem = 0, t = 0;
    char msg[96] = "", cfgPath[160] = "", status[64] = "USB: --";
    const char *boot = (argc > 0) ? argv[0] : NULL;

    for (;;) {
        u32 pad = read_pad();
        frame++;
        t += 1.0f / 60.0f;

        if (state == ST_SCAN) {
            /* Nunca bloqueia: tenta a cada 0.5s, espera um pouco apos achar
             * o 1o dispositivo (para os demais montarem) e segue em frente. */
            if (frame % 30 == 0) {
                int m = mass_mask();
                if (m != mask) { mask = m; settle = frame + 90; }
            }
            int timeout = frame > cfg.usb_wait_ms * 60 / 1000;
            int settled = (settle > 0 && frame >= settle);
            if (timeout || settled || (pad & PAD_CROSS)) {
                cfgOk = find_config(boot, cfgPath, sizeof(cfgPath)) && config_load(&cfg, cfgPath) == 0;
                if (cfgOk) load_theme();
                else snprintf(msg, sizeof(msg), "launcher.ini nao encontrado");
                msgTimer = cfgOk ? 0 : 99999;
                make_status(status, sizeof(status), mask);
                selCat = 0; fCat = 0; fItem = 0;
                memset(selItem, 0, sizeof(selItem));
                state = ST_MENU;
            }
        } else {
            if ((pad & PAD_RIGHT) && selCat < cfg.ncat - 1) { selCat++; fItem = selItem[selCat]; }
            if ((pad & PAD_LEFT) && selCat > 0)             { selCat--; fItem = selItem[selCat]; }
            if ((pad & PAD_DOWN) && selItem[selCat] < catCnt[selCat] - 1) selItem[selCat]++;
            if ((pad & PAD_UP) && selItem[selCat] > 0) selItem[selCat]--;

            if (pad & (PAD_R1 | PAD_TRIANGLE)) {      /* rescan: recarrega dispositivos + .ini */
                state = ST_SCAN; frame = 0; settle = -1; mask = 0; msg[0] = 0;
                continue;
            }
            if ((pad & PAD_CROSS) && catCnt[selCat] > 0) {
                int idx = catItems[selCat][selItem[selCat]];
                char real[200];
                if (resolve_path(cfg.entries[idx].paths, real, sizeof(real))) {
                    char *args[1] = { real };
                    padPortClose(0, 0);
                    padEnd();
                    LoadELFFromFile(real, 1, args);   /* so retorna em caso de erro */
                    init_pad();
                    snprintf(msg, sizeof(msg), "Falha ao iniciar o ELF");
                } else {
                    snprintf(msg, sizeof(msg), "ELF nao encontrado");
                }
                msgTimer = 180;
            }
            fCat += (selCat - fCat) * 0.25f;
            fItem += (selItem[selCat] - fItem) * 0.25f;
            if (msgTimer > 0 && msgTimer < 99999) msgTimer--;
        }

        /* ---------- desenho ---------- */
        draw_background(t);
        float W = gs->Width, H = gs->Height;

        if (state == ST_SCAN) {
            static const char *dots[] = { "", ".", "..", "..." };
            char s[96];
            snprintf(s, sizeof(s), "Detectando dispositivos de armazenamento%s", dots[(frame / 20) & 3]);
            text_c(W / 2, H / 2 - 20, 0.9f, cfg.text, s);
            snprintf(s, sizeof(s), "Encontrados: %d    (X = continuar agora)", __builtin_popcount(mask));
            text_c(W / 2, H / 2 + 10, 0.7f, cfg.text_dim, s);
        } else {
            text(30, 24, 0.8f, cfg.text, cfg.header);
            text(W - 30 - strlen(status) * FONT_W * 0.65f, 26, 0.65f, cfg.text_dim, status);

            /* ---- linha de categorias (horizontal) ---- */
            float cy = H * 0.24f, base = cfg.icon_size, big = base * cfg.selected_scale / 100.0f;
            float cgap = base * 0.8f, cext = (big - base) * 0.5f + 12;
            for (int c = 0; c < cfg.ncat; c++) {
                float d = c - fCat, ad = fabsf(d);
                float k = ad < 1.0f ? 1.0f - ad : 0.0f;
                float x = W / 2 + d * (base + cgap) + (d < 0 ? -1 : 1) * (ad < 1.0f ? ad : 1.0f) * cext;
                if (x < -60 || x > W + 60) continue;
                draw_icon(&catTex[c], hasCat[c], x, cy, base + (big - base) * k, 0, cfg.cats[c].title);
            }
            text_c(W / 2, cy + big * 0.5f + 8, 0.8f, cfg.text, cfg.cats[selCat].title);

            /* ---- lista vertical de apps da categoria selecionada ---- */
            float isz = cfg.item_size, ibig = isz * 1.35f;
            float y0 = cy + big * 0.5f + 70, gap = isz + 18, iext = (ibig - isz) * 0.5f + 8;
            float ix = W / 2 - 60;
            for (int k = 0; k < catCnt[selCat]; k++) {
                float d = k - fItem, ad = fabsf(d);
                float y = y0 + d * gap + (d < 0 ? -1 : 1) * (ad < 1.0f ? ad : 1.0f) * iext;
                if (y < cy + big * 0.5f + 40 || y > H - 50) continue;
                float kk = ad < 1.0f ? 1.0f - ad : 0.0f;
                float sz = isz + (ibig - isz) * kk;
                int idx = catItems[selCat][k];
                draw_icon(&iconTex[idx], hasIcon[idx], ix, y, sz, k == selItem[selCat], cfg.entries[idx].title);
                if (k == selItem[selCat]) text(ix + sz * 0.5f + 14, y - 8, 0.95f, cfg.text, cfg.entries[idx].title);
                else text(ix + sz * 0.5f + 14, y - 6, 0.7f, cfg.text_dim, cfg.entries[idx].title);
            }
            if (catCnt[selCat] == 0)
                text_c(W / 2, y0 - 8, 0.75f, cfg.text_dim, "(categoria vazia)");

            if (msg[0] && msgTimer > 0) text_c(W / 2, H - 70, 0.8f, cfg.accent, msg);
            text_c(W / 2, H - 34, 0.55f, cfg.text_dim, "X Abrir    R1 Recarregar    < > Categoria    ^ v App");
        }

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    return 0;
}