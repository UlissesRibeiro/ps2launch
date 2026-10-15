/*
 * PS2 Launcher - launcher de .ELF com visual proprio ("Roda"), leve para o GS do PS2.
 * USB via stack BDM (bdm + bdmfs_fatfs + usbmass_bd): FAT32 e exFAT,
 * varios dispositivos ao mesmo tempo (mass0:, mass1:, ...).
 *
 * Visual: abas de categoria no topo, uma roda de apps no lado esquerdo (arco com
 * marcas que giram) e um painel de detalhes com cantos chanfrados a direita.
 * Tudo e desenhado com primitivas do gsKit (quads/triangulos/linhas): nenhum PNG
 * e obrigatorio. Sem PNG de fundo/icone, o sistema usa o visual padrao procedural.
 *
 * Otimizacoes: framebuffer 16 bits, fundo opaco (sem blending), escurecimento do PNG
 * por modulacao de cor, ~100 primitivas por frame, sem alocacao dentro do loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
/* Fonte (FONTM da ROM): cada glifo ocupa uma celula de 26x26 px na escala 1.0.
 * FONT_SP   = espacamento entre letras (gsKit: font->Spacing; 1.0 = celula inteira; menor = mais junto)
 * fontK     = fator global de tamanho (cfg.font_scale / 100) - ajustavel no menu de configuracoes
 * FONT_W    = avanco horizontal por caractere, por unidade de escala */
#define FONT_CELL 26.0f
#define FONT_SP   0.65f
static float fontK = 1.0f;
#define FONT_W    (FONT_CELL * FONT_SP * fontK)
#define WHEEL_R  270.0f  /* raio da roda de apps */
#define STEP     0.25f   /* angulo (rad) entre dois apps na roda */
#define PANEL_X(W) ((W) * 0.52f)

static GSGLOBAL *gs;
static GSFONTM  *font;
static Config    cfg;
static GSTEXTURE bgTex, iconTex[MAX_ENTRIES], catTex[MAX_CATS];
static int       hasBg, hasIcon[MAX_ENTRIES], hasCat[MAX_CATS];

/* botoes do controle (X, O, quadrado, triangulo): PNG opcional na pasta buttons/ (cross.png etc.);
 * sem PNG sao desenhados por codigo */
enum { BTN_NONE = -1, BTN_CROSS, BTN_CIRCLE, BTN_SQUARE, BTN_TRIANGLE, BTN_COUNT };
static const u32   BTN_COL[BTN_COUNT]  = { 0x6EA8FE, 0xFF6B6B, 0xF08FC0, 0x5BD6A0 };
static const char *BTN_FILE[BTN_COUNT] = { "buttons/cross.png", "buttons/circle.png",
                                           "buttons/square.png", "buttons/triangle.png" };
static GSTEXTURE btnTex[BTN_COUNT];
static int       hasBtn[BTN_COUNT];
static int       catItems[MAX_CATS][MAX_ENTRIES], catCnt[MAX_CATS];
static char      padBuf[256] __attribute__((aligned(64)));

/* estado do menu */
static int   cfgOk, selCat, selItem[MAX_CATS], msgTimer;
static float fCat, fItem;
static char  msg[96];
static char  cfgPath[160], bootDir[128];
static int   usingDefaults, selSet;

/* paleta usada nos icones padrao (quando nao ha PNG) */
static const u32 PAL[8] = { 0x4FA3FF, 0xFF6B6B, 0x6BCB77, 0xFFC857, 0xB388FF, 0x2EC4B6, 0xFF8FA3, 0xF79256 };

/* ---------- utilidades ---------- */
static u64 C(u32 rgb, int a)
{
    if (a < 0) a = 0;
    if (a > 0x80) a = 0x80;
    return GS_SETREG_RGBAQ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a, 0);
}

