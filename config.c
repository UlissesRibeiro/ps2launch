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
    return 0;
}