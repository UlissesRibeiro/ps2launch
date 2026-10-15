#ifndef CONFIG_H
#define CONFIG_H

#include <tamtypes.h>

#define MAX_ENTRIES 32
#define MAX_CATS    8

typedef struct {
    char title[32];
    char icon[128];
} Category;

typedef struct {
    char title[48];
    char paths[512];   /* varios caminhos separados por '|' (fallback) */
    char icon[128];
    char catname[32];  /* nome (ou numero) da categoria */
    int  cat;          /* indice resolvido */
} Entry;

typedef struct {
    /* [theme] */
    char background[128];
    u32  bg_top, bg_bottom;      /* 0xRRGGBB */
    u32  accent, text, text_dim;
    int  bg_dim;                 /* 0..112, escurece a imagem de fundo (sem custo extra) */
    int  icon_size;              /* icone de categoria (px) */
    int  selected_scale;         /* percentual, 100 = sem zoom */
    int  item_size;              /* icone dos apps (px) */
    int  waves;                  /* 1 = ondas animadas no fundo (mais pesado; padrao 0) */
    int  font_scale;             /* 60..140 (%), tamanho do texto; 100 = padrao */
    /* [general] */
    int  usb_wait_ms;
    char header[48];
    /* [categoryN] e [appN] */
    Category cats[MAX_CATS];
    int      ncat;
    Entry    entries[MAX_ENTRIES];
    int      count;
    char     base[128];
} Config;

void config_defaults(Config *c);
int  config_load(Config *c, const char *path);   /* 0 = ok */
void config_rel(const Config *c, const char *rel, char *out, int outsz);

/* padrao interno = mesmo conteudo do launcher.ini de exemplo (usado se nao houver .ini) */
void config_reset_theme(Config *c);                   /* so cores/tamanhos/usb_wait_ms */
void config_builtin(Config *c, const char *base);     /* tema + categorias + apps padrao */
int  config_save(const Config *c, const char *path);  /* grava .ini (faz .bak se ja existir); 0 = ok */

#endif