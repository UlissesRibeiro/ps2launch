#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static u32 parse_color(const char *s)
{
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (u32)strtoul(s, NULL, 16) & 0xFFFFFF;
}

static void cpy(char *dst, int sz, const char *src)
{
    strncpy(dst, src, sz - 1);
    dst[sz - 1] = 0;
}

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->bg_top = 0x1B2A4A;
    c->bg_bottom = 0x05070F;
    c->accent = 0x4FA3FF;
    c->text = 0xFFFFFF;
    c->text_dim = 0x8A93A6;
    c->bg_dim = 0;
    c->icon_size = 56;
    c->selected_scale = 125;
    c->item_size = 36;
    c->waves = 0;
    c->font_scale = 100;
    c->usb_wait_ms = 8000;
    cpy(c->header, sizeof(c->header), "PS2 Launcher");
}

void config_rel(const Config *c, const char *rel, char *out, int outsz)
{
    if (!rel[0]) { out[0] = 0; return; }
    if (strchr(rel, ':')) cpy(out, outsz, rel);
    else snprintf(out, outsz, "%s%s", c->base, rel);
}

int config_load(Config *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return -1; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    sz = fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    config_defaults(c);

    /* pasta base */
    cpy(c->base, sizeof(c->base), path);
    char *slash = strrchr(c->base, '/');
    if (slash) slash[1] = 0; else c->base[0] = 0;

    char sec[32] = "";
    Entry *cur = NULL;
    Category *ccur = NULL;
    char *save = NULL;

    for (char *ln = strtok_r(buf, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        ln = trim(ln);
        if (!*ln || *ln == ';' || *ln == '#') continue;

        if (*ln == '[') {
            char *e = strchr(ln, ']');
            if (!e) continue;
            *e = 0;
            cpy(sec, sizeof(sec), ln + 1);
            for (char *p = sec; *p; p++) *p = tolower((unsigned char)*p);
            cur = NULL;
            ccur = NULL;
            if (!strncmp(sec, "cat", 3) && c->ncat < MAX_CATS) {
                ccur = &c->cats[c->ncat++];
                memset(ccur, 0, sizeof(*ccur));
            } else if (!strncmp(sec, "app", 3) && c->count < MAX_ENTRIES) {
                cur = &c->entries[c->count++];
                memset(cur, 0, sizeof(*cur));
            }
            continue;
        }

        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(ln), *v = trim(eq + 1);
        for (char *p = k; *p; p++) *p = tolower((unsigned char)*p);

        if (!strcmp(sec, "theme")) {
            if (!strcmp(k, "background")) cpy(c->background, sizeof(c->background), v);
            else if (!strcmp(k, "bg_top")) c->bg_top = parse_color(v);
            else if (!strcmp(k, "bg_bottom")) c->bg_bottom = parse_color(v);
            else if (!strcmp(k, "accent")) c->accent = parse_color(v);
            else if (!strcmp(k, "text")) c->text = parse_color(v);
            else if (!strcmp(k, "text_dim")) c->text_dim = parse_color(v);
            else if (!strcmp(k, "bg_dim")) c->bg_dim = atoi(v);
            else if (!strcmp(k, "icon_size")) c->icon_size = atoi(v);
            else if (!strcmp(k, "selected_scale")) c->selected_scale = atoi(v);
            else if (!strcmp(k, "item_size")) c->item_size = atoi(v);
            else if (!strcmp(k, "waves")) c->waves = atoi(v);
            else if (!strcmp(k, "font_scale")) c->font_scale = atoi(v);
        } else if (!strcmp(sec, "general")) {
            if (!strcmp(k, "usb_wait_ms")) c->usb_wait_ms = atoi(v);
            else if (!strcmp(k, "header")) cpy(c->header, sizeof(c->header), v);
        } else if (ccur) {
            if (!strcmp(k, "title")) cpy(ccur->title, sizeof(ccur->title), v);
            else if (!strcmp(k, "icon")) cpy(ccur->icon, sizeof(ccur->icon), v);
        } else if (cur) {
            if (!strcmp(k, "title")) cpy(cur->title, sizeof(cur->title), v);
            else if (!strcmp(k, "path")) cpy(cur->paths, sizeof(cur->paths), v);
            else if (!strcmp(k, "icon")) cpy(cur->icon, sizeof(cur->icon), v);
            else if (!strcmp(k, "category") || !strcmp(k, "cat")) cpy(cur->catname, sizeof(cur->catname), v);
        }
    }
    free(buf);

    if (c->ncat == 0) {
        cpy(c->cats[0].title, sizeof(c->cats[0].title), "Apps");
        c->ncat = 1;
    }
    /* categoria do app: por numero (1..N) ou por nome */
    for (int i = 0; i < c->count; i++) {
        Entry *e = &c->entries[i];
        e->cat = 0;
        if (!e->catname[0]) continue;
        if (isdigit((unsigned char)e->catname[0])) {
            int n = atoi(e->catname) - 1;
            if (n >= 0 && n < c->ncat) e->cat = n;
        } else {
            for (int k = 0; k < c->ncat; k++)
                if (!strcasecmp(e->catname, c->cats[k].title)) { e->cat = k; break; }
        }
    }

    if (c->icon_size < 32) c->icon_size = 32;
    if (c->icon_size > 96) c->icon_size = 96;
    if (c->selected_scale < 100) c->selected_scale = 100;
    if (c->selected_scale > 200) c->selected_scale = 200;
    if (c->bg_dim < 0) c->bg_dim = 0;
    if (c->bg_dim > 112) c->bg_dim = 112;
    if (c->item_size < 24) c->item_size = 24;
    if (c->item_size > 64) c->item_size = 64;
    if (c->font_scale < 60) c->font_scale = 60;
    if (c->font_scale > 140) c->font_scale = 140;
    return 0;
}

/* ---------- padrao interno (espelha o launcher.ini de exemplo) ---------- */
void config_reset_theme(Config *c)
{
    c->bg_top = 0x1B2A4A;
    c->bg_bottom = 0x05070F;
    c->accent = 0x4FA3FF;
    c->text = 0xFFFFFF;
    c->text_dim = 0x8A93A6;
    c->bg_dim = 30;
    c->icon_size = 56;
    c->selected_scale = 125;
    c->item_size = 36;
    c->waves = 0;
    c->font_scale = 100;
    c->usb_wait_ms = 8000;
}

void config_builtin(Config *c, const char *base)
{
    static const struct { const char *title, *icon; } cats[] = {
        { "Jogos",       "icons/games.png" },
        { "Ferramentas", "icons/tools.png" },
    };
    static const struct { const char *title, *cat, *path, *icon; } apps[] = {
        { "OPL",           "Jogos",       "mass:/OPL/OPNPS2LD.ELF|mass:/OPNPS2LD.ELF|mc0:/APPS/OPL/OPNPS2LD.ELF", "icons/opl.png" },
        { "Neutrino",      "Jogos",       "mass:/neutrino/neutrino.elf",                                          "icons/neutrino.png" },
        { "uLaunchELF",    "Ferramentas", "mass:/APPS/ULE.ELF|mass:/BOOT/BOOT.ELF|mc0:/BOOT/BOOT.ELF",            "icons/ule.png" },
        { "Meu ELF exFAT", "Ferramentas", "mass1:/APPS/MEUAPP.ELF",                                               "icons/app.png" },
    };

    config_defaults(c);
    config_reset_theme(c);
    cpy(c->background, sizeof(c->background), "bg.png");
    cpy(c->base, sizeof(c->base), base ? base : "");

    for (int i = 0; i < (int)(sizeof(cats) / sizeof(cats[0])) && c->ncat < MAX_CATS; i++) {
        Category *k = &c->cats[c->ncat++];
        memset(k, 0, sizeof(*k));
        cpy(k->title, sizeof(k->title), cats[i].title);
        cpy(k->icon, sizeof(k->icon), cats[i].icon);
    }
    for (int i = 0; i < (int)(sizeof(apps) / sizeof(apps[0])) && c->count < MAX_ENTRIES; i++) {
        Entry *e = &c->entries[c->count++];
        memset(e, 0, sizeof(*e));
        cpy(e->title, sizeof(e->title), apps[i].title);
        cpy(e->catname, sizeof(e->catname), apps[i].cat);
        cpy(e->paths, sizeof(e->paths), apps[i].path);
        cpy(e->icon, sizeof(e->icon), apps[i].icon);
        for (int k = 0; k < c->ncat; k++)
            if (!strcasecmp(apps[i].cat, c->cats[k].title)) { e->cat = k; break; }
    }
}

/* ---------- gravacao ---------- */
static void put_color(FILE *f, const char *k, u32 v)
{
    fprintf(f, "%s=#%06X\n", k, (unsigned)(v & 0xFFFFFF));
}

static void backup_file(const char *path)
{
    FILE *in = fopen(path, "rb");
    if (!in) return;
    char bak[200];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    FILE *out = fopen(bak, "wb");
    if (out) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(in);
}

int config_save(const Config *c, const char *path)
{
    backup_file(path);                      /* preserva comentarios do arquivo antigo */
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "; PS2 Launcher - configuracao (gerada pelo menu de configuracoes)\n");
    fprintf(f, "; Cores: #RRGGBB ou 0xRRGGBB. Caminhos relativos partem da pasta deste .ini\n");
    fprintf(f, "; mass:/ = todos os USB | mass1:/ = dispositivo fixo | mc0:/ = memory card\n");
    fprintf(f, "; Varios caminhos separados por '|' = tenta em ordem.\n\n");

    fprintf(f, "[general]\nheader=%s\nusb_wait_ms=%d\n\n", c->header, c->usb_wait_ms);

    fprintf(f, "[theme]\n");
    if (c->background[0]) fprintf(f, "background=%s\n", c->background);
    put_color(f, "bg_top", c->bg_top);
    put_color(f, "bg_bottom", c->bg_bottom);
    fprintf(f, "bg_dim=%d\n", c->bg_dim);
    put_color(f, "accent", c->accent);
    put_color(f, "text", c->text);
    put_color(f, "text_dim", c->text_dim);
    fprintf(f, "icon_size=%d\nselected_scale=%d\nitem_size=%d\nwaves=%d\nfont_scale=%d\n\n",
            c->icon_size, c->selected_scale, c->item_size, c->waves, c->font_scale);

    for (int i = 0; i < c->ncat; i++) {
        fprintf(f, "[category%d]\ntitle=%s\n", i + 1, c->cats[i].title);
        if (c->cats[i].icon[0]) fprintf(f, "icon=%s\n", c->cats[i].icon);
        fprintf(f, "\n");
    }
    for (int i = 0; i < c->count; i++) {
        const Entry *e = &c->entries[i];
        int k = (e->cat >= 0 && e->cat < c->ncat) ? e->cat : 0;
        fprintf(f, "[app%d]\ntitle=%s\ncategory=%s\npath=%s\n", i + 1, e->title, c->cats[k].title, e->paths);
        if (e->icon[0]) fprintf(f, "icon=%s\n", e->icon);
        fprintf(f, "\n");
    }

    int err = ferror(f);
    if (fclose(f) != 0) err = 1;
    return err ? -1 : 0;
}