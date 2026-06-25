/*
 * pong.c — firmware bare-metal MicroBlaze V, Pong en Nexys A7-100T
 *
 * Arquitectura de renderizado:
 *   MicroBlaze escribe píxeles directamente al framebuffer BRAM (Port A, 32-bit AXI).
 *   El VGA lee Port B en cada pixel clock y convierte el índice de paleta a RGB.
 *
 *   Framebuffer: 640×480 × 4-bit = 38 400 palabras de 32 bits (big-endian, 8 px/word).
 *   word[31:28] = píxel 0 (x mod 8 == 0) … word[3:0] = píxel 7 (x mod 8 == 7).
 *
 * Periféricos:
 *   XPAR_AXI_BRAM_CTRL_0_BASEADDR   — framebuffer VRAM (Port A)
 *   XPAR_AXI_GPIO_0_BASEADDR        — GPIO0 ch1: botones {BTNR,BTNL,BTNC,BTND,BTNU}
 *                                     GPIO0 ch2: SW[0] modo 2P
 *   XPAR_AXI_GPIO_1_BASEADDR        — GPIO1 ch1: LED[14:0]
 *   XPAR_AXI_QUAD_SPI_0_BASEADDR    — SPI inter-FPGA modo 2P
 *   XPAR_AXI_QUAD_SPI_1_BASEADDR    — SPI microSD (requiere rebuild de plataforma)
 */

#include <stdio.h>
#include "xparameters.h"
#include "xgpio.h"
#include "xspi.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "sleep.h"

/* ── SD SPI: dirección provisional hasta rebuild de plataforma ─────────── */
#ifndef XPAR_AXI_QUAD_SPI_1_BASEADDR
#define XPAR_AXI_QUAD_SPI_1_BASEADDR  0x44A10000UL
#endif

/* ── Framebuffer ──────────────────────────────────────────────────────────── */
#define FB_BASE  XPAR_AXI_BRAM_CTRL_0_BASEADDR
#define FB_W     640
#define FB_H     480
#define FB_WORDS 38400   /* (640×480) / 8 */

/* ── DDR2 (MIG 7-series, calibración automática al arrancar) ────────────── */
#define DDR2_BASE       XPAR_MIG_0_BASEADDRESS
#define DDR2_SPR_BALL   ((u8 *)(DDR2_BASE + 0x40000UL))  /* 32 B,  tras 256 KB de código */
#define DDR2_SPR_PADDLE ((u8 *)(DDR2_BASE + 0x40020UL))  /* 240 B */
#define DDR2_SPR_LOGO     ((u8 *)(DDR2_BASE + 0x40110UL))  /* 512 B */
#define DDR2_SPR_GAMEOVER ((u8 *)(DDR2_BASE + 0x41000UL))  /* 22500 B — 200×225 4bpp (3 secciones) */
#define DDR2_SPR_MSEL     ((u8 *)(DDR2_BASE + 0x47000UL))  /* 11200 B — 200×112 4bpp */
#define DDR2_SPR_PAUSE    ((u8 *)(DDR2_BASE + 0x4A000UL))  /* 8000 B  — 200×80  4bpp */
#define SPR_GO_W         200
#define SPR_GO_SEC_H      75   /* alto de cada sección (3 secciones × 75 = 225) */
#define SPR_GO_SEC_BYTES (SPR_GO_W * SPR_GO_SEC_H / 2)  /* 7500 B por sección */
#define SPR_PAUSE_W      200
#define SPR_PAUSE_H       80

/* ── Paleta (debe coincidir con top_pong_project.v) ──────────────────────── */
#define COL_BLACK   0
#define COL_WHITE   1
#define COL_RED     2
#define COL_BLUE    3
#define COL_YELLOW  4
#define COL_GREEN   5
#define COL_ORANGE  6
#define COL_GRAY    7
#define COL_DGRAY   8
#define COL_MAGENTA 9

/* ── Geometría del juego ──────────────────────────────────────────────────── */
#define BALL_TICK_DIV 1    /* pelota avanza cada frame (60 Hz, vsync real) */
#define BALL_SZ    8
#define PAD_W      8
#define PAD_H      60
#define PAD1_X     20
#define PAD2_X     612
#define PAD_SPEED  5
#define SCORE_WIN  10

/* ── Botones (GPIO0 canal 1: bit = {BTNR,BTNL,BTNC,BTND,BTNU}) ──────────── */
#define BTN_U  0x01u
#define BTN_D  0x02u
#define BTN_C  0x04u
#define BTN_L  0x08u
#define BTN_R  0x10u
#define BTN_VSYNC 0x20u  /* GPIO0 bit 5 = vsync (activo bajo) */

/* ── Estados del juego ───────────────────────────────────────────────────── */
#define ST_MENU     0
#define ST_PLAYING  1
#define ST_PAUSE    2
#define ST_GAMEOVER 3
#define ST_WAIT_2P  4   /* espera handshake SPI antes de iniciar 2P */

/* ── Split-screen 2P ─────────────────────────────────────────────────────── */
#define GAME_W   1280             /* ancho total del campo (2 pantallas) */
#define PAD2_X_G (FB_W + PAD2_X) /* game-x paleta derecha: 1252 */
#define SPI_PING 0xA5u
#define SPI_PONG 0x5Au

/* ── AXI Quad SPI registros (offset desde base, PG153 v3.2) ─────────────── */
#define SPI_SRR   0x40u   /* Software Reset Register (offset 0x40, no 0x00!) */
#define SPI_CR    0x60u   /* Control Register */
#define SPI_SR    0x64u   /* Status Register */
#define SPI_DTR   0x68u   /* TX FIFO */
#define SPI_DRR   0x6Cu   /* RX FIFO */
#define SPI_SSR   0x70u   /* Slave Select (activo bajo) */

#define SPICR_RXRST    (1u << 6)   /* Reset RX FIFO */
#define SPICR_TXRST    (1u << 5)   /* Reset TX FIFO */
#define SPICR_INHIBIT  (1u << 8)
#define SPICR_MANSS    (1u << 7)
#define SPICR_MASTER   (1u << 2)
#define SPICR_SPE      (1u << 1)
#define SPICR_LOOP     (1u << 0)   /* Loopback: MOSI → MISO internamente */
#define SPISR_RX_EMPTY (1u << 0)   /* 1 = RX FIFO vacío */
#define SPISR_TX_EMPTY (1u << 2)

/* ── Tipos ───────────────────────────────────────────────────────────────── */
typedef struct { int x, y, dx, dy; } ball_t;
typedef struct { int y; }             pad_t;

/* ── Sprites (4-bit packed, high nibble = px izquierdo) ──────────────────── */
#define SPR_BALL_W   8
#define SPR_BALL_H   8
#define SPR_PAD_W    8
#define SPR_PAD_H    60
#define SPR_LOGO_W   64
#define SPR_LOGO_H   16
#define SPR_MSEL_W   200
#define SPR_MSEL_H   112

/* Sprites viven en DDR2 — ver DDR2_SPR_BALL / _PADDLE / _LOGO */
static u8 sd_sector_buf[512];

/* Layout de sectores en la SD (LBA, antes de la partición → sector 0 = MBR) */
#define SD_MAGIC        0x504F4E47u   /* "PONG" */
#define SD_LBA_HDR      1
#define SD_LBA_BALL     2
#define SD_LBA_PADDLE   3
#define SD_LBA_LOGO     5   /* 512 B = 1 sector exacto */
#define SD_LBA_GAMEOVER 6   /* 200×225 4bpp = 22500 B = 44 sectores (LBA 6-49) */
#define SD_LBA_MSEL    50   /* 200×112 4bpp = 11200 B = 22 sectores (LBA 50-71) */
#define SD_LBA_PAUSE   72   /* 200×80  4bpp = 8000 B  = 16 sectores (LBA 72-87) */

/* ── Estado global ───────────────────────────────────────────────────────── */
static ball_t  ball;
static pad_t   pad[2];
static int     score[2];
static int     game_state;
static int     selected;
static int     mode_2p;
static int     is_slave  = 0;   /* 0=maestro (izquierda), 1=esclavo (derecha) */
static int     sd_ok      = 0;
static int     sd_init_rc    = 0;   /* -1=CMD0 sin resp, -2=ACMD41 timeout, 0=OK */
static u8      sd_acmd41_r1  = 0xFF; /* ultimo R1 de CMD41 (0x00=OK, 0x01=idle, 0xFF=sin resp) */
static u8      sd_cmd55_r1   = 0xFF; /* ultimo R1 de CMD55 (para diagnóstico) */
static u8      sd_cmd8_r1    = 0xFF; /* R1 de CMD8 (0x01=SDHC, 0x05=SDSC/illegal, 0xFF=sin resp) */
static int     sd_acmd41_try = 0;    /* intentos hasta que termino el loop */
static int     sd_loopback_ok = -1;  /* 1=IP SPI ok, 0=IP roto, -1=no testeado */
static u8      sd_cmd0_r1    = 0xFF; /* respuesta real de CMD0 */
static u8      sd_read_r1    = 0xFF; /* R1 de CMD17 (debe ser 0x00) */
static u8      sd_read_token = 0xFF; /* token de datos (debe ser 0xFE) */
static u8      sd_cmd58_r1   = 0xFF; /* R1 de CMD58 (debe ser 0x00) */
static u8      sd_ocr0       = 0xFF; /* OCR byte 0 [31:24]: bit6=CCS */
static int     sd_sdhc       = 0;
static u32     sd_magic_read = 0;    /* magic leído de LBA1 (esperado 0x504F4E47="PONG") */
static int     load_step     = 0;    /* 0=no ran, 1=hdr ok, 2=ball ok, 3=paddle ok, 4=logo ok */
static int     sprites_ok = 0;
static int     msel_loaded = 0;
static int     pause_loaded = 0;

/* Dirty-rect state — evita fb_clear() por frame */
static int     ball_tick         = 0;
static int     hit_count         = 0;
static int     bounce_count      = 0;
static u32     rng_state         = 73856093u;
static int     score_dirty       = 1;
static int     needs_full_redraw = 1;
static int     prev_game_state   = -1;
static int     prev_selected     = -1;
static int     prev_ball_x, prev_ball_y;
static int     prev_pad0_y, prev_pad1_y;

static XGpio   gpio0;
static XGpio   gpio1;
static XSpi    spi;

/* ═══════════════════════════════════════════════════════════════════════════
 * EFECTOS VISUALES — estado
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Índice arcoíris: avanza en cada golpe de la pelota con un pad.
 * Usa los colores de paleta disponibles en orden visual. */
static int rainbow_index = 0;

/* Tabla de colores arcoíris usando los índices de paleta existentes.
 * 7 entradas ciclando: rojo, naranja, amarillo, verde, cyan(gris), azul, magenta */
static const u8 rainbow_colors[7] = {
    COL_RED,     /* 2 */
    COL_ORANGE,  /* 6 */
    COL_YELLOW,  /* 4 */
    COL_GREEN,   /* 5 */
    COL_GRAY,    /* 7 — más claro que cyan pero disponible */
    COL_BLUE,    /* 3 */
    COL_MAGENTA  /* 9 */
};

/* Borde de golpe: paddle que fue golpeada muestra un borde blanco de
 * HIT_BORDER_THICK px durante HIT_BORDER_FRAMES frames, siguiendo a la
 * paddle si esta se mueve. border_pad = -1 significa "ningún borde activo". */
static int   border_pad   = -1;
static int   border_timer = 0;
#define HIT_BORDER_FRAMES 30   /* ~0.5 s a 60 Hz */
#define HIT_BORDER_THICK   2   /* grosor del borde en píxeles */

/* Onda rombo: distancia Manhattan desde el punto de impacto, sin trigonometría.
 * Solo se dibuja el perímetro del rombo (borde de grosor WAVE_THICK px). */
static int   wave_active = 0;
static int   wave_x      = 0;
static int   wave_y      = 0;
static int   wave_r      = 0;   /* radio actual en píxeles Manhattan */
static int   wave_speed  = 0;   /* expansión px/frame según velocidad de la bola */
#define WAVE_THICK  2            /* grosor del borde del rombo en píxeles */
#define WAVE_MAX_R  240          /* radio máximo: ondulación local, NO cubre toda la pantalla */

/* ═══════════════════════════════════════════════════════════════════════════
 * RENDERER — escritura al framebuffer BRAM
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Borra el framebuffer entero con un color uniforme (8 px/word, sin lectura) */
static void fb_clear(u8 color)
{
    u8  c = color & 0xF;
    u32 p = (u32)c * 0x11111111u;   /* replica el nibble 8 veces */
    for (u32 i = 0; i < FB_WORDS; i++)
        Xil_Out32(FB_BASE + (i << 2), p);
}

/* Espera flanco de subida del vsync. */
static void wait_vsync(void)
{
    int t;
    if (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC)) {
        t = 200000;
        while (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
        return;
    }
    t = 2000000;
    while ((Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
    if (!t) return;
    t = 200000;
    while (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
}

static u32 rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Dibuja un rectángulo relleno. */
static void fb_fill_rect(int x, int y, int w, int h, u8 color)
{
    u8  c     = color & 0xF;
    int x1    = (x < 0)      ? 0    : x;
    int x2    = (x+w > FB_W) ? FB_W : x+w;
    int y1    = (y < 0)      ? 0    : y;
    int y2    = (y+h > FB_H) ? FB_H : y+h;

    for (int row = y1; row < y2; row++) {
        int col = x1;

        while (col < x2 && (col & 7)) {
            u32 pidx  = (u32)row * FB_W + col;
            u32 widx  = pidx >> 3;
            u32 nib   = pidx & 7u;
            u32 addr  = FB_BASE + (widx << 2);
            u32 shift = (7u - nib) * 4u;
            u32 word  = Xil_In32(addr);
            word = (word & ~(0xFu << shift)) | ((u32)c << shift);
            Xil_Out32(addr, word);
            col++;
        }

        u32 pattern = (u32)c * 0x11111111u;
        while (col + 8 <= x2) {
            u32 pidx = (u32)row * FB_W + col;
            Xil_Out32(FB_BASE + ((pidx >> 3) << 2), pattern);
            col += 8;
        }

        while (col < x2) {
            u32 pidx  = (u32)row * FB_W + col;
            u32 widx  = pidx >> 3;
            u32 nib   = pidx & 7u;
            u32 addr  = FB_BASE + (widx << 2);
            u32 shift = (7u - nib) * 4u;
            u32 word  = Xil_In32(addr);
            word = (word & ~(0xFu << shift)) | ((u32)c << shift);
            Xil_Out32(addr, word);
            col++;
        }
    }
}

/* Color de fondo "puro" para una fila dada, respetando la grilla de
 * scanlines CRT (COL_DGRAY cada 4 filas en vez de COL_BLACK).
 * Usado al borrar pelota/paletas para no agujerear las scanlines. */
static u8 bg_row_color(int y)
{
    return ((y & 3) == 0) ? COL_DGRAY : COL_BLACK;
}

/* Igual que fb_fill_rect pero rellena cada fila con su color de fondo
 * correcto (negro u oscuro de scanline) en vez de un color fijo. */
static void fb_fill_rect_bg(int x, int y, int w, int h)
{
    int y1 = y, y2 = y + h;
    for (int row = y1; row < y2; row++)
        fb_fill_rect(x, row, w, 1, bg_row_color(row));
}

/* Font 4×5 para dígitos 0-9. */
static const u8 font4x5[10][5] = {
    {0x6, 0x9, 0x9, 0x9, 0x6},  /* 0 */
    {0x2, 0x6, 0x2, 0x2, 0x7},  /* 1 */
    {0x6, 0x9, 0x2, 0x4, 0xF},  /* 2 */
    {0xE, 0x1, 0x6, 0x1, 0xE},  /* 3 */
    {0x9, 0x9, 0xF, 0x1, 0x1},  /* 4 */
    {0xF, 0x8, 0xE, 0x1, 0xE},  /* 5 */
    {0x6, 0x8, 0xE, 0x9, 0x6},  /* 6 */
    {0xF, 0x1, 0x2, 0x4, 0x4},  /* 7 */
    {0x6, 0x9, 0x6, 0x9, 0x6},  /* 8 */
    {0x6, 0x9, 0x7, 0x1, 0x6},  /* 9 */
};

static void fb_draw_digit(int x, int y, int digit, int scale, u8 color)
{
    const u8 *g = font4x5[digit % 10];
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (g[row] & (0x8u >> col))
                fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void fb_draw_scores(void)
{
    fb_draw_digit(256, 8, score[0], 6, COL_WHITE);
    fb_draw_digit(360, 8, score[1], 6, COL_WHITE);
}

static u8 bg_pixel(int x, int y)
{
    if (!mode_2p && x >= 318 && x < 322 && (y & 7) < 4) return COL_GRAY;
    if (x >= 256 && x < 280 && y >= 8 && y < 38) {
        u8 p = font4x5[score[0] % 10][(y - 8) / 6];
        if (p & (0x8u >> ((x - 256) / 6))) return COL_WHITE;
    }
    if (x >= 360 && x < 384 && y >= 8 && y < 38) {
        u8 p = font4x5[score[1] % 10][(y - 8) / 6];
        if (p & (0x8u >> ((x - 360) / 6))) return COL_WHITE;
    }
    if ((!mode_2p || !is_slave) && x >= PAD1_X && x < PAD1_X + PAD_W &&
        y >= pad[0].y && y < pad[0].y + PAD_H) return rainbow_colors[rainbow_index];
    if ((!mode_2p || is_slave) && x >= PAD2_X && x < PAD2_X + PAD_W &&
        y >= pad[1].y && y < pad[1].y + PAD_H) return rainbow_colors[rainbow_index];
    /* Grilla CRT — cualquier restauración de fondo que no caiga sobre marcador
     * o paletas debe respetar la scanline en vez de pintar negro puro. */
    if ((y & 3) == 0) return COL_DGRAY;
    return COL_BLACK;
}

static void fb_set_pixel(int x, int y, u8 color)
{
    u32 pidx  = (u32)y * FB_W + x;
    u32 addr  = FB_BASE + ((pidx >> 3) << 2);
    u32 shift = (7u - (pidx & 7u)) * 4u;
    u32 word  = Xil_In32(addr);
    Xil_Out32(addr, (word & ~(0xFu << shift)) | ((u32)color << shift));
}

static void fb_blit_scaled(int x, int y, int w, int h, const u8 *spr, int transparent, int sc)
{
    for (int row = 0; row < h; row++) {
        int col = 0;
        while (col < w) {
            int idx = row * w + col;
            u8  px  = (idx & 1) ? (spr[idx >> 1] & 0xF) : (spr[idx >> 1] >> 4);
            if (transparent && px == 0) { col++; continue; }
            int run = 1;
            while (col + run < w) {
                int idx2 = row * w + col + run;
                u8  px2  = (idx2 & 1) ? (spr[idx2 >> 1] & 0xF) : (spr[idx2 >> 1] >> 4);
                if (px2 != px) break;
                run++;
            }
            fb_fill_rect(x + col * sc, y + row * sc, run * sc, sc, px);
            col += run;
        }
    }
}

static const u8 font_ext[16][5] = {
    {0x6, 0x9, 0xF, 0x9, 0x9},  /* A */
    {0xE, 0x9, 0x9, 0x9, 0xE},  /* D */
    {0xF, 0x8, 0xE, 0x8, 0xF},  /* E */
    {0x7, 0x8, 0xB, 0x9, 0x7},  /* G */
    {0xF, 0x6, 0x6, 0x6, 0xF},  /* I */
    {0x7, 0x1, 0x1, 0x9, 0x6},  /* J */
    {0x8, 0x8, 0x8, 0x8, 0xF},  /* L */
    {0x9, 0xF, 0xF, 0x9, 0x9},  /* M */
    {0x9, 0xD, 0xB, 0x9, 0x9},  /* N */
    {0x6, 0x9, 0x9, 0x9, 0x6},  /* O */
    {0xE, 0x9, 0xE, 0x8, 0x8},  /* P */
    {0xE, 0x9, 0xE, 0xA, 0x9},  /* R */
    {0x7, 0x8, 0x6, 0x1, 0xE},  /* S */
    {0xF, 0x6, 0x6, 0x6, 0x6},  /* T */
    {0x9, 0x9, 0x9, 0x9, 0x6},  /* U */
    {0x9, 0x9, 0x9, 0x6, 0x6},  /* V */
};

static int char_idx(char c) {
    switch (c) {
        case 'A': return 0; case 'D': return 1; case 'E': return 2;
        case 'G': return 3; case 'I': return 4; case 'J': return 5;
        case 'L': return 6; case 'M': return 7; case 'N': return 8;
        case 'O': return 9; case 'P': return 10; case 'R': return 11;
        case 'S': return 12; case 'T': return 13; case 'U': return 14;
        case 'V': return 15;
        default:  return -1;
    }
}

static void fb_draw_str(int x, int y, const char *s, int scale, u8 color) {
    while (*s) {
        char c = *s++;
        const u8 *g = NULL;
        if (c >= '0' && c <= '9')  g = font4x5[c - '0'];
        else { int idx = char_idx(c); if (idx >= 0) g = font_ext[idx]; }
        if (g) {
            for (int row = 0; row < 5; row++)
                for (int col = 0; col < 4; col++)
                    if (g[row] & (0x8u >> col))
                        fb_fill_rect(x + col*scale, y + row*scale, scale, scale, color);
        }
        x += 4*scale + 2;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * EFECTOS VISUALES — funciones de dibujo
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * draw_wave: dibuja el perímetro de un rombo (distancia Manhattan) expandiéndose
 * desde el punto de impacto. Sin trigonometría — solo abs() y sumas enteras.
 *
 * Es una ONDA LOCAL, no un efecto de pantalla completa: el radio está
 * acotado a WAVE_MAX_R. Al llegar a ese radio, el frame siguiente solo
 * borra el último anillo dibujado (sin dibujar uno nuevo) y se desactiva,
 * garantizando que no quede un anillo "fantasma" pegado en pantalla.
 *
 * Estrategia dirty-rect: solo pinta el perímetro nuevo (WAVE_THICK px de grosor)
 * y borra el perímetro anterior restaurando el color de fondo real (bg_pixel,
 * que ya respeta paletas, marcador y scanlines). No hace fb_clear(), no toca
 * el resto de la pantalla.
 */
/* ═══════════════════════════════════════════════════════════════════════════
 * draw_wave — reemplaza la versión anterior
 *
 * BUG 3 FIX: el anillo nuevo no se dibuja sobre la zona del marcador
 * (y < 42). El borrado del rastro anterior sí usa bg_pixel, que ya
 * devuelve COL_WHITE para los píxeles de dígito, así que el marcador
 * se restaura correctamente al pasar la onda por encima.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void draw_wave(void)
{
    if (!wave_active) return;

    u8  color    = rainbow_colors[rainbow_index];
    int r        = wave_r;
    int thick    = WAVE_THICK;
    int draw_new = (r <= WAVE_MAX_R);

    int y_start = wave_y - r - thick;
    int y_end   = wave_y + r + thick;
    if (y_start < 0)     y_start = 0;
    if (y_end   >= FB_H) y_end   = FB_H - 1;

    for (int py = y_start; py <= y_end; py++) {
        int dy = py - wave_y;
        if (dy < 0) dy = -dy;

        if (draw_new) {
            for (int t = 0; t < thick; t++) {
                int ro = r + t;
                int rx = ro - dy;
                if (rx < 0) continue;
                int px_l = wave_x - rx;
                int px_r = wave_x + rx;

                /*
                 * BUG 3 FIX — no pintar encima del marcador (zona y < 42).
                 * El borrado del rastro usa bg_pixel() que ya sabe
                 * restaurar los dígitos, así que solo bloqueamos
                 * el dibujo del anillo nuevo.
                 */
                if (py >= 42) {
                    if (px_l >= 0 && px_l < FB_W)
                        fb_set_pixel(px_l, py, color);
                    if (px_r >= 0 && px_r < FB_W && px_r != px_l)
                        fb_set_pixel(px_r, py, color);
                }
            }
        }

        /* Borrar rastro anterior — sin cambios, bg_pixel ya maneja el marcador */
        int r_old = r - wave_speed;
        if (r_old > 0) {
            for (int t = 0; t < thick; t++) {
                int ro = r_old + t;
                int rx = ro - dy;
                if (rx < 0) continue;
                int px_l = wave_x - rx;
                int px_r = wave_x + rx;
                if (px_l >= 0 && px_l < FB_W)
                    fb_set_pixel(px_l, py, bg_pixel(px_l, py));
                if (px_r >= 0 && px_r < FB_W && px_r != px_l)
                    fb_set_pixel(px_r, py, bg_pixel(px_r, py));
            }
        }
    }

    wave_r += wave_speed;
    if (!draw_new) wave_active = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * draw_hit_border — reemplaza la versión anterior
 *
 * BUG 2 FIX:
 *   - El borde se dibuja en CADA frame mientras border_timer > 0,
 *     independientemente de si la paddle se movió o no.
 *   - Antes de dibujar el borde nuevo, se borra el borde del frame
 *     anterior restaurando los píxeles correctos (interior arcoíris
 *     o bg si la paddle se movió).
 *   - Al expirar, se borra el borde y se deja la paddle limpia.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Posición del borde en el frame anterior — para poder borrarlo
 * aunque la paddle se haya movido entre frames. */
static int border_prev_py = -1;

static void draw_hit_border(void)
{
    if (border_timer <= 0 || border_pad < 0) return;

    int idx     = border_pad;
    int visible = (!mode_2p) || (idx == 0 ? !is_slave : is_slave);
    if (!visible) { border_timer = 0; border_pad = -1; border_prev_py = -1; return; }

    int px      = (idx == 0) ? PAD1_X : PAD2_X;
    int py      = pad[idx].y;   /* posición ACTUAL de la paddle */
    int t       = HIT_BORDER_THICK;
    u8  pad_col = rainbow_colors[rainbow_index];

    /*
     * Paso 1 — borrar el borde del frame anterior si la paddle se movió.
     * Si border_prev_py == py la paddle no se movió y el borde anterior
     * está exactamente donde lo vamos a redibujar: no hace falta borrarlo.
     */
    if (border_prev_py >= 0 && border_prev_py != py) {
        int opy = border_prev_py;
        /* Restaurar las 4 tiras del borde viejo con el color correcto.
         * Usamos bg_pixel para el fondo y pad_col para el interior
         * de la paddle, si es que alguna tira cae dentro de la paddle
         * actual (caso de solapamiento parcial al moverse). */
        for (int row = opy; row < opy + PAD_H; row++) {
            for (int col = px; col < px + PAD_W; col++) {
                /* Solo las tiras que eran borde */
                int en_borde = (row < opy + t) || (row >= opy + PAD_H - t) ||
                               (col < px + t)  || (col >= px + PAD_W - t);
                if (!en_borde) continue;
                /* ¿Este píxel pertenece a la paddle en su posición NUEVA? */
                int en_pad_nuevo = (row >= py && row < py + PAD_H);
                if (en_pad_nuevo)
                    fb_set_pixel(col, row, pad_col);
                else
                    fb_set_pixel(col, row, bg_pixel(col, row));
            }
        }
    }

    border_timer--;

    if (border_timer == 0) {
        /*
         * Paso 2a — expiró: restaurar los 4 bordes en la posición actual
         * con el color correcto (interior arcoíris).
         */
        fb_fill_rect(px,             py,             PAD_W, t,     pad_col);
        fb_fill_rect(px,             py + PAD_H - t, PAD_W, t,     pad_col);
        fb_fill_rect(px,             py,             t,     PAD_H, pad_col);
        fb_fill_rect(px + PAD_W - t, py,             t,     PAD_H, pad_col);
        border_pad    = -1;
        border_prev_py = -1;
        return;
    }

    /*
     * Paso 2b — todavía activo: dibujar el borde blanco en la posición actual.
     */
    fb_fill_rect(px,             py,             PAD_W, t,     COL_WHITE);
    fb_fill_rect(px,             py + PAD_H - t, PAD_W, t,     COL_WHITE);
    fb_fill_rect(px,             py,             t,     PAD_H, COL_WHITE);
    fb_fill_rect(px + PAD_W - t, py,             t,     PAD_H, COL_WHITE);

    border_prev_py = py;
}

/*
 * draw_scanlines: dibuja líneas horizontales oscuras (COL_DGRAY) cada 4 filas,
 * altura 1px. Solo se llama en needs_full_redraw para no sobrecargar cada frame.
 * Costo: 120 fb_fill_rect de 640×1 px = 120 × 80 palabras = 9600 escrituras BRAM.
 * Coste único — no se repite por frame.
 */
static void draw_scanlines(void)
{
    for (int y = 0; y < FB_H; y += 4) {
        /* Pintar 1px cada 4 filas con DGRAY encima del fondo negro */
        fb_fill_rect(0, y, FB_W, 1, COL_DGRAY);
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * on_paddle_hit — reemplaza la versión anterior
 * ═══════════════════════════════════════════════════════════════════════════ */
static void on_paddle_hit(int pad_idx, int cx, int cy, int ball_spd)
{
    int old_index = rainbow_index;

    /* Avanzar color arcoíris */
    rainbow_index++;
    if (rainbow_index >= 7) rainbow_index = 0;

    /*
     * BUG 1 FIX — repintar ambas paddles visibles con el nuevo color
     * inmediatamente, antes de que el delta-rect del frame actual
     * solo pinte las franjas movidas con el color viejo.
     * Solo se repinta la paddle visible en esta pantalla.
     */
    (void)old_index;  /* ya no necesitamos el color anterior */
    u8 new_color = rainbow_colors[rainbow_index];

    if (!mode_2p || !is_slave)
        fb_fill_rect(PAD1_X, pad[0].y, PAD_W, PAD_H, new_color);
    if (!mode_2p || is_slave)
        fb_fill_rect(PAD2_X, pad[1].y, PAD_W, PAD_H, new_color);

    /* Activar onda rombo desde el centro del pad */
    wave_active = 1;
    wave_x      = cx;
    wave_y      = cy;
    wave_r      = WAVE_THICK;
    wave_speed  = 2 + ball_spd / 3;
    if (wave_speed > 4) wave_speed = 4;

    /* Activar borde de golpe en la paddle correspondiente */
    border_pad   = pad_idx;
    border_timer = HIT_BORDER_FRAMES;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SD CARD — driver mínimo SPI manual (modo SPI 0, CPOL=0 CPHA=0)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SD_BASE   XPAR_AXI_QUAD_SPI_1_BASEADDR
#define SPI2P_BASE XPAR_AXI_QUAD_SPI_0_BASEADDR

static void sd_spi_setup(void)
{
    Xil_Out32(SD_BASE + SPI_SRR, 0x0Au);
    usleep(100);
    Xil_Out32(SD_BASE + SPI_CR,
              SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE |
              SPICR_TXRST | SPICR_RXRST);
    usleep(10);
    Xil_Out32(SD_BASE + SPI_CR, SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
}

static u8 sd_spi_byte(u8 tx)
{
    while (!(Xil_In32(SD_BASE + SPI_SR) & SPISR_RX_EMPTY))
        (void)Xil_In32(SD_BASE + SPI_DRR);

    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_DTR, tx);
    Xil_Out32(SD_BASE + SPI_CR, cr & ~SPICR_INHIBIT);

    for (int t = 0; t < 100000; t++) {
        u32 sr = Xil_In32(SD_BASE + SPI_SR);
        if ((sr & SPISR_TX_EMPTY) && !(sr & SPISR_RX_EMPTY)) break;
    }
    Xil_Out32(SD_BASE + SPI_CR, cr);
    return (u8)Xil_In32(SD_BASE + SPI_DRR);
}

static int sd_loopback_test(void)
{
    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_CR, cr | SPICR_LOOP);
    u8 echo = sd_spi_byte(0x5A);
    Xil_Out32(SD_BASE + SPI_CR, cr);
    return (echo == 0x5A) ? 1 : 0;
}

static int sd_init(void)
{
    sd_spi_setup();

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 10; i++) sd_spi_byte(0xFF);

    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    const u8 cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    u8 r1 = 0xFF;
    for (int attempt = 0; attempt < 20 && r1 != 0x01; attempt++) {
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd0[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        if (r1 != 0x01) {
            Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
            for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
            Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
            usleep(10000);
        }
    }
    sd_cmd0_r1 = r1;
    if (r1 != 0x01) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    const u8 cmd8[] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd8[i]);
    u8 r8 = 0xFF;
    for (int i = 0; i < 8 && r8 == 0xFF; i++) r8 = sd_spi_byte(0xFF);
    sd_cmd8_r1 = r8;
    if (r8 == 0x01) {
        for (int i = 0; i < 4; i++) sd_spi_byte(0xFF);
    } else {
        for (int i = 0; i < 8; i++) sd_spi_byte(0xFF);
    }

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    int tries = 0;
    do {
        Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
        const u8 cmd55[] = {0x77, 0x00, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd55[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        sd_cmd55_r1 = r1;
        Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
        sd_spi_byte(0xFF);

        Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
        const u8 cmd41[] = {0x69, 0x40, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd41[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
        sd_spi_byte(0xFF);

        tries++;
        usleep(10000);
    } while (r1 != 0x00 && tries < 200);
    sd_acmd41_r1  = r1;
    sd_acmd41_try = tries;

    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -2; }

    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    const u8 cmd58[] = {0x7A, 0x00, 0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd58[i]);
    u8 r58 = 0xFF;
    for (int i = 0; i < 20 && r58 == 0xFF; i++) r58 = sd_spi_byte(0xFF);
    sd_cmd58_r1 = r58;
    u8 ocr0 = sd_spi_byte(0xFF);
    sd_ocr0 = ocr0;
    sd_spi_byte(0xFF); sd_spi_byte(0xFF); sd_spi_byte(0xFF);
    sd_sdhc = (r58 == 0x00 && (ocr0 & 0x40)) ? 1 : 0;

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    return 0;
}

static int sd_read_block(u32 lba, u8 *buf)
{
    u32 addr = sd_sdhc ? lba : (lba << 9);

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 8; i++) sd_spi_byte(0xFF);

    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    u8 cmd[6] = { 0x51,
        (u8)(addr >> 24), (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, 0x01 };
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd[i]);

    u8 r1 = 0xFF;
    for (int i = 0; i < 20 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
    sd_read_r1 = r1;
    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    u8 tok = 0xFF;
    for (int i = 0; i < 40000 && tok != 0xFE; i++) tok = sd_spi_byte(0xFF);
    sd_read_token = tok;
    if (tok != 0xFE) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    for (int i = 0; i < 512; i++) buf[i] = sd_spi_byte(0xFF);
    sd_spi_byte(0xFF); sd_spi_byte(0xFF);

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    return 0;
}

static int sd_run_test(void)
{
    xil_printf("\r\n=== SD TEST ===\r\n");

    sd_spi_setup();
    sd_loopback_ok = sd_loopback_test();
    xil_printf("Loopback: %s\r\n", sd_loopback_ok ? "PASS" : "FAIL");

    int rc = sd_init();
    sd_init_rc = rc;
    if (rc == -1) {
        xil_printf("FAIL CMD0: tarjeta no responde\r\n");
        return 0;
    }
    if (rc == -2) {
        xil_printf("FAIL ACMD41: tarjeta no sale de idle\r\n");
        return 0;
    }
    xil_printf("PASS init: SD lista, tipo=%s\r\n", sd_sdhc ? "SDHC/SDXC" : "SDSC");

    usleep(10000);

    if (sd_read_block(0, sd_sector_buf) != 0) {
        xil_printf("FAIL read LBA0: CMD17 sin token de datos\r\n");
        return 0;
    }
    xil_printf("PASS read LBA0: primeros 4 bytes = %02X %02X %02X %02X\r\n",
               sd_sector_buf[0], sd_sector_buf[1],
               sd_sector_buf[2], sd_sector_buf[3]);

    if (sd_sector_buf[510] == 0x55 && sd_sector_buf[511] == 0xAA) {
        xil_printf("PASS MBR: firma 0x55AA encontrada\r\n");
    } else {
        xil_printf("WARN MBR: firma no encontrada "
                   "(bytes 510-511 = %02X %02X)\r\n",
                   sd_sector_buf[510], sd_sector_buf[511]);
    }

    xil_printf("=== SD TEST OK ===\r\n\r\n");
    return 1;
}

static int sd_read_block_r(u32 lba, u8 *buf)
{
    for (int r = 0; r < 3; r++)
        if (sd_read_block(lba, buf) == 0) return 0;
    return -1;
}

static void load_sprites(void)
{
    if (!sd_ok) return;

    if (sd_read_block(SD_LBA_HDR, sd_sector_buf) != 0) return;
    u32 magic = ((u32)sd_sector_buf[0] << 24) | ((u32)sd_sector_buf[1] << 16) |
                ((u32)sd_sector_buf[2] <<  8) |  (u32)sd_sector_buf[3];
    sd_magic_read = magic;
    if (magic != SD_MAGIC) return;
    load_step = 1;

    u8 *dst;

    if (sd_read_block(SD_LBA_BALL, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_BALL;
    for (int i = 0; i < SPR_BALL_W * SPR_BALL_H / 2; i++) dst[i] = sd_sector_buf[i];
    load_step = 2;

    if (sd_read_block(SD_LBA_PADDLE, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_PADDLE;
    for (int i = 0; i < SPR_PAD_W * SPR_PAD_H / 2; i++) dst[i] = sd_sector_buf[i];
    load_step = 3;

    if (sd_read_block(SD_LBA_LOGO, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_LOGO;
    for (int i = 0; i < SPR_LOGO_W * SPR_LOGO_H / 2; i++) dst[i] = sd_sector_buf[i];
    load_step = 4;

    dst = DDR2_SPR_GAMEOVER;
    for (int s = 0; s < 44; s++) {
        if (sd_read_block_r(SD_LBA_GAMEOVER + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_GO_W * 225 / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    load_step = 5;
    sprites_ok = 1;

    dst = DDR2_SPR_MSEL;
    for (int s = 0; s < 22; s++) {
        if (sd_read_block_r(SD_LBA_MSEL + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_MSEL_W * SPR_MSEL_H / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    msel_loaded = 1;

    dst = DDR2_SPR_PAUSE;
    for (int s = 0; s < 16; s++) {
        if (sd_read_block_r(SD_LBA_PAUSE + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_PAUSE_W * SPR_PAUSE_H / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    pause_loaded = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LÓGICA DEL JUEGO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void init_game(void)
{
    u32 r = rng_next();
    ball.x  = mode_2p ? GAME_W / 2 : FB_W / 2;
    ball.y  = FB_H / 4 + (int)(r % (FB_H / 2));
    ball.dx = (r & 1u) ? 2 : -2;
    ball.dy = (int)(1 + (rng_next() % 2));
    if (rng_next() & 1u) ball.dy = -ball.dy;
    pad[0].y = (FB_H - PAD_H) / 2;
    pad[1].y = (FB_H - PAD_H) / 2;
    ball_tick    = 0;
    hit_count    = 0;
    bounce_count = 0;
    needs_full_redraw = 1;

    /* Resetear efectos al iniciar partida */
    wave_active  = 0;
    border_pad   = -1;
    border_timer = 0;
}

static int collide(ball_t *b, int px, int py)
{
    int x0   = b->x - b->dx;
    int xmin = x0 < b->x ? x0 : b->x;
    int xmax = x0 > b->x ? x0 + BALL_SZ : b->x + BALL_SZ;
    if (xmax <= px || xmin >= px + PAD_W) return 0;
    if (b->y + BALL_SZ <= py || b->y >= py + PAD_H) return 0;
    return 1;
}

static int check_score(void)
{
    for (int i = 0; i < 2; i++) {
        if (score[i] >= SCORE_WIN) {
            selected = i;
            score[0] = score[1] = 0;
            return i + 1;
        }
    }
    return 0;
}

static void move_local_pad(int p, int up)
{
    pad[p].y += up ? -PAD_SPEED : PAD_SPEED;
    if (pad[p].y < 0)            pad[p].y = 0;
    if (pad[p].y > FB_H - PAD_H) pad[p].y = FB_H - PAD_H;
}

static void move_ai(void)
{
    static int react_delay = 0;
    static int hit_offset  = PAD_H / 2;
    static int aim_err     = 0;
    static int last_dx_ai  = 0;

    if ((ball.dx > 0 && last_dx_ai < 0) || (ball.dx < 0 && last_dx_ai > 0))
        bounce_count++;

    if (ball.dx < 0 && last_dx_ai >= 0) {
        hit_offset = (int)(rng_next() % (PAD_H - BALL_SZ + 1));
        if ((int)(rng_next() % 100) < 27) {
            react_delay = 33 + (int)(rng_next() % 23);
            aim_err = (int)(rng_next() % 13) - 6;
        } else {
            aim_err = (int)(rng_next() % 9) - 4;
        }
    }
    last_dx_ai = ball.dx;

    if (bounce_count >= 8) {
        bounce_count = 0;
        int mid = (PAD_H - BALL_SZ) / 2;
        if (hit_offset <= mid)
            hit_offset = mid + 2 + (int)(rng_next() % (mid - 2));
        else
            hit_offset = 1 + (int)(rng_next() % (mid - 2));
    }

    if (react_delay > 0) { react_delay--; return; }

    int target = ball.y + BALL_SZ / 2 - hit_offset + aim_err;

    if (ball.dx < 0) {
        int diff = target - pad[0].y;
        if      (diff >  3) pad[0].y += 3;
        else if (diff < -3) pad[0].y -= 3;
        else                pad[0].y += diff;
    } else {
        int mid  = (FB_H - PAD_H) / 2;
        int diff = mid - pad[0].y;
        if      (diff >  3) pad[0].y += 3;
        else if (diff < -3) pad[0].y -= 3;
        else                pad[0].y += diff;
    }

    if (pad[0].y < 0)            pad[0].y = 0;
    if (pad[0].y > FB_H - PAD_H) pad[0].y = FB_H - PAD_H;
}

static void move_ball(void)
{
    int rpad_x     = mode_2p ? PAD2_X_G : PAD2_X;
    int right_wall = mode_2p ? GAME_W   : FB_W;

    ball.x += ball.dx;
    ball.y += ball.dy;

    if (ball.y < 0)               { ball.y = 0;              ball.dy = -ball.dy; }
    if (ball.y > FB_H - BALL_SZ)  { ball.y = FB_H - BALL_SZ; ball.dy = -ball.dy; }

    if (ball.x < 0)                    { score[1]++; score_dirty = 1; init_game(); return; }
    if (ball.x > right_wall - BALL_SZ) { score[0]++; score_dirty = 1; init_game(); return; }

    /* Colisión paleta izquierda */
    if (collide(&ball, PAD1_X, pad[0].y)) {
        int hit = (pad[0].y + PAD_H) - ball.y;
        int spd = 2 + (++hit_count);
        if (spd > 7) spd = 7;
        ball.dx = spd;
        ball.dy = (hit < 8) ? 2 : (hit < 16) ? 2 : (hit < 24) ? 1 :
                  (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                  (hit < 46) ? -1 : (hit < 54) ? -2 : -2;
        if (ball.x < PAD1_X + PAD_W + 2) ball.x = PAD1_X + PAD_W + 2;

        /* ── EFECTOS de impacto: pad izquierdo (idx 0) ── */
        on_paddle_hit(0, PAD1_X + PAD_W / 2, pad[0].y + PAD_H / 2, spd);
    }

    /* Colisión paleta derecha */
    {
        int r_hit = collide(&ball, rpad_x, pad[1].y);
        if (r_hit) {
            int hit = (pad[1].y + PAD_H) - ball.y;
            int spd = 2 + (++hit_count);
            if (spd > 7) spd = 7;
            ball.dx = -spd;
            ball.dy = (hit < 8) ? 2 : (hit < 16) ? 2 : (hit < 24) ? 1 :
                      (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                      (hit < 46) ? -1 : (hit < 54) ? -2 : -2;
            if (ball.x > rpad_x - BALL_SZ - 2) ball.x = rpad_x - BALL_SZ - 2;

            /* ── EFECTOS de impacto: pad derecho (idx 1, coord pantalla) ── */
            on_paddle_hit(1, PAD2_X + PAD_W / 2, pad[1].y + PAD_H / 2, spd);
        }
    }
}

/* ── SPI inter-FPGA 2P ───────────────────────────────────────────────────── */
static u32 btn_prev = 0;

static u32 btn_edge(void)
{
    u32 cur  = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
    u32 edge = cur & ~btn_prev;
    btn_prev = cur;
    if (cur & (BTN_U | BTN_D)) edge &= ~BTN_C;
    return edge;
}

static int sw_on(void)
{
    return (int)(XGpio_DiscreteRead(&gpio0, 2) & 0x1u);
}

static void spi_exchange(void)
{
    u8 tx[8], rx[8];
    tx[0] = (u8)(ball.x    >> 8); tx[1] = (u8)ball.x;
    tx[2] = (u8)(ball.y    >> 8); tx[3] = (u8)ball.y;
    tx[4] = (u8)(pad[0].y  >> 8); tx[5] = (u8)pad[0].y;
    tx[6] = (u8)((game_state << 4) | (score[0] & 0xF));
    tx[7] = (u8)((selected   << 4) | (score[1] & 0xF));
    {
        int rc = XSpi_Transfer(&spi, tx, rx, 8);
        if (rc == XST_SUCCESS) {
            int remote_y = (int)rx[0] << 1;
            if (remote_y >= 0 && remote_y <= FB_H - PAD_H)
                pad[1].y = remote_y;
        }
    }
}

static void update_leds(void)
{
    u32 v = ((u32)(score[1] & 0xF) << 4) | (u32)(score[0] & 0xF);
    XGpio_DiscreteWrite(&gpio1, 1, v);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RENDER_FRAME — dibuja el estado actual al framebuffer
 * ═══════════════════════════════════════════════════════════════════════════ */
static void render_frame(void)
{
    if (game_state != prev_game_state) {
        int was_game = (prev_game_state == ST_PLAYING || prev_game_state == ST_PAUSE);
        int is_game  = (game_state     == ST_PLAYING || game_state     == ST_PAUSE);
        if (!was_game || !is_game || prev_game_state < 0) {
            needs_full_redraw = 1;
            score_dirty       = 1;
        }
        if (prev_game_state == ST_PAUSE && game_state == ST_PLAYING)
            needs_full_redraw = 1;
        if (game_state == ST_PAUSE)
            prev_selected = -1;
        prev_game_state = game_state;
    }

    switch (game_state) {

    /* ── MENÚ ─────────────────────────────────────────────────────────── */
    case ST_MENU:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            if (load_step >= 4)
                fb_blit_scaled(160, 30, SPR_LOGO_W, 13, DDR2_SPR_LOGO, 1, 5);
            else
                fb_draw_str(221, 30, "PONG", 12, COL_WHITE);
            needs_full_redraw = 0;
            prev_selected = -1;
        }
        if (prev_selected != selected) {
            if (msel_loaded) {
                fb_fill_rect(120, 128, 400, 224, COL_BLACK);
                if (selected==0) fb_fill_rect(120, 167, 400, 38, COL_BLUE);
                else             fb_fill_rect(120, 233, 400, 38, COL_BLUE);
                fb_blit_scaled(120, 128, SPR_MSEL_W, SPR_MSEL_H, DDR2_SPR_MSEL, 1, 2);
            } else {
                fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
                fb_draw_digit(312, 183, 1, 6, COL_BLACK);
                fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
                fb_draw_digit(312, 268, 2, 6, COL_BLACK);
            }
            prev_selected = selected;
        }
        break;

    /* ── JUGANDO + PAUSA ─────────────────────────────────────────────── */
    case ST_PLAYING:
    case ST_PAUSE:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);

            /* 4. SCANLINES CRT — solo en redibujado completo */
            draw_scanlines();

            if (!mode_2p)
                for (int ny = 0; ny < FB_H; ny += 8)
                    fb_fill_rect(318, ny, 4, 4, COL_GRAY);
            fb_draw_scores();
            score_dirty = 0;
            {
                int bsx  = is_slave ? ball.x - FB_W : ball.x;
                int bvis = mode_2p ? (is_slave ? (ball.x >= FB_W) : (ball.x < FB_W)) : 1;
                if (bvis) fb_fill_rect(bsx, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
            }
            /* 1. PALETAS CON COLOR ARCOÍRIS */
            if (!mode_2p || !is_slave)
                fb_fill_rect(PAD1_X, pad[0].y, PAD_W, PAD_H,
                             rainbow_colors[rainbow_index]);
            if (!mode_2p || is_slave)
                fb_fill_rect(PAD2_X, pad[1].y, PAD_W, PAD_H,
                             rainbow_colors[rainbow_index]);

            prev_ball_x = ball.x; prev_ball_y = ball.y;
            prev_pad0_y = pad[0].y; prev_pad1_y = pad[1].y;
            needs_full_redraw = 0;

            if (game_state == ST_PAUSE) {
                if (pause_loaded) {
                    fb_fill_rect(120, 160, 400, 160, COL_BLACK);
                    if (selected==0) fb_fill_rect(120, 175, 400, 49, COL_BLUE);
                    else             fb_fill_rect(120, 255, 400, 49, COL_BLUE);
                    fb_blit_scaled(120, 160, SPR_PAUSE_W, SPR_PAUSE_H, DDR2_SPR_PAUSE, 1, 2);
                } else {
                    fb_fill_rect(220, 175, 200, 45, (selected==0) ? COL_YELLOW : COL_DGRAY);
                    fb_draw_str(249, 187, "REANUDAR", 4, COL_BLACK);
                    fb_fill_rect(220, 260, 200, 45, (selected==1) ? COL_YELLOW : COL_DGRAY);
                    fb_draw_str(276, 272, "SALIR", 4, COL_BLACK);
                }
                prev_selected = selected;
            }
            break;
        }

        /* Render incremental de la pelota */
        {
            int old_bsx, new_bsx, old_vis, new_vis;
            if (!mode_2p) {
                old_bsx = prev_ball_x; new_bsx = ball.x;
                old_vis = new_vis = 1;
            } else if (!is_slave) {
                old_bsx = prev_ball_x;        new_bsx = ball.x;
                old_vis = (prev_ball_x < FB_W); new_vis = (ball.x < FB_W);
            } else {
                old_bsx = prev_ball_x - FB_W;  new_bsx = ball.x - FB_W;
                old_vis = (prev_ball_x >= FB_W); new_vis = (ball.x >= FB_W);
            }
            int old_by = prev_ball_y;

            if ((old_vis || new_vis) && (new_bsx != old_bsx || ball.y != old_by)) {
                if (old_vis && new_vis) {
                    int lpad_x = (!mode_2p || !is_slave) ? PAD1_X : -99;
                    int lpad_y = (!mode_2p || !is_slave) ? pad[0].y : 0;
                    int rpad_x = (!mode_2p || is_slave)  ? PAD2_X : -99;
                    int rpad_y = (!mode_2p || is_slave)  ? pad[1].y : 0;
                    int special = (old_by < 42) ||
                                  (!mode_2p && old_bsx < 322 && old_bsx + BALL_SZ > 318) ||
                                  (old_bsx + BALL_SZ > lpad_x && old_bsx < lpad_x + PAD_W &&
                                   old_by + BALL_SZ > lpad_y && old_by < lpad_y + PAD_H) ||
                                  (old_bsx + BALL_SZ > rpad_x && old_bsx < rpad_x + PAD_W &&
                                   old_by + BALL_SZ > rpad_y && old_by < rpad_y + PAD_H);
                    if (special) {
                        fb_fill_rect(new_bsx, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
                        for (int r = 0; r < BALL_SZ; r++)
                            for (int d = 0; d < BALL_SZ; d++) {
                                int px = old_bsx + d, py = old_by + r;
                                if (px >= new_bsx && px < new_bsx + BALL_SZ &&
                                    py >= ball.y  && py < ball.y  + BALL_SZ) continue;
                                fb_set_pixel(px, py, bg_pixel(px, py));
                            }
                    } else {
                        int ovx = (old_bsx + BALL_SZ > new_bsx && new_bsx + BALL_SZ > old_bsx);
                        int ovy = (old_by  + BALL_SZ > ball.y  && ball.y  + BALL_SZ > old_by);
                        if (!(ovx && ovy)) {
                            fb_fill_rect(new_bsx, ball.y,  BALL_SZ, BALL_SZ, COL_WHITE);
                            fb_fill_rect_bg(old_bsx, old_by, BALL_SZ, BALL_SZ);
                        } else {
                            fb_fill_rect(new_bsx, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
                            for (int r = 0; r < BALL_SZ; r++) {
                                int py = old_by + r;
                                if (py >= ball.y && py < ball.y + BALL_SZ) {
                                    if (new_bsx > old_bsx)
                                        fb_fill_rect(old_bsx, py, new_bsx - old_bsx, 1, bg_row_color(py));
                                    if (new_bsx + BALL_SZ < old_bsx + BALL_SZ)
                                        fb_fill_rect(new_bsx + BALL_SZ, py,
                                                     (old_bsx + BALL_SZ) - (new_bsx + BALL_SZ), 1, bg_row_color(py));
                                } else {
                                    fb_fill_rect(old_bsx, py, BALL_SZ, 1, bg_row_color(py));
                                }
                            }
                        }
                    }
                } else if (old_vis) {
                    int lpad_x2 = (!mode_2p || !is_slave) ? PAD1_X : -99;
                    int lpad_y2 = (!mode_2p || !is_slave) ? pad[0].y : 0;
                    int rpad_x2 = (!mode_2p || is_slave)  ? PAD2_X : -99;
                    int rpad_y2 = (!mode_2p || is_slave)  ? pad[1].y : 0;
                    int special = (old_by < 42) ||
                                  (!mode_2p && old_bsx < 322 && old_bsx + BALL_SZ > 318) ||
                                  (old_bsx + BALL_SZ > lpad_x2 && old_bsx < lpad_x2 + PAD_W &&
                                   old_by + BALL_SZ > lpad_y2 && old_by < lpad_y2 + PAD_H) ||
                                  (old_bsx + BALL_SZ > rpad_x2 && old_bsx < rpad_x2 + PAD_W &&
                                   old_by + BALL_SZ > rpad_y2 && old_by < rpad_y2 + PAD_H);
                    if (special) {
                        for (int r = 0; r < BALL_SZ; r++)
                            for (int d = 0; d < BALL_SZ; d++)
                                fb_set_pixel(old_bsx+d, old_by+r, bg_pixel(old_bsx+d, old_by+r));
                    } else {
                        fb_fill_rect_bg(old_bsx, old_by, BALL_SZ, BALL_SZ);
                    }
                } else {
                    fb_fill_rect(new_bsx, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
                }
            }
        }

        /* Delta-rect paddles con color arcoíris.
         * Si esta paddle tiene el borde de golpe activo, se fuerza un
         * repintado completo (no las franjas parciales optimizadas) para
         * no dejar restos del borde blanco "atrapados" dentro del cuerpo
         * de la paddle al moverse. */
        if ((!mode_2p || !is_slave) && pad[0].y != prev_pad0_y) {
            u8 pc = rainbow_colors[rainbow_index];
            int dy = pad[0].y - prev_pad0_y;
            /* BUG 1+2 FIX: ya no hay caso especial para border_pad.
             * on_paddle_hit repintó la paddle entera al cambiar el color,
             * y draw_hit_border gestiona el borde blanco frame a frame. */
            if (dy < 0 && -dy < PAD_H) {
                fb_fill_rect(PAD1_X, pad[0].y,         PAD_W, -dy, pc);
                fb_fill_rect_bg(PAD1_X, pad[0].y + PAD_H, PAD_W, -dy);
            } else if (dy > 0 && dy < PAD_H) {
                fb_fill_rect_bg(PAD1_X, prev_pad0_y,         PAD_W, dy);
                fb_fill_rect(PAD1_X, prev_pad0_y + PAD_H, PAD_W, dy, pc);
            } else {
                fb_fill_rect_bg(PAD1_X, prev_pad0_y, PAD_W, PAD_H);
                fb_fill_rect(PAD1_X, pad[0].y,       PAD_W, PAD_H, pc);
            }
            prev_pad0_y = pad[0].y;
        }
        if ((!mode_2p || is_slave) && pad[1].y != prev_pad1_y) {
            u8 pc = rainbow_colors[rainbow_index];
            int dy = pad[1].y - prev_pad1_y;
            if (dy < 0 && -dy < PAD_H) {
                fb_fill_rect(PAD2_X, pad[1].y,         PAD_W, -dy, pc);
                fb_fill_rect_bg(PAD2_X, pad[1].y + PAD_H, PAD_W, -dy);
            } else if (dy > 0 && dy < PAD_H) {
                fb_fill_rect_bg(PAD2_X, prev_pad1_y,         PAD_W, dy);
                fb_fill_rect(PAD2_X, prev_pad1_y + PAD_H, PAD_W, dy, pc);
            } else {
                fb_fill_rect_bg(PAD2_X, prev_pad1_y, PAD_W, PAD_H);
                fb_fill_rect(PAD2_X, pad[1].y,       PAD_W, PAD_H, pc);
            }
            prev_pad1_y = pad[1].y;
        }

        prev_ball_x = ball.x; prev_ball_y = ball.y;

        /* 3. ONDA ROMBO — se dibuja incremental, solo el perímetro, radio acotado */
        draw_wave();

        /* 2. BORDE DE GOLPE — sigue a la paddle golpeada durante ~0.5 s */
        draw_hit_border();

        /* Overlay de pausa */
        if (!is_slave && game_state == ST_PAUSE && prev_selected != selected) {
            if (pause_loaded) {
                fb_fill_rect(120, 160, 400, 160, COL_BLACK);
                if (selected==0) fb_fill_rect(120, 175, 400, 49, COL_BLUE);
                else             fb_fill_rect(120, 255, 400, 49, COL_BLUE);
                fb_blit_scaled(120, 160, SPR_PAUSE_W, SPR_PAUSE_H, DDR2_SPR_PAUSE, 1, 2);
            } else {
                fb_fill_rect(220, 175, 200, 45, (selected==0) ? COL_YELLOW : COL_DGRAY);
                fb_draw_str(249, 187, "REANUDAR", 4, COL_BLACK);
                fb_fill_rect(220, 260, 200, 45, (selected==1) ? COL_YELLOW : COL_DGRAY);
                fb_draw_str(276, 272, "SALIR", 4, COL_BLACK);
            }
            prev_selected = selected;
        }
        break;

    /* ── ESPERA HANDSHAKE 2P (maestro) ─────────────────────────────── */
    case ST_WAIT_2P:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            fb_draw_str(115, 190, "MODO 2 JUGADORES", 5, COL_WHITE);
            fb_draw_str(110, 270, "ESPERANDO JUGADOR 2...", 3, COL_GRAY);
            needs_full_redraw = 0;
        }
        break;

    /* ── GAMEOVER ────────────────────────────────────────────────────── */
    case ST_GAMEOVER:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            {
                int sec;
                if (!mode_2p)
                    sec = (selected == 1) ? 0 : 2;
                else
                    sec = (selected == 1) ? 0 : 1;
                const u8 *spr = DDR2_SPR_GAMEOVER + sec * SPR_GO_SEC_BYTES;
                fb_blit_scaled(20, 90, SPR_GO_W, SPR_GO_SEC_H, spr, 0, 3);
            }
            needs_full_redraw = 0;
        }
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DDR2 — init y sprites por defecto
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ddr2_init(void)
{
    usleep(350000);
}

static void ddr2_selftest(void)
{
    static const u32 pat[4] = {
        0xDEADBEEFu, 0x12345678u, 0xA5A5A5A5u, 0x0F0F0F0Fu
    };
    u32 base = DDR2_BASE + 0x20000UL;
    u32 result = 0;

    for (int i = 0; i < 4; i++)
        Xil_Out32(base + (u32)(i * 4), pat[i]);

    for (int i = 0; i < 4; i++)
        if (Xil_In32(base + (u32)(i * 4)) == pat[i])
            result |= (1u << i);

    XGpio_DiscreteWrite(&gpio1, 1, result);
    usleep(2000000);
    XGpio_DiscreteWrite(&gpio1, 1, 0);
}

static void ddr2_sprite_defaults(void)
{
    u32 addr;
    int i;

    addr = (u32)(uintptr_t)DDR2_SPR_BALL;
    for (i = 0; i < SPR_BALL_W * SPR_BALL_H / 8; i++)
        Xil_Out32(addr + (u32)(i * 4), 0x11111111u);

    addr = (u32)(uintptr_t)DDR2_SPR_PADDLE;
    for (i = 0; i < SPR_PAD_W * SPR_PAD_H / 8; i++)
        Xil_Out32(addr + (u32)(i * 4), 0x11111111u);

    addr = (u32)(uintptr_t)DDR2_SPR_LOGO;
    for (i = 0; i < SPR_LOGO_W * SPR_LOGO_H / 8; i++)
        Xil_Out32(addr + (u32)(i * 4), 0x00000000u);

    sprites_ok = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SLAVE_LOOP
 * ═══════════════════════════════════════════════════════════════════════════ */
static void slave_loop(void)
{
    XSpi_Stop(&spi);
    XSpi_SetOptions(&spi, XSP_MANUAL_SSELECT_OPTION);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);

    fb_clear(COL_BLACK);
    fb_draw_str(200, 190, "JUGADOR 2", 6, COL_WHITE);
    fb_draw_str(110, 270, "ESPERANDO JUGADOR 1...", 3, COL_GRAY);

    {
        while (XGpio_DiscreteRead(&gpio0, 1) & BTN_C) wait_vsync();

        (void)Xil_In32(SPI2P_BASE + SPI_SR);
        u32 cr = SPICR_MANSS | SPICR_SPE;
        Xil_Out32(SPI2P_BASE + SPI_CR, cr | SPICR_TXRST | SPICR_RXRST);
        Xil_Out32(SPI2P_BASE + SPI_CR, cr);

        u32 prev_lvl = 0;
        while (1) {
            u32 sr = Xil_In32(SPI2P_BASE + SPI_SR);
            if (sr & SPISR_TX_EMPTY)
                Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)SPI_PONG);

            wait_vsync();

            u32 cur_lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
            if ((cur_lvl & BTN_C) && !(prev_lvl & BTN_C)) {
                Xil_Out32(SPI2P_BASE + SPI_CR, cr | SPICR_INHIBIT);
                goto slave_exit;
            }
            prev_lvl = cur_lvl;

            if (!(Xil_In32(SPI2P_BASE + SPI_SR) & SPISR_RX_EMPTY)) {
                u8 rx = (u8)Xil_In32(SPI2P_BASE + SPI_DRR);
                if (rx == SPI_PING) break;
            }
        }
        Xil_Out32(SPI2P_BASE + SPI_CR, cr | SPICR_INHIBIT);
    }

    {
        (void)Xil_In32(SPI2P_BASE + SPI_SR);
        u32 c = SPICR_MANSS | SPICR_SPE;
        Xil_Out32(SPI2P_BASE + SPI_CR, c | SPICR_TXRST | SPICR_RXRST);
        Xil_Out32(SPI2P_BASE + SPI_CR, c);
    }

    mode_2p           = 1;
    is_slave          = 1;
    game_state        = ST_PLAYING;
    needs_full_redraw = 1;
    score_dirty       = 1;
    init_game();

    {
        u32 gl_prev  = 0;
        u8  slv_btnc = 0;

        Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)(pad[1].y >> 1));
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);

        while (1) {
        u32 lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;

        if ((lvl & BTN_C) && !(gl_prev & BTN_C) && !(lvl & (BTN_U | BTN_D))) {
            if (game_state == ST_PLAYING || game_state == ST_PAUSE)
                slv_btnc = 1;
        }
        gl_prev = lvl;

        if (game_state == ST_MENU) goto slave_exit;

        if (lvl & BTN_U) move_local_pad(1, 1);
        if (lvl & BTN_D) move_local_pad(1, 0);

        update_leds();
        render_frame();

        Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)(pad[1].y >> 1));
        Xil_Out32(SPI2P_BASE + SPI_DTR, (u32)slv_btnc);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);
        Xil_Out32(SPI2P_BASE + SPI_DTR, 0u);

        {
            u8 tmp[16];
            int n = 0;
            while (n < 16 && !(Xil_In32(SPI2P_BASE + SPI_SR) & SPISR_RX_EMPTY))
                tmp[n++] = (u8)Xil_In32(SPI2P_BASE + SPI_DRR);
            if (n >= 8) {
                u8 *rx = tmp + (n - 8);
                slv_btnc = 0;
                ball.x   = ((int)rx[0] << 8) | rx[1];
                ball.y   = ((int)rx[2] << 8) | rx[3];
                pad[0].y = ((int)rx[4] << 8) | rx[5];
                int ns   = (rx[6] >> 4) & 0xF;
                score[0] = rx[6] & 0xF;
                selected = (rx[7] >> 4) & 0xF;
                score[1] = rx[7] & 0xF;
                score_dirty = 1;
                if (ns >= ST_PLAYING && ns <= ST_GAMEOVER && ns != game_state) {
                    game_state        = ns;
                    needs_full_redraw = 1;
                }
                if (ns == ST_MENU) goto slave_exit;
            }
        }

        wait_vsync();
        }
    }

slave_exit:
    XSpi_Stop(&spi);
    XSpi_SetOptions(&spi, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    XSpi_SetSlaveSelect(&spi, 0x01u);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);
    mode_2p          = 0;
    is_slave         = 0;
    game_state       = ST_MENU;
    needs_full_redraw = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    xil_printf("\r\n=== PONG BOOT ===\r\n");

    XGpio_Initialize(&gpio0, XPAR_AXI_GPIO_0_BASEADDR);
    XGpio_SetDataDirection(&gpio0, 1, 0x3Fu);
    XGpio_SetDataDirection(&gpio0, 2, 0x01u);

    XGpio_Initialize(&gpio1, XPAR_AXI_GPIO_1_BASEADDR);
    XGpio_SetDataDirection(&gpio1, 1, 0x0000u);

    XSpi_Config *spi_cfg = XSpi_LookupConfig(XPAR_AXI_QUAD_SPI_0_BASEADDR);
    XSpi_CfgInitialize(&spi, spi_cfg, spi_cfg->BaseAddress);
    XSpi_SetOptions(&spi, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    XSpi_SetSlaveSelect(&spi, 0x01u);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);

    xil_printf("INFO: DDR2 init...\r\n");
    ddr2_init();
    xil_printf("INFO: DDR2 selftest...\r\n");
    ddr2_selftest();
    xil_printf("INFO: DDR2 sprites...\r\n");
    ddr2_sprite_defaults();

    game_state = ST_MENU;
    selected   = 0;
    mode_2p    = 0;
    score[0]   = score[1] = 0;
    init_game();
    usleep(500000);
    sd_ok = sd_run_test();
    load_sprites();

    render_frame();

    while (1) {
        u32 btn = btn_edge();

        if (mode_2p && game_state != ST_WAIT_2P) {
            spi_exchange();
            if (game_state == ST_MENU) { mode_2p = 0; is_slave = 0; }
        }

        switch (game_state) {

        case ST_MENU:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                mode_2p  = (selected == 1) ? 1 : 0;
                score[0] = score[1] = 0;
                init_game();
                if (mode_2p) {
                    is_slave = sw_on();
                    if (is_slave) {
                        slave_loop();
                    }
                    if (mode_2p) {
                        game_state        = ST_WAIT_2P;
                        needs_full_redraw = 1;
                    }
                } else {
                    game_state = ST_PLAYING;
                }
            }
            break;

        case ST_WAIT_2P: {
            if (btn & BTN_C) {
                mode_2p = 0; is_slave = 0;
                game_state = ST_MENU; needs_full_redraw = 1; break;
            }
            u8 tx = SPI_PING, rx = 0;
            int rc = XSpi_Transfer(&spi, &tx, &rx, 1);
            if (rc == XST_SUCCESS && rx == SPI_PONG) {
                score[0] = score[1] = 0;
                init_game();
                game_state        = ST_PLAYING;
                needs_full_redraw = 1;
            }
            break;
        }

        case ST_PLAYING:
            {
                u32 lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
                if ((btn & BTN_C) && !(lvl & (BTN_U | BTN_D))) {
                    selected = 0; game_state = ST_PAUSE; break;
                }
                if (mode_2p) {
                    if (lvl & BTN_U) move_local_pad(0, 1);
                    if (lvl & BTN_D) move_local_pad(0, 0);
                } else {
                    if (lvl & BTN_U) move_local_pad(1, 1);
                    if (lvl & BTN_D) move_local_pad(1, 0);
                }
            }

            if (!mode_2p) move_ai();

            if (++ball_tick >= BALL_TICK_DIV) { ball_tick = 0; move_ball(); }
            update_leds();

            if (check_score()) game_state = ST_GAMEOVER;
            break;

        case ST_PAUSE:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                game_state = (selected == 0) ? ST_PLAYING : ST_MENU;
                if (selected != 0) selected = 0;
            }
            break;

        case ST_GAMEOVER:
            if (btn & BTN_C) { game_state = ST_MENU; selected = 0; }
            break;
        }

        wait_vsync();
        render_frame();
    }

    return 0;
}