static void blend(int on)
{
    gs->PrimAlphaEnable = on ? GS_SETTING_ON : GS_SETTING_OFF;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static u32 shade(u32 rgb, int pct)
{
    int r = (int)((rgb >> 16) & 255) * pct / 100;
    int g = (int)((rgb >> 8) & 255) * pct / 100;
    int b = (int)(rgb & 255) * pct / 100;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static u32 mix(u32 a, u32 b, float t)
{
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

static u64 grad(u32 top, int at, u32 bot, int ab, float t)
{
    return C(mix(top, bot, t), at + (int)((ab - at) * t));
}

static u32 name_color(const char *s)
{
    u32 h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return PAL[(h >> 3) & 7];
}

/* copia ate maxc caracteres; se cortar, termina com ".." */
static void clip_copy(char *dst, int sz, const char *src, int maxc)
{
    int n = (int)strlen(src);
    if (maxc > sz - 1) maxc = sz - 1;
    if (maxc < 1) maxc = 1;
    if (n <= maxc) { memcpy(dst, src, n + 1); return; }
    memcpy(dst, src, maxc);
    dst[maxc] = 0;
    if (maxc >= 3) { dst[maxc - 1] = '.'; dst[maxc - 2] = '.'; }
}

/* largura real (px) de um texto desenhado na escala dada */
static float text_w(const char *s, float scale)
{
    int n = (int)strlen(s);
    if (n <= 0) return 0.0f;
    return (n - 1) * FONT_W * scale + FONT_CELL * fontK * scale;
}

/* quantos caracteres cabem em w px na escala dada */
static int max_chars(float w, float scale)
{
    int n = (int)((w - FONT_CELL * fontK * scale) / (FONT_W * scale)) + 1;
    return n < 1 ? 1 : n;
}

static float fit_scale(const char *s, float maxw, float maxs, float mins)
{
    float w = text_w(s, 1.0f);
    if (w < 1.0f) return maxs;
    return clampf(maxw / w, mins, maxs);
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
    /* o blend padrao do gsKit (BACK2FRONT) inverte o alpha; este e o blend normal:
     * resultado = Cs*As + Cd*(1-As) */
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    font = gsKit_init_fontm();
    gsKit_fontm_upload(gs, font);
    font->Spacing = FONT_SP;          /* o upload zera para 1.0: ajustar DEPOIS */
}

/* PNG ausente ou invalido => devolve 0 e o desenho usa o visual padrao.
 * IMPORTANTE: gsKit_texture_png (Delayed = 0) JA aloca a VRAM, envia a textura e libera a
 * memoria da RAM. Alocar/enviar de novo aqui mandaria lixo (Mem == NULL) para a VRAM. */
static int load_tex(GSTEXTURE *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    if (gsKit_texture_png(gs, t, (char *)path) < 0) {
        if (t->Mem) free(t->Mem);                  /* falha ao alocar VRAM: nao vaza a RAM */
        memset(t, 0, sizeof(*t));
        return 0;
    }
    t->Filter = GS_FILTER_LINEAR;
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
    for (int b = 0; b < BTN_COUNT; b++) {          /* opcional: ausente => botao desenhado por codigo */
        config_rel(&cfg, BTN_FILE[b], p, sizeof(p));
        hasBtn[b] = load_tex(&btnTex[b], p);
    }
}

/* ---------- texto e formas ---------- */
static void text_a(float x, float y, float scale, u32 rgb, int a, const char *s)
{
    gsKit_fontm_print_scaled(gs, font, x, y, 3, scale * fontK, C(rgb, a), (char *)s);
}

static void text(float x, float y, float scale, u32 rgb, const char *s)
{
    text_a(x, y, scale, rgb, 0x80, s);
}

static void text_ca(float cx, float y, float scale, u32 rgb, int a, const char *s)
{
    text_a(cx - text_w(s, scale) * 0.5f, y, scale, rgb, a, s);
}

static void text_c(float cx, float y, float scale, u32 rgb, const char *s)
{
    text_ca(cx, y, scale, rgb, 0x80, s);
}

/* centralizado, reduzindo a escala se nao couber em maxw */
static void text_fit_c(float cx, float y, float maxw, float maxs, u32 rgb, const char *s)
{
    text_ca(cx, y, fit_scale(s, maxw, maxs, 0.3f), rgb, 0x80, s);
}

static void rect(float x, float y, float w, float h, u64 c)
{
    gsKit_prim_sprite(gs, x, y, x + w, y + h, 2, c);
}

/* linha "grossa" (2 px): evita cintilar no modo entrelacado */
static void line2(float x1, float y1, float x2, float y2, u64 c)
{
    gsKit_prim_line(gs, x1, y1, x2, y2, 2, c);
    gsKit_prim_line(gs, x1 + 1, y1 + 1, x2 + 1, y2 + 1, 2, c);
}

/* retangulo com cantos chanfrados (superior esquerdo e inferior direito),
 * preenchido com degrade vertical. 4 triangulos. */
static void chamfer_fill(float x0, float y0, float x1, float y1, float c,
                         u32 top, int at, u32 bot, int ab)
{
    float h = y1 - y0;
    if (h < 1.0f) h = 1.0f;
    float ax = x0 + c, ay = y0;
    float bx = x1,     by = y0;
    float qx = x1,     qy = y1 - c;
    float dx = x1 - c, dy = y1;
    float ex = x0,     ey = y1;
    float fx = x0,     fy = y0 + c;
    u64 cA = grad(top, at, bot, ab, 0.0f);
    u64 cQ = grad(top, at, bot, ab, (h - c) / h);
    u64 cD = grad(top, at, bot, ab, 1.0f);
    u64 cF = grad(top, at, bot, ab, c / h);
    gsKit_prim_triangle_gouraud(gs, ax, ay, bx, by, qx, qy, 2, cA, cA, cQ);
    gsKit_prim_triangle_gouraud(gs, ax, ay, qx, qy, dx, dy, 2, cA, cQ, cD);
    gsKit_prim_triangle_gouraud(gs, ax, ay, dx, dy, ex, ey, 2, cA, cD, cD);
    gsKit_prim_triangle_gouraud(gs, ax, ay, ex, ey, fx, fy, 2, cA, cD, cF);
}

static void chamfer_edge(float x0, float y0, float x1, float y1, float c, u64 col)
{
    line2(x0 + c, y0, x1, y0, col);
    line2(x1, y0, x1, y1 - c, col);
    line2(x1, y1 - c, x1 - c, y1, col);
    line2(x1 - c, y1, x0, y1, col);
    line2(x0, y1, x0, y0 + c, col);
    line2(x0, y0 + c, x0 + c, y0, col);
}

/* icone: PNG se existir; senao um bloco chanfrado colorido com a inicial do nome */
static void draw_icon(GSTEXTURE *tex, int has, float x, float y, float size, int a, const char *title)
{
    float h = size * 0.5f;
    if (has) {
        gsKit_prim_sprite_texture(gs, tex, x - h, y - h, 0, 0, x + h, y + h,
                                  tex->Width, tex->Height, 3,
                                  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, a, 0));
    } else {
        u32 col = name_color(title);
        chamfer_fill(x - h, y - h, x + h, y + h, size * 0.22f, shade(col, 115), a, shade(col, 55), a);
        char ch[2] = { title[0] ? title[0] : '?', 0 };
        if (ch[0] >= 'a' && ch[0] <= 'z') ch[0] -= 32;
        text_ca(x, y - 13.0f * (size / 40.0f), size / 40.0f / fontK, 0xFFFFFF, a, ch);
    }
}

/* botao do controle de diametro d: PNG se existir, senao disco + aro + simbolo colorido */
static void draw_btn(int kind, float cx, float cy, float d, int a)
{
    float r = d * 0.5f;
    if (hasBtn[kind]) {
        gsKit_prim_sprite_texture(gs, &btnTex[kind], cx - r, cy - r, 0, 0, cx + r, cy + r,
                                  btnTex[kind].Width, btnTex[kind].Height, 3,
                                  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, a, 0));
        return;
    }
    u32 col = BTN_COL[kind];
    u64 cd = C(0x0A0F1C, a * 3 / 4), cl = C(col, a);
    float px = cx + r, py = cy;
    for (int i = 1; i <= 14; i++) {                /* disco escuro + aro */
        float ang = i * (6.2832f / 14.0f);
        float nx = cx + cosf(ang) * r, ny = cy + sinf(ang) * r;
        gsKit_prim_triangle(gs, cx, cy, px, py, nx, ny, 2, cd);
        line2(px, py, nx, ny, cl);
        px = nx; py = ny;
    }
    float s = r * 0.42f;                           /* simbolo */
    switch (kind) {
    case BTN_CROSS:
        line2(cx - s, cy - s, cx + s, cy + s, cl);
        line2(cx + s, cy - s, cx - s, cy + s, cl);
        break;
    case BTN_CIRCLE: {
        float qx = cx + s, qy = cy;
        for (int i = 1; i <= 10; i++) {
            float ang = i * (6.2832f / 10.0f);
            float nx = cx + cosf(ang) * s, ny = cy + sinf(ang) * s;
            line2(qx, qy, nx, ny, cl);
            qx = nx; qy = ny;
        }
        break; }
    case BTN_SQUARE:
        line2(cx - s, cy - s, cx + s, cy - s, cl);
        line2(cx + s, cy - s, cx + s, cy + s, cl);
        line2(cx + s, cy + s, cx - s, cy + s, cl);
        line2(cx - s, cy + s, cx - s, cy - s, cl);
        break;
    case BTN_TRIANGLE:
        line2(cx, cy - s, cx + s, cy + s * 0.8f, cl);
        line2(cx + s, cy + s * 0.8f, cx - s, cy + s * 0.8f, cl);
        line2(cx - s, cy + s * 0.8f, cx, cy - s, cl);
        break;
    }
}

/* linha de dicas: itens com botao (btn >= 0) ou so texto (btn = BTN_NONE) */
typedef struct { int btn; const char *label; } Hint;

static void draw_hints(float W, float y, float maxs, const Hint *h, int n)
{
    const float gap = 18.0f, maxw = W - 40.0f;
    float sc = maxs, total = 0, d = 0;
    for (; sc > 0.3f; sc -= 0.02f) {
        d = FONT_CELL * fontK * sc;
        total = gap * (n - 1);
        for (int i = 0; i < n; i++)
            total += (h[i].btn >= 0 ? d + 6.0f : 0.0f) + text_w(h[i].label, sc);
        if (total <= maxw) break;
    }
    d = FONT_CELL * fontK * sc;
    float x = (W - total) * 0.5f;
    for (int i = 0; i < n; i++) {
        if (h[i].btn >= 0) { draw_btn(h[i].btn, x + d * 0.5f, y + d * 0.5f, d, 0x80); x += d + 6.0f; }
        text_a(x, y, sc, cfg.text_dim, 0x80, h[i].label);
        x += text_w(h[i].label, sc) + gap;
    }
}

/* fundo: degrade opaco (1 quad) + PNG opcional + faixas diagonais translucidas */
static void draw_background(float W, float H, float t)
{
    blend(0);
    gsKit_prim_quad_gouraud(gs, 0, 0, W, 0, 0, H, W, H, 1,
                            C(cfg.bg_top, 0x80), C(cfg.bg_top, 0x80),
                            C(cfg.bg_bottom, 0x80), C(cfg.bg_bottom, 0x80));
    if (hasBg) {
        int m = 128 - cfg.bg_dim;
        gsKit_prim_sprite_texture(gs, &bgTex, 0, 0, 0, 0, W, H, bgTex.Width, bgTex.Height, 1,
                                  GS_SETREG_RGBAQ(m, m, m, 0x80, 0));
    }
    blend(1);

    /* decoracao padrao: 2 cunhas com degrade de alpha (so 2 triangulos) */
    int a = 0x1C + (int)(sinf(t * 0.8f) * 6.0f);
    gsKit_prim_triangle_gouraud(gs, W * 0.46f, 0, W, 0, W, H * 0.82f, 1,
                                C(cfg.accent, a), C(cfg.accent, a / 2), C(cfg.accent, 0));
    gsKit_prim_triangle_gouraud(gs, 0, H * 0.50f, 0, H, W * 0.42f, H, 1,
                                C(cfg.accent, 0), C(cfg.accent, a), C(cfg.accent, a / 2));

    if (cfg.waves) {                       /* opcional: 2 faixas de onda (~32 sprites) */
        for (int w = 0; w < 2; w++) {
            float base = H * (0.62f + 0.12f * w), amp = 16.0f + 8.0f * w;
            for (float x = 0; x < W; x += 40) {
                float y = base + sinf(x * 0.012f + t * (0.6f + 0.3f * w) + w * 2.0f) * amp;
                rect(x, y, 40, 10 + 6 * w, C(cfg.accent, 0x10 + 0x08 * w));
            }
        }
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

/* carrega o launcher.ini; se nao existir ou for invalido, usa o padrao interno
 * (mesmo conteudo do launcher.ini de exemplo). PNGs ausentes -> visual procedural. */
static void load_config_or_defaults(const char *boot)
{
    usingDefaults = 0;
    cfgPath[0] = 0;
    if (find_config(boot, cfgPath, sizeof(cfgPath)) && config_load(&cfg, cfgPath) == 0) {
        cfgOk = 1;
    } else {
        cfgPath[0] = 0;
        config_builtin(&cfg, bootDir);
        cfgOk = 1;
        usingDefaults = 1;
    }
    load_theme();
}

static void make_status(char *st, int sz, int mask)
{
    snprintf(st, sz, "USB:");
    for (int i = 0; i < MAX_MASS; i++)
        if (mask & (1 << i)) { char b[8]; snprintf(b, sizeof(b), " %d", i); strncat(st, b, sz - strlen(st) - 1); }
    if (!mask) strncat(st, " --", sz - strlen(st) - 1);
}

/* ---------- telas ---------- */
static void draw_spinner(float x, float y, int frame)
{
    int lead = (frame / 3) % 12;
    for (int i = 0; i < 12; i++) {
        float a = i * (6.2832f / 12.0f);
        int dist = (lead - i + 12) % 12;
        int al = 0x80 - dist * 9;
        if (al < 0x10) al = 0x10;
        rect(x + cosf(a) * 22.0f - 3, y + sinf(a) * 22.0f - 3, 6, 6, C(cfg.accent, al));
    }
}

/* abas das categorias (topo): a barra de destaque desliza entre as abas */
static void draw_tabs(float W)
{
    if (cfg.ncat == 0) return;
    const float ts = 0.75f, ty = 52.0f;
    float tw[MAX_CATS], tx[MAX_CATS], total = 0;
    for (int c = 0; c < cfg.ncat; c++) {
        tw[c] = text_w(cfg.cats[c].title, ts) + (hasCat[c] ? 30.0f : 0.0f) + 30.0f;
        tx[c] = total;
        total += tw[c];
    }
    int a = (int)floorf(fCat);
    if (a < 0) a = 0;
    if (a > cfg.ncat - 1) a = cfg.ncat - 1;
    int b = (a + 1 < cfg.ncat) ? a + 1 : a;
    float f = clampf(fCat - a, 0.0f, 1.0f);
    float sl = tx[a] + (tx[b] - tx[a]) * f, sw = tw[a] + (tw[b] - tw[a]) * f;
    float start = (total <= W - 40.0f) ? (W - total) * 0.5f
                                       : clampf(W * 0.5f - (sl + sw * 0.5f), W - 20.0f - total, 20.0f);

    rect(20, ty + 24, W - 40, 2, C(cfg.text_dim, 0x18));
    for (int c = 0; c < cfg.ncat; c++) {
        float x = start + tx[c] + 15.0f;
        int on = fabsf(c - fCat) < 0.5f;
        if (hasCat[c]) {
            draw_icon(&catTex[c], 1, x + 12, ty + 8, 24, on ? 0x80 : 0x50, "");
            x += 30.0f;
        }
        text_a(x, ty, ts, on ? cfg.text : cfg.text_dim, 0x80, cfg.cats[c].title);
    }
    rect(start + sl + 10.0f, ty + 24, sw - 20.0f, 3, C(cfg.accent, 0x80));
}

/* roda de apps (esquerda): arco + marcas que giram + icones sobre o arco */
static void draw_wheel(float W, float H, float t)
{
    int n = catCnt[selCat];
    float cy = H * 0.56f, selx = W * 0.19f, cx = selx - WHEEL_R;
    float big = cfg.item_size * 1.6f, small = big * 0.62f;
    float top = 100.0f, bot = H - 52.0f;

    /* arco guia */
    u64 ring = C(cfg.accent, 0x28);
    float lx = cx + WHEEL_R * cosf(-1.0f), ly = cy + WHEEL_R * sinf(-1.0f);
    for (int i = 1; i <= 20; i++) {
        float a = -1.0f + 0.1f * i;
        float x = cx + WHEEL_R * cosf(a), y = cy + WHEEL_R * sinf(a);
        if (ly > top && ly < bot && y > top && y < bot) line2(lx, ly, x, y, ring);
        lx = x; ly = y;
    }
    /* marcas entre os itens: acompanham fItem, dando a sensacao de giro */
    float frac = fItem - floorf(fItem);
    for (int j = -4; j <= 4; j++) {
        float a = (j + 0.5f - frac) * STEP;
        float y = cy + WHEEL_R * sinf(a);
        if (y < top || y > bot) continue;
        int al = (int)(0x40 * clampf(1.0f - fabsf(a), 0.0f, 1.0f));
        line2(cx + (WHEEL_R - 7) * cosf(a), cy + (WHEEL_R - 7) * sinf(a),
              cx + (WHEEL_R + 7) * cosf(a), y + 0.0f + 7.0f * sinf(a), C(cfg.accent, al));
    }

    if (n == 0) return;

    /* conector do item selecionado ate o painel */
    float cxs = selx + big * 0.5f + 14.0f;
    rect(cxs, cy - 1, PANEL_X(W) - 6.0f - cxs, 2, C(cfg.accent, 0x40));

    for (int k = 0; k < n; k++) {
        float d = k - fItem, ad = fabsf(d);
        if (ad > 2.6f) continue;
        float a = d * STEP;
        float x = cx + WHEEL_R * cosf(a), y = cy + WHEEL_R * sinf(a);
        if (y < top || y > bot) continue;
        float u = ad < 1.0f ? ad : 1.0f;
        float sz = big - (big - small) * u, h = sz * 0.5f;
        int al = (int)(0x80 * clampf(1.15f - ad * 0.4f, 0.0f, 1.0f));
        int idx = catItems[selCat][k];
        int sel = (k == selItem[selCat]);

        if (sel) {                                 /* brilho pulsante + moldura chanfrada */
            int g = 0x18 + (int)(sinf(t * 3.0f) * 0x0C);
            chamfer_fill(x - h - 9, y - h - 9, x + h + 9, y + h + 9, 10, cfg.accent, g, cfg.accent, g / 2);
            chamfer_edge(x - h - 4, y - h - 4, x + h + 4, y + h + 4, 6, C(cfg.accent, 0x80));
        }
        draw_icon(&iconTex[idx], hasIcon[idx], x, y, sz, al, cfg.entries[idx].title);
        if (!sel) {                                /* o nome do selecionado fica no painel */
            char lb[20];
            clip_copy(lb, sizeof(lb), cfg.entries[idx].title, 15);
            text_a(x + h + 12.0f, y - 13.0f * 0.62f * fontK, 0.62f, cfg.text_dim, al, lb);
        }
    }
}

/* painel de detalhes (direita) */
static void draw_panel(float W, float H)
{
    float cy = H * 0.56f, psz = (float)cfg.icon_size;
    float ph = psz + 108.0f, x0 = PANEL_X(W), x1 = W - 28.0f, y0 = cy - ph * 0.5f, y1 = y0 + ph, c = 16.0f;
    float ix = x0 + 22.0f, iy = y0 + 22.0f, tw = x1 - x0 - 44.0f;

    chamfer_fill(x0, y0, x1, y1, c, shade(cfg.bg_top, 80), 0x60, shade(cfg.bg_bottom, 50), 0x70);
    chamfer_edge(x0, y0, x1, y1, c, C(cfg.accent, 0x60));
    rect(x0 + c, y0, 70, 3, C(cfg.accent, 0x80));

    if (catCnt[selCat] == 0) {
        text_c((x0 + x1) * 0.5f, cy - 10, 0.75f, cfg.text_dim, "(categoria vazia)");
        return;
    }

    int idx = catItems[selCat][selItem[selCat]];
    Entry *e = &cfg.entries[idx];
    draw_icon(&iconTex[idx], hasIcon[idx], ix + psz * 0.5f, iy + psz * 0.5f, psz, 0x80, e->title);

    float tx = ix + psz + 16.0f;
    char up[32], cnt[24];
    int i = 0;
    for (; cfg.cats[selCat].title[i] && i < (int)sizeof(up) - 1; i++)
        up[i] = (char)toupper((unsigned char)cfg.cats[selCat].title[i]);
    up[i] = 0;
    text_a(tx, iy + 4, 0.65f, cfg.accent, 0x80, up);
    snprintf(cnt, sizeof(cnt), "%d / %d", selItem[selCat] + 1, catCnt[selCat]);
    text_a(tx, iy + 26, 0.65f, cfg.text_dim, 0x80, cnt);

    float ty = iy + psz + 14.0f;
    float sc = fit_scale(e->title, tw, 1.0f, 0.5f);
    char tt[48];
    clip_copy(tt, sizeof(tt), e->title, max_chars(tw, sc));
    text_a(ix, ty, sc, cfg.text, 0x80, tt);

    /* primeiro caminho configurado (+ quantos alternativos existem) */
    char p1[96], pc[96];
    const char *bar = strchr(e->paths, '|');
    int len = bar ? (int)(bar - e->paths) : (int)strlen(e->paths);
    if (len > (int)sizeof(p1) - 1) len = (int)sizeof(p1) - 1;
    memcpy(p1, e->paths, len);
    p1[len] = 0;
    int alt = 0;
    for (const char *s = e->paths; *s; s++) if (*s == '|') alt++;
    clip_copy(pc, sizeof(pc), p1, max_chars(tw, 0.5f));
    text_a(ix, ty + 30.0f, 0.5f, cfg.text_dim, 0x80, pc);
    if (alt > 0) {
        char s[40];
        snprintf(s, sizeof(s), "+%d caminho(s) alternativo(s)", alt);
        text_a(ix, ty + 45.0f, 0.45f, cfg.accent, 0x60, s);
    }
}

/* ---------- configuracoes ---------- */
typedef struct { const char *name; u32 top, bot; } BgPreset;
typedef struct { const char *name; u32 rgb; } ColPreset;

static const BgPreset BG_PRESETS[] = {
    { "Azul noite", 0x1B2A4A, 0x05070F }, { "Roxo",    0x2E1A47, 0x0A0512 },
    { "Verde",      0x12362B, 0x03100B }, { "Vinho",   0x40161C, 0x10040A },
    { "Grafite",    0x2A2D34, 0x0B0C0F }, { "Oceano",  0x0F3A4A, 0x02101A },
};
static const ColPreset ACC_PRESETS[] = {
    { "Azul", 0x4FA3FF }, { "Vermelho", 0xFF6B6B }, { "Verde", 0x6BCB77 }, { "Amarelo", 0xFFC857 },
    { "Roxo", 0xB388FF }, { "Turquesa", 0x2EC4B6 }, { "Rosa",  0xFF8FA3 }, { "Laranja", 0xF79256 },
};
#define N_BG  ((int)(sizeof(BG_PRESETS) / sizeof(BG_PRESETS[0])))
#define N_ACC ((int)(sizeof(ACC_PRESETS) / sizeof(ACC_PRESETS[0])))

enum { SET_ACCENT, SET_BG, SET_DIM, SET_ICON, SET_ITEM, SET_FONT, SET_WAVES, SET_USB, SET_SAVE, SET_RESET, SET_BACK, SET_COUNT };
static const char *SET_LABEL[SET_COUNT] = {
    "Cor de destaque", "Cor de fundo", "Escurecer fundo (PNG)", "Icone do painel", "Icone da roda",
    "Tamanho da fonte", "Ondas animadas", "Espera USB", "Salvar launcher.ini", "Restaurar tema padrao", "Voltar"
};

static int acc_index(void)
{
    for (int i = 0; i < N_ACC; i++) if (ACC_PRESETS[i].rgb == cfg.accent) return i;
    return -1;
}

static int bg_index(void)
{
    for (int i = 0; i < N_BG; i++)
        if (BG_PRESETS[i].top == cfg.bg_top && BG_PRESETS[i].bot == cfg.bg_bottom) return i;
    return -1;
}

static void settings_adjust(int row, int dir)
{
    switch (row) {
    case SET_ACCENT: {
        int i = acc_index();
        i = (i < 0) ? (dir > 0 ? 0 : N_ACC - 1) : (i + dir + N_ACC) % N_ACC;
        cfg.accent = ACC_PRESETS[i].rgb;
        break; }
    case SET_BG: {
        int i = bg_index();
        i = (i < 0) ? (dir > 0 ? 0 : N_BG - 1) : (i + dir + N_BG) % N_BG;
        cfg.bg_top = BG_PRESETS[i].top;
        cfg.bg_bottom = BG_PRESETS[i].bot;
        break; }
    case SET_DIM:   cfg.bg_dim = (int)clampf((float)(cfg.bg_dim + dir * 8), 0, 112); break;
    case SET_ICON:  cfg.icon_size = (int)clampf((float)(cfg.icon_size + dir * 8), 32, 96); break;
    case SET_ITEM:  cfg.item_size = (int)clampf((float)(cfg.item_size + dir * 4), 24, 64); break;
    case SET_FONT:  cfg.font_scale = (int)clampf((float)(cfg.font_scale + dir * 5), 60, 140); break;
    case SET_WAVES: cfg.waves = !cfg.waves; break;
    case SET_USB:   cfg.usb_wait_ms = (int)clampf((float)(cfg.usb_wait_ms + dir * 1000), 2000, 30000); break;
    default: break;
    }
}

/* tenta gravar em: .ini atual -> pasta do ELF -> USB -> memory card */
static int save_config(int mask, char *used, int sz)
{
    char cand[160];
    if (cfgPath[0] && config_save(&cfg, cfgPath) == 0) { snprintf(used, sz, "%s", cfgPath); return 1; }
    if (bootDir[0]) {
        snprintf(cand, sizeof(cand), "%slauncher.ini", bootDir);
        if (config_save(&cfg, cand) == 0) { snprintf(used, sz, "%s", cand); return 1; }
    }
    for (int m = 0; m < MAX_MASS; m++) {
        if (!(mask & (1 << m))) continue;
        snprintf(cand, sizeof(cand), "mass%d:/launcher.ini", m);
        if (config_save(&cfg, cand) == 0) { snprintf(used, sz, "%s", cand); return 1; }
    }
    for (int m = 0; m < 2; m++) {
        snprintf(cand, sizeof(cand), "mc%d:/launcher.ini", m);
        if (config_save(&cfg, cand) == 0) { snprintf(used, sz, "%s", cand); return 1; }
    }
    return 0;
}

static void settings_value(int row, char *out, int sz)
{
    out[0] = 0;
    switch (row) {
    case SET_ACCENT: { int i = acc_index(); snprintf(out, sz, "%s", i < 0 ? "Personalizado" : ACC_PRESETS[i].name); break; }
    case SET_BG:     { int i = bg_index();  snprintf(out, sz, "%s", i < 0 ? "Personalizado" : BG_PRESETS[i].name); break; }
    case SET_DIM:    snprintf(out, sz, "%d", cfg.bg_dim); break;
    case SET_ICON:   snprintf(out, sz, "%d px", cfg.icon_size); break;
    case SET_ITEM:   snprintf(out, sz, "%d px", cfg.item_size); break;
    case SET_FONT:   snprintf(out, sz, "%d%%", cfg.font_scale); break;
    case SET_WAVES:  snprintf(out, sz, "%s", cfg.waves ? "Ligado" : "Desligado"); break;
    case SET_USB:    snprintf(out, sz, "%d s", cfg.usb_wait_ms / 1000); break;
    default: break;
    }
}

static void draw_settings(float W, float H)
{
    const float rowh = 22.0f;
    float x0 = W * 0.16f, x1 = W * 0.84f, y0 = 52.0f, y1 = y0 + 58.0f + SET_COUNT * rowh + 12.0f, c = 16.0f;

    chamfer_fill(x0, y0, x1, y1, c, shade(cfg.bg_top, 80), 0x68, shade(cfg.bg_bottom, 50), 0x78);
    chamfer_edge(x0, y0, x1, y1, c, C(cfg.accent, 0x60));
    rect(x0 + c, y0, 70, 3, C(cfg.accent, 0x80));

    text_a(x0 + 26, y0 + 14, 0.8f, cfg.accent, 0x80, "CONFIGURACOES");
    char sub[64];
    if (usingDefaults) snprintf(sub, sizeof(sub), "Sem launcher.ini: padrao interno em uso");
    else clip_copy(sub, sizeof(sub), cfgPath, max_chars(x1 - x0 - 52, 0.5f));
    text_a(x0 + 26, y0 + 38, 0.5f, cfg.text_dim, 0x80, sub);

    for (int r = 0; r < SET_COUNT; r++) {
        float y = y0 + 58.0f + r * rowh;
        int sel = (r == selSet);
        int action = (r >= SET_SAVE);
        if (sel) {
            rect(x0 + 18, y - 3, x1 - x0 - 36, rowh - 2, C(cfg.accent, 0x20));
            rect(x0 + 18, y - 3, 3, rowh - 2, C(cfg.accent, 0x80));
        }
        text_a(x0 + 30, y, 0.7f, sel ? cfg.text : cfg.text_dim, 0x80, SET_LABEL[r]);
        if (action) continue;

        char v[40], vv[48];
        settings_value(r, v, sizeof(v));
        if (sel) snprintf(vv, sizeof(vv), "< %s >", v); else snprintf(vv, sizeof(vv), "%s", v);
        float vx = x1 - 30.0f - text_w(vv, 0.7f);
        text_a(vx, y, 0.7f, sel ? cfg.accent : cfg.text, 0x80, vv);
        if (r == SET_ACCENT) rect(vx - 22, y - 1, 14, 14, C(cfg.accent, 0x80));
        if (r == SET_BG)
            gsKit_prim_quad_gouraud(gs, vx - 22, y - 1, vx - 8, y - 1, vx - 22, y + 13, vx - 8, y + 13, 2,
                                    C(cfg.bg_top, 0x80), C(cfg.bg_top, 0x80), C(cfg.bg_bottom, 0x80), C(cfg.bg_bottom, 0x80));
    }
    if (msg[0] && msgTimer > 0) text_fit_c(W / 2, y1 + 10, W - 40, 0.65f, cfg.accent, msg);
    static const Hint hints[] = {
        { BTN_NONE, "^ v Item" }, { BTN_NONE, "< > Alterar" },
        { BTN_CROSS, "Confirmar" }, { BTN_CIRCLE, "ou START Voltar" },
    };
    draw_hints(W, H - 34, 0.55f, hints, 4);
}

/* ---------- main ---------- */
enum { ST_SCAN, ST_MENU, ST_SETTINGS };

int main(int argc, char *argv[])
{
    init_iop();
    init_gfx();
    init_pad();
    config_defaults(&cfg);

    int state = ST_SCAN, frame = 0, mask = 0, settle = -1;
    float t = 0;
    char status[64] = "USB: --";
    const char *boot = (argc > 0) ? argv[0] : NULL;
    if (boot) {                                   /* pasta do ELF (base dos PNGs e local de gravacao) */
        strncpy(bootDir, boot, sizeof(bootDir) - 1);
        char *sl = strrchr(bootDir, '/');
        if (sl) sl[1] = 0; else bootDir[0] = 0;
    }

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
                load_config_or_defaults(boot);
                msg[0] = 0;
                msgTimer = 0;
                if (usingDefaults) {
                    snprintf(msg, sizeof(msg), "Sem launcher.ini: padrao interno (START = Config)");
                    msgTimer = 360;
                }
                make_status(status, sizeof(status), mask);
                selCat = 0; fCat = 0; fItem = 0;
                memset(selItem, 0, sizeof(selItem));
                state = ST_MENU;
            }
        } else if (state == ST_SETTINGS) {
            if (pad & PAD_UP)   selSet = (selSet + SET_COUNT - 1) % SET_COUNT;
            if (pad & PAD_DOWN) selSet = (selSet + 1) % SET_COUNT;
            if (pad & PAD_LEFT)  settings_adjust(selSet, -1);
            if (pad & PAD_RIGHT) settings_adjust(selSet, +1);
            if (pad & PAD_CROSS) {
                if (selSet == SET_SAVE) {
                    char used[160], shown[48];
                    if (save_config(mask, used, sizeof(used))) {
                        snprintf(cfgPath, sizeof(cfgPath), "%s", used);
                        usingDefaults = 0;
                        clip_copy(shown, sizeof(shown), used, 40);
                        snprintf(msg, sizeof(msg), "Salvo: %s", shown);
                    } else {
                        snprintf(msg, sizeof(msg), "Falha ao salvar (sem pendrive/memory card gravavel)");
                    }
                    msgTimer = 240;
                } else if (selSet == SET_RESET) {
                    config_reset_theme(&cfg);
                    snprintf(msg, sizeof(msg), "Tema padrao restaurado (use Salvar para gravar)");
                    msgTimer = 180;
                } else if (selSet == SET_BACK) {
                    state = ST_MENU;
                } else {
                    settings_adjust(selSet, +1);
                }
            }
            if (pad & (PAD_CIRCLE | PAD_START)) state = ST_MENU;
            if (msgTimer > 0) msgTimer--;
            fCat += (selCat - fCat) * 0.25f;
            fItem += (selItem[selCat] - fItem) * 0.25f;
        } else {
            if (pad & PAD_START) { state = ST_SETTINGS; selSet = 0; continue; }
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
            if (msgTimer > 0) msgTimer--;
        }

        /* ---------- desenho ---------- */
        float W = gs->Width, H = gs->Height;
        fontK = cfg.font_scale * 0.01f;
        draw_background(W, H, t);

        if (state == ST_SCAN) {
            static const char *dots[] = { "", ".", "..", "..." };
            char s[96];
            draw_spinner(W / 2, H / 2 - 64, frame);
            snprintf(s, sizeof(s), "Detectando dispositivos de armazenamento%s", dots[(frame / 20) & 3]);
            text_c(W / 2, H / 2 - 20, 0.9f, cfg.text, s);
            snprintf(s, sizeof(s), "Encontrados: %d", __builtin_popcount(mask));
            text_c(W / 2, H / 2 + 10, 0.7f, cfg.text_dim, s);
            static const Hint hscan[] = { { BTN_CROSS, "Continuar agora" } };
            draw_hints(W, H / 2 + 46, 0.7f, hscan, 1);
        } else if (state == ST_SETTINGS) {
            draw_settings(W, H);
        } else {
            text(30, 20, 0.8f, cfg.text, cfg.header);
            text(W - 30 - text_w(status, 0.65f), 22, 0.65f, cfg.text_dim, status);
            if (usingDefaults) text(30 + text_w(cfg.header, 0.8f) + 14, 24, 0.5f, cfg.accent, "PADRAO INTERNO");

            draw_tabs(W);
            draw_wheel(W, H, t);
            draw_panel(W, H);

            if (msg[0] && msgTimer > 0) text_fit_c(W / 2, H - 70, W - 40, 0.7f, cfg.accent, msg);
            static const Hint hints[] = {
                { BTN_CROSS, "Abrir" }, { BTN_NONE, "START Config" }, { BTN_NONE, "R1 Recarregar" },
                { BTN_NONE, "< > Categoria" }, { BTN_NONE, "^ v App" },
            };
            draw_hints(W, H - 34, 0.55f, hints, 5);
        }

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    return 0;
}