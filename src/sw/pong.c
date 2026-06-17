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
#define DDR2_SPR_GAMEOVER ((u8 *)(DDR2_BASE + 0x41000UL))  /* 18000 B — 160×225 4bpp (3 secciones) */
#define SPR_GO_W         160
#define SPR_GO_SEC_H      75   /* alto de cada sección (3 secciones × 75 = 225) */
#define SPR_GO_SEC_BYTES (SPR_GO_W * SPR_GO_SEC_H / 2)  /* 6000 B por sección */

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
#define BALL_TICK_DIV 2    /* pelota avanza cada 2 frames (~60 Hz con usleep 8 ms) */
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

/* Sprites viven en DDR2 — ver DDR2_SPR_BALL / _PADDLE / _LOGO */
static u8 sd_sector_buf[512];

/* Layout de sectores en la SD (LBA, antes de la partición → sector 0 = MBR) */
#define SD_MAGIC      0x504F4E47u   /* "PONG" */
#define SD_LBA_HDR    1
#define SD_LBA_BALL   2
#define SD_LBA_PADDLE 3
#define SD_LBA_LOGO     5   /* 512 B = 1 sector exacto */
#define SD_LBA_GAMEOVER 6   /* 160×225 4bpp = 18000 B = 36 sectores (LBA 6-41) */

/* ── Estado global ───────────────────────────────────────────────────────── */
static ball_t  ball;
static pad_t   pad[2];
static int     score[2];
static int     game_state;
static int     selected;
static int     mode_2p;
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

/* Dirty-rect state — evita fb_clear() por frame */
static int     ball_tick         = 0;
static int     hit_count         = 0;
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

/* Espera flanco de subida del vsync. Si el bit no está disponible (GPIO <6 bits) regresa
   inmediatamente para no bloquear el loop. */
static void wait_vsync(void)
{
    int t;
    /* Si no está en HIGH ahora, el bit no funciona (siempre 0) — no esperar */
    if (!(Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC)) return;
    /* Esperar LOW (inicio del pulso vsync) */
    t = 2000000;
    while ((Xil_In32(XPAR_AXI_GPIO_0_BASEADDR) & BTN_VSYNC) && --t);
    if (!t) return;
    /* Esperar HIGH (fin del pulso = inicio del back porch) */
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

/* Dibuja un rectángulo relleno.  Usa read-modify-write solo en nibbles parciales. */
static void fb_fill_rect(int x, int y, int w, int h, u8 color)
{
    u8  c     = color & 0xF;
    int x1    = (x < 0)    ? 0   : x;
    int x2    = (x+w > FB_W) ? FB_W : x+w;
    int y1    = (y < 0)    ? 0   : y;
    int y2    = (y+h > FB_H) ? FB_H : y+h;

    for (int row = y1; row < y2; row++) {
        int col = x1;

        /* Nibbles iniciales hasta alineación a 8 px */
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

        /* Palabras completas (8 píxeles alineados) — solo escritura */
        u32 pattern = (u32)c * 0x11111111u;
        while (col + 8 <= x2) {
            u32 pidx = (u32)row * FB_W + col;
            Xil_Out32(FB_BASE + ((pidx >> 3) << 2), pattern);
            col += 8;
        }

        /* Nibbles finales */
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

/* Font 4×5 para dígitos 0-9.  bit 3 = columna izquierda, bit 0 = columna derecha. */
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

/*
 * Dibuja un dígito en (x,y) con factor de escala `scale` (px por celda).
 * Resultado: (4×scale) × (5×scale) píxeles.
 */
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

/* Dibuja el marcador centrado arriba: dos dígitos a cada lado de la red */
static void fb_draw_scores(void)
{
    /* Dígitos de 6×6 px/celda → 24×30 px por dígito */
    fb_draw_digit(256, 8, score[0], 6, COL_WHITE);
    fb_draw_digit(360, 8, score[1], 6, COL_WHITE);
}

/* Devuelve el color de fondo estático en (x,y).
   Conoce la línea central y los dígitos del marcador para poder
   restaurar el fondo exacto al borrar la pelota sin redibujarlo. */
static u8 bg_pixel(int x, int y)
{
    /* Línea central: 4px gris / 4px negro alternados en bloques de 8 */
    if (x >= 318 && x < 322 && (y & 7) < 4) return COL_GRAY;
    /* Dígito izquierdo (256,8) escala 6 → celdas de 6×6 px */
    if (x >= 256 && x < 280 && y >= 8 && y < 38) {
        u8 p = font4x5[score[0] % 10][(y - 8) / 6];
        if (p & (0x8u >> ((x - 256) / 6))) return COL_WHITE;
    }
    /* Dígito derecho (360,8) escala 6 */
    if (x >= 360 && x < 384 && y >= 8 && y < 38) {
        u8 p = font4x5[score[1] % 10][(y - 8) / 6];
        if (p & (0x8u >> ((x - 360) / 6))) return COL_WHITE;
    }
    return COL_BLACK;
}

/* Escribe un solo píxel con RMW (para restauración de fondo). */
static void fb_set_pixel(int x, int y, u8 color)
{
    u32 pidx  = (u32)y * FB_W + x;
    u32 addr  = FB_BASE + ((pidx >> 3) << 2);
    u32 shift = (7u - (pidx & 7u)) * 4u;
    u32 word  = Xil_In32(addr);
    Xil_Out32(addr, (word & ~(0xFu << shift)) | ((u32)color << shift));
}

/*
 * Blit de sprite 4-bit (high nibble = px izquierdo) al framebuffer.
 * Si transparent != 0, los píxeles con valor 0 (negro) no se escriben.
 */
static void fb_blit(int x, int y, int w, int h, const u8 *spr, int transparent)
{
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= FB_H) continue;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= FB_W) continue;
            int idx = row * w + col;
            u8  px  = (idx & 1) ? (spr[idx >> 1] & 0xF) : (spr[idx >> 1] >> 4);
            if (transparent && px == 0) continue;
            u32 pidx  = (u32)sy * FB_W + sx;
            u32 addr  = FB_BASE + ((pidx >> 3) << 2);
            u32 shift = (7u - (pidx & 7u)) * 4u;
            u32 word  = Xil_In32(addr);
            Xil_Out32(addr, (word & ~(0xFu << shift)) | ((u32)px << shift));
        }
    }
}

/* Blit con escala entera (sc=1 → 1:1, sc=4 → cada px del sprite = 4×4 en FB). */
static void fb_blit_scaled(int x, int y, int w, int h, const u8 *spr, int transparent, int sc)
{
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int idx = row * w + col;
            u8  px  = (idx & 1) ? (spr[idx >> 1] & 0xF) : (spr[idx >> 1] >> 4);
            if (transparent && px == 0) continue;
            fb_fill_rect(x + col * sc, y + row * sc, sc, sc, px);
        }
    }
}

/* Mini-fuente de letras: A,D,E,G,I,L,N,R,S,T,U (4×5 px, nibble MSB = col izquierdo) */
static const u8 font_ext[11][5] = {
    {0x6, 0x9, 0xF, 0x9, 0x9},  /* A */
    {0xE, 0x9, 0x9, 0x9, 0xE},  /* D */
    {0xF, 0x8, 0xE, 0x8, 0xF},  /* E */
    {0x7, 0x8, 0xB, 0x9, 0x7},  /* G */
    {0xF, 0x6, 0x6, 0x6, 0xF},  /* I */
    {0x8, 0x8, 0x8, 0x8, 0xF},  /* L */
    {0x9, 0xD, 0xB, 0x9, 0x9},  /* N */
    {0xE, 0x9, 0xE, 0xA, 0x9},  /* R */
    {0x7, 0x8, 0x6, 0x1, 0xE},  /* S */
    {0xF, 0x6, 0x6, 0x6, 0x6},  /* T */
    {0x9, 0x9, 0x9, 0x9, 0x6},  /* U */
};

static int char_idx(char c) {
    switch (c) {
        case 'A': return 0; case 'D': return 1; case 'E': return 2;
        case 'G': return 3; case 'I': return 4; case 'L': return 5;
        case 'N': return 6; case 'R': return 7; case 'S': return 8;
        case 'T': return 9; case 'U': return 10;
        default:  return -1;
    }
}

static void fb_draw_str(int x, int y, const char *s, int scale, u8 color) {
    while (*s) {
        int idx = char_idx(*s++);
        if (idx >= 0) {
            const u8 *g = font_ext[idx];
            for (int row = 0; row < 5; row++)
                for (int col = 0; col < 4; col++)
                    if (g[row] & (0x8u >> col))
                        fb_fill_rect(x + col*scale, y + row*scale, scale, scale, color);
        }
        x += 4*scale + 2;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * SD CARD — driver mínimo SPI manual (modo SPI 0, CPOL=0 CPHA=0)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SD_BASE  XPAR_AXI_QUAD_SPI_1_BASEADDR

static void sd_spi_setup(void)
{
    /* Software reset AXI Quad SPI (SRR offset correcto: 0x40) */
    Xil_Out32(SD_BASE + SPI_SRR, 0x0Au);
    usleep(100);
    /* Resetear TX y RX FIFOs via bits CR[5:6] */
    Xil_Out32(SD_BASE + SPI_CR,
              SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE |
              SPICR_TXRST | SPICR_RXRST);
    usleep(10);
    /* Modo normal: master, manual SS, transfer inhibited, SPE */
    Xil_Out32(SD_BASE + SPI_CR, SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);   /* CS desasertado */
}

static u8 sd_spi_byte(u8 tx)
{
    /* Drenar RX FIFO de datos obsoletos antes de iniciar nueva transferencia */
    while (!(Xil_In32(SD_BASE + SPI_SR) & SPISR_RX_EMPTY))
        (void)Xil_In32(SD_BASE + SPI_DRR);

    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_DTR, tx);
    Xil_Out32(SD_BASE + SPI_CR, cr & ~SPICR_INHIBIT);  /* iniciar transferencia */

    /* Esperar TX vacío Y RX con dato: garantiza que el shift register terminó */
    for (int t = 0; t < 100000; t++) {
        u32 sr = Xil_In32(SD_BASE + SPI_SR);
        if ((sr & SPISR_TX_EMPTY) && !(sr & SPISR_RX_EMPTY)) break;
    }
    Xil_Out32(SD_BASE + SPI_CR, cr);  /* detener transferencia (INHIBIT) */
    return (u8)Xil_In32(SD_BASE + SPI_DRR);
}

/* Verifica que el IP AXI Quad SPI funcione enviando 0x5A en loopback.
 * Retorna 1 si el byte enviado = byte recibido, 0 si hay error. */
static int sd_loopback_test(void)
{
    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_CR, cr | SPICR_LOOP);   /* activar loopback */
    u8 echo = sd_spi_byte(0x5A);
    Xil_Out32(SD_BASE + SPI_CR, cr);                  /* desactivar loopback */
    return (echo == 0x5A) ? 1 : 0;
}

/*
 * Inicializa la SD en modo SPI.
 * Retorna  0: OK
 *         -1: CMD0 sin respuesta (tarjeta no detectada o SCK no llega)
 *         -2: ACMD41 timeout (tarjeta no sale de idle — posible SD dañada)
 */
static int sd_init(void)
{
    sd_spi_setup();

    /* 80 pulsos de clock con CS desasertado */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 10; i++) sd_spi_byte(0xFF);

    /* Aserta CS[0] — CMD0 en SPI mode requiere CS bajo ANTES del primer bit */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD0: GO_IDLE_STATE — espera R1=0x01
     * Reintentar hasta 10 veces (con gap inter-cmd) en caso de mala sincronización. */
    const u8 cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    u8 r1 = 0xFF;
    for (int attempt = 0; attempt < 10 && r1 != 0x01; attempt++) {
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd0[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        if (r1 != 0x01) {
            /* Gap inter-comando: desaserta CS, 8 clocks, reaserta */
            Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
            for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
            Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
            usleep(1000);
        }
    }
    sd_cmd0_r1 = r1;
    if (r1 != 0x01) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    /* Gap post-CMD0: desaserta CS + 8 clocks + reaserta */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD8: SEND_IF_COND (SDHC check)
     * R1=0x01 → SDHC (consume 4 echo bytes R7)
     * R1=0x05 → SDSC legacy (no echo)
     * R1=0xFF → no responde (tarjeta antigua o sin soporte) */
    const u8 cmd8[] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd8[i]);
    u8 r8 = 0xFF;
    for (int i = 0; i < 8 && r8 == 0xFF; i++) r8 = sd_spi_byte(0xFF);
    sd_cmd8_r1 = r8;
    if (r8 == 0x01) {
        for (int i = 0; i < 4; i++) sd_spi_byte(0xFF);  /* consume 4 echo bytes R7 */
    } else {
        for (int i = 0; i < 8; i++) sd_spi_byte(0xFF);  /* limpiar bus */
    }

    /* Gap post-CMD8 */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 1; i++) sd_spi_byte(0xFF);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* ACMD41: espera hasta salir de idle.
     * CS se toglea entre comandos: algunas tarjetas SDHC lo requieren.
     * Máx 200 intentos × 10 ms = 2 s de timeout. */
    int tries = 0;
    do {
        /* CMD55 — CS bajo, enviar, leer R1, CS alto + 8 clocks */
        Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
        const u8 cmd55[] = {0x77, 0x00, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd55[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        sd_cmd55_r1 = r1;
        Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
        sd_spi_byte(0xFF);   /* 8 clocks con CS alto */

        /* CMD41 — CS bajo, enviar, leer R1, CS alto + 8 clocks */
        Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);
        const u8 cmd41[] = {0x69, 0x40, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd41[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
        sd_spi_byte(0xFF);   /* 8 clocks con CS alto */

        tries++;
        usleep(10000);   /* 10 ms por intento → 2 s de timeout total */
    } while (r1 != 0x00 && tries < 200);
    sd_acmd41_r1  = r1;
    sd_acmd41_try = tries;

    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -2; }

    /* Reaserta CS para CMD58 */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD58: READ_OCR — detectar SDHC (bit CCS en OCR[30]) */
    const u8 cmd58[] = {0x7A, 0x00, 0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd58[i]);
    u8 r58 = 0xFF;
    for (int i = 0; i < 20 && r58 == 0xFF; i++) r58 = sd_spi_byte(0xFF);
    sd_cmd58_r1 = r58;
    u8 ocr0 = sd_spi_byte(0xFF);
    sd_ocr0 = ocr0;
    sd_spi_byte(0xFF); sd_spi_byte(0xFF); sd_spi_byte(0xFF);  /* ocr[1..3] */
    sd_sdhc = (r58 == 0x00 && (ocr0 & 0x40)) ? 1 : 0;

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    return 0;
}

/*
 * Lee un bloque de 512 bytes desde el sector LBA.
 * SDHC: dirección por bloques.  SDSC: dirección por bytes (lba << 9).
 */
static int sd_read_block(u32 lba, u8 *buf)
{
    u32 addr = sd_sdhc ? lba : (lba << 9);

    /* Gap mínimo SD SPI: 8+ ciclos idle con CS desasertado antes de cualquier cmd */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 2; i++) sd_spi_byte(0xFF);

    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);   /* CS assert */

    u8 cmd[6] = { 0x51,
        (u8)(addr >> 24), (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, 0x01 };
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd[i]);

    u8 r1 = 0xFF;
    for (int i = 0; i < 20 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
    sd_read_r1 = r1;
    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    u8 tok = 0xFF;
    for (int i = 0; i < 500 && tok != 0xFE; i++) tok = sd_spi_byte(0xFF);
    sd_read_token = tok;
    if (tok != 0xFE) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    for (int i = 0; i < 512; i++) buf[i] = sd_spi_byte(0xFF);
    sd_spi_byte(0xFF); sd_spi_byte(0xFF);  /* CRC */

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);   /* CS deassert */
    return 0;
}

/*
 * Test de diagnóstico de la SD — imprime resultados por UART.
 * Retorna 1 si la SD quedó lista, 0 si falló (el juego sigue igual).
 *
 * Secuencia:
 *   1. sd_init()  — negociación SPI + detección SDHC
 *   2. Leer sector 0 (MBR) — verifica que SCK/MOSI/MISO funcionen
 *   3. Comprobar firma MBR 0x55AA en bytes 510-511
 */
static int sd_run_test(void)
{
    xil_printf("\r\n=== SD TEST ===\r\n");

    sd_spi_setup();
    sd_loopback_ok = sd_loopback_test();
    xil_printf("Loopback: %s\r\n", sd_loopback_ok ? "PASS" : "FAIL");

    int rc = sd_init();
    sd_init_rc = rc;
    if (rc == -1) {
        xil_printf("FAIL CMD0: tarjeta no responde "
                   "(verificar SD_RESET=0, SCK llega al pin B1)\r\n");
        return 0;
    }
    if (rc == -2) {
        xil_printf("FAIL ACMD41: tarjeta no sale de idle "
                   "(timeout 200 intentos)\r\n");
        return 0;
    }
    xil_printf("PASS init: SD lista, tipo=%s\r\n",
               sd_sdhc ? "SDHC/SDXC" : "SDSC");

    /* Pausa post-init: garantiza que la tarjeta esté lista antes del primer CMD17 */
    usleep(10000);

    /* Leer sector 0 (MBR) */
    if (sd_read_block(0, sd_sector_buf) != 0) {
        xil_printf("FAIL read LBA0: CMD17 sin token de datos\r\n");
        return 0;
    }
    xil_printf("PASS read LBA0: primeros 4 bytes = %02X %02X %02X %02X\r\n",
               sd_sector_buf[0], sd_sector_buf[1],
               sd_sector_buf[2], sd_sector_buf[3]);

    /* Verificar firma MBR */
    if (sd_sector_buf[510] == 0x55 && sd_sector_buf[511] == 0xAA) {
        xil_printf("PASS MBR: firma 0x55AA encontrada\r\n");
    } else {
        xil_printf("WARN MBR: firma no encontrada "
                   "(bytes 510-511 = %02X %02X) "
                   "-- SD sin particionar o tarjeta en blanco\r\n",
                   sd_sector_buf[510], sd_sector_buf[511]);
    }

    xil_printf("=== SD TEST OK ===\r\n\r\n");
    return 1;
}

/* Carga los sprites desde los sectores reservados de la SD. */
static void load_sprites(void)
{
    if (!sd_ok) return;

    /* Verificar magic "PONG" en sector cabecera */
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
    for (int s = 0; s < 36; s++) {
        if (sd_read_block(SD_LBA_GAMEOVER + s, sd_sector_buf) != 0) return;
        int loaded = s * 512;
        int remain = (SPR_GO_W * 225 / 2) - loaded;
        int copy   = (remain >= 512) ? 512 : remain;
        if (copy <= 0) break;
        for (int i = 0; i < copy; i++) dst[loaded + i] = sd_sector_buf[i];
    }
    load_step = 5;

    sprites_ok = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LÓGICA DEL JUEGO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void init_game(void)
{
    u32 r = rng_next();
    /* Posición Y aleatoria en la mitad central de la pantalla */
    ball.x  = FB_W / 2;
    ball.y  = FB_H / 4 + (int)(r % (FB_H / 2));
    /* Dirección X: alterna hacia el último perdedor (r bit 0); dy: 2–4 aleatorio */
    ball.dx = (r & 1u) ? 4 : -4;
    ball.dy = (int)(2 + (rng_next() % 3));  /* 2, 3 o 4 */
    if (rng_next() & 1u) ball.dy = -ball.dy;
    pad[0].y = (FB_H - PAD_H) / 2;
    pad[1].y = (FB_H - PAD_H) / 2;
    ball_tick = 0;
    hit_count = 0;
    needs_full_redraw = 1;
}

/* Detección barrida: verdadero si la pelota tocó o cruzó la paleta este frame */
static int collide(ball_t *b, int px, int py)
{
    /* Rango X que la pelota recorrió este frame (incluyendo posición previa) */
    int x0 = b->x - b->dx;  /* posición antes del movimiento */
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
    if (pad[p].y < 0)           pad[p].y = 0;
    if (pad[p].y > FB_H - PAD_H) pad[p].y = FB_H - PAD_H;
}

static void move_ai(void)
{
    static int freeze      = 0;
    static int interval    = 0;
    static int hit_offset  = PAD_H / 2;  /* zona de la paleta con que se golpea */
    static int last_dx_ai  = 0;

    /* Sortear zona de golpeo cada vez que la pelota viene hacia la IA */
    if (ball.dx < 0 && last_dx_ai >= 0)
        hit_offset = (int)(rng_next() % (PAD_H - BALL_SZ + 1));
    last_dx_ai = ball.dx;

    /* Cada ~3.5 s (300 frames) se sortea una probabilidad aleatoria entre 0% y 2% */
    if (freeze == 0) {
        if (++interval >= 300) {
            interval = 0;
            int prob = (int)(rng_next() % 3);
            if ((int)(rng_next() % 100) < prob)
                freeze = 15 + (int)(rng_next() % 16);
        }
    }

    if (freeze > 0) { freeze--; return; }

    /* Seguimiento: mover la paleta para que hit_offset quede alineado con la pelota */
    int target = ball.y + BALL_SZ / 2 - hit_offset;
    int spd    = ball.dy < 0 ? -ball.dy : ball.dy;
    if (spd < 2) spd = 2;

    if (ball.dx < 0) {
        if (pad[0].y < target) pad[0].y += spd;
        else if (pad[0].y > target) pad[0].y -= spd;
    } else {
        int mid = (FB_H - PAD_H) / 2;
        if      (pad[0].y < mid) pad[0].y += spd;
        else if (pad[0].y > mid) pad[0].y -= spd;
    }

    if (pad[0].y < 0)            pad[0].y = 0;
    if (pad[0].y > FB_H - PAD_H) pad[0].y = FB_H - PAD_H;
}

static void move_ball(void)
{
    ball.x += ball.dx;
    ball.y += ball.dy;

    if (ball.y < 0)               { ball.y = 0;              ball.dy = -ball.dy; }
    if (ball.y > FB_H - BALL_SZ)  { ball.y = FB_H - BALL_SZ; ball.dy = -ball.dy; }

    if (ball.x < 0)               { score[1]++; score_dirty = 1; init_game(); return; }
    if (ball.x > FB_W - BALL_SZ)  { score[0]++; score_dirty = 1; init_game(); return; }

    /* Colisión paleta izquierda */
    if (collide(&ball, PAD1_X, pad[0].y)) {
        int hit = (pad[0].y + PAD_H) - ball.y;
        int spd = 4 + (++hit_count);
        if (spd > 25) spd = 25;
        ball.dx = spd;
        ball.dy = (hit < 8) ? 4 : (hit < 16) ? 3 : (hit < 24) ? 2 :
                  (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                  (hit < 46) ? -2 : (hit < 54) ? -3 : -4;
        if (ball.x < PAD1_X + PAD_W + 2) ball.x = PAD1_X + PAD_W + 2;
    }

    /* Colisión paleta derecha */
    if (collide(&ball, PAD2_X, pad[1].y)) {
        int hit = (pad[1].y + PAD_H) - ball.y;
        int spd = 4 + (++hit_count);
        if (spd > 25) spd = 25;
        ball.dx = -spd;
        ball.dy = (hit < 8) ? 4 : (hit < 16) ? 3 : (hit < 24) ? 2 :
                  (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                  (hit < 46) ? -2 : (hit < 54) ? -3 : -4;
        if (ball.x > PAD2_X - BALL_SZ - 2) ball.x = PAD2_X - BALL_SZ - 2;
    }
}

/* ── SPI inter-FPGA 2P ───────────────────────────────────────────────────── */
static u32 btn_prev = 0;

static u32 btn_edge(void)
{
    u32 cur  = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
    u32 edge = cur & ~btn_prev;
    btn_prev = cur;
    return edge;
}

static int sw_on(void)
{
    return (int)(XGpio_DiscreteRead(&gpio0, 2) & 0x1u);
}

static void spi_exchange(void)
{
    u8 tx[2], rx[2];
    tx[0] = (u8)((pad[1].y >> 8) & 0xFF);
    tx[1] = (u8)( pad[1].y       & 0xFF);
    if (XSpi_Transfer(&spi, tx, rx, 2) == XST_SUCCESS) {
        int ry = ((int)rx[0] << 8) | rx[1];
        if (ry >= 0 && ry <= FB_H - PAD_H) pad[0].y = ry;
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
    /* Detectar cambio de estado */
    if (game_state != prev_game_state) {
        int was_game = (prev_game_state == ST_PLAYING || prev_game_state == ST_PAUSE);
        int is_game  = (game_state     == ST_PLAYING || game_state     == ST_PAUSE);
        if (!was_game || !is_game || prev_game_state < 0) {
            needs_full_redraw = 1;
            score_dirty       = 1;
        }
        /* Al reanudar desde pausa: limpiar overlay */
        if (prev_game_state == ST_PAUSE && game_state == ST_PLAYING)
            needs_full_redraw = 1;
        /* Al entrar en pausa: forzar redibujado del overlay */
        if (game_state == ST_PAUSE)
            prev_selected = -1;
        prev_game_state = game_state;
    }

    switch (game_state) {

    /* ── MENÚ ─────────────────────────────────────────────────────────── */
    case ST_MENU:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            if (sprites_ok)
                fb_blit_scaled(192, 40, SPR_LOGO_W, SPR_LOGO_H, DDR2_SPR_LOGO, 1, 4);
            needs_full_redraw = 0;
            prev_selected = -1;
        }
        if (prev_selected != selected) {
            fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
            fb_draw_digit(312, 183, 1, 6, COL_BLACK);
            fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
            fb_draw_digit(312, 268, 2, 6, COL_BLACK);
            prev_selected = selected;
        }
        break;

    /* ── JUGANDO + PAUSA ─────────────────────────────────────────────── */
    case ST_PLAYING:
    case ST_PAUSE:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            for (int ny = 0; ny < FB_H; ny += 8)
                fb_fill_rect(318, ny, 4, 4, COL_GRAY);
            fb_draw_scores();
            score_dirty = 0;
            fb_fill_rect(ball.x, ball.y,   BALL_SZ, BALL_SZ, COL_WHITE);
            fb_fill_rect(PAD1_X, pad[0].y, PAD_W,   PAD_H,   COL_WHITE);
            fb_fill_rect(PAD2_X, pad[1].y, PAD_W,   PAD_H,   COL_WHITE);
            prev_ball_x = ball.x; prev_ball_y = ball.y;
            prev_pad0_y = pad[0].y; prev_pad1_y = pad[1].y;
            needs_full_redraw = 0;
            if (game_state == ST_PAUSE) {
                fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
                fb_draw_str(249, 187, "REANUDAR", 4, COL_BLACK);
                fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
                fb_draw_str(276, 272, "SALIR", 4, COL_BLACK);
                prev_selected = selected;
            }
            break;
        }

        /* Render de la pelota: solo si cambió de posición */
        int old_bx = prev_ball_x, old_by = prev_ball_y;
        if (ball.x != old_bx || ball.y != old_by) {
            int special = (old_by < 42) ||
                          (old_bx < 322 && old_bx + BALL_SZ > 318);
            if (special) {
                /* Área especial (marcador/midline): dibujar nueva, restaurar píxel a píxel */
                fb_fill_rect(ball.x, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
                for (int r = 0; r < BALL_SZ; r++)
                    for (int d = 0; d < BALL_SZ; d++) {
                        int px = old_bx + d, py = old_by + r;
                        if (px >= ball.x && px < ball.x + BALL_SZ &&
                            py >= ball.y && py < ball.y + BALL_SZ) continue;
                        fb_set_pixel(px, py, bg_pixel(px, py));
                    }
            } else {
                /* Área pura (fondo negro): usa fill_rect — 8x más rápido */
                int ovx = (old_bx + BALL_SZ > ball.x && ball.x + BALL_SZ > old_bx);
                int ovy = (old_by + BALL_SZ > ball.y && ball.y + BALL_SZ > old_by);
                if (!(ovx && ovy)) {
                    /* Sin overlap: borrar vieja entera, dibujar nueva */
                    fb_fill_rect(old_bx, old_by, BALL_SZ, BALL_SZ, COL_BLACK);
                    fb_fill_rect(ball.x,  ball.y,  BALL_SZ, BALL_SZ, COL_WHITE);
                } else {
                    /* Con overlap parcial: dibujar nueva, borrar filas no solapadas */
                    fb_fill_rect(ball.x, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
                    for (int r = 0; r < BALL_SZ; r++) {
                        int py = old_by + r;
                        if (py >= ball.y && py < ball.y + BALL_SZ) {
                            if (ball.x > old_bx)
                                fb_fill_rect(old_bx, py, ball.x - old_bx, 1, COL_BLACK);
                            if (ball.x + BALL_SZ < old_bx + BALL_SZ)
                                fb_fill_rect(ball.x + BALL_SZ, py,
                                             (old_bx + BALL_SZ) - (ball.x + BALL_SZ), 1, COL_BLACK);
                        } else {
                            fb_fill_rect(old_bx, py, BALL_SZ, 1, COL_BLACK);
                        }
                    }
                }
            }
        }
        /* Delta-rect paddles: solo los strips que cambian */
        if (pad[0].y != prev_pad0_y) {
            int dy = pad[0].y - prev_pad0_y;
            if (dy < 0 && -dy < PAD_H) {
                fb_fill_rect(PAD1_X, pad[0].y,         PAD_W, -dy, COL_WHITE);
                fb_fill_rect(PAD1_X, pad[0].y + PAD_H, PAD_W, -dy, COL_BLACK);
            } else if (dy > 0 && dy < PAD_H) {
                fb_fill_rect(PAD1_X, prev_pad0_y,         PAD_W, dy, COL_BLACK);
                fb_fill_rect(PAD1_X, prev_pad0_y + PAD_H, PAD_W, dy, COL_WHITE);
            } else {
                fb_fill_rect(PAD1_X, prev_pad0_y, PAD_W, PAD_H, COL_BLACK);
                fb_fill_rect(PAD1_X, pad[0].y,    PAD_W, PAD_H, COL_WHITE);
            }
            prev_pad0_y = pad[0].y;
        }
        if (pad[1].y != prev_pad1_y) {
            int dy = pad[1].y - prev_pad1_y;
            if (dy < 0 && -dy < PAD_H) {
                fb_fill_rect(PAD2_X, pad[1].y,         PAD_W, -dy, COL_WHITE);
                fb_fill_rect(PAD2_X, pad[1].y + PAD_H, PAD_W, -dy, COL_BLACK);
            } else if (dy > 0 && dy < PAD_H) {
                fb_fill_rect(PAD2_X, prev_pad1_y,         PAD_W, dy, COL_BLACK);
                fb_fill_rect(PAD2_X, prev_pad1_y + PAD_H, PAD_W, dy, COL_WHITE);
            } else {
                fb_fill_rect(PAD2_X, prev_pad1_y, PAD_W, PAD_H, COL_BLACK);
                fb_fill_rect(PAD2_X, pad[1].y,    PAD_W, PAD_H, COL_WHITE);
            }
            prev_pad1_y = pad[1].y;
        }

        prev_ball_x = ball.x; prev_ball_y = ball.y;

        /* Overlay de pausa: solo cuando cambió la selección */
        if (game_state == ST_PAUSE && prev_selected != selected) {
            fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
            fb_draw_str(249, 187, "REANUDAR", 4, COL_BLACK);
            fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
            fb_draw_str(276, 272, "SALIR", 4, COL_BLACK);
            prev_selected = selected;
        }
        break;

    /* ── GAMEOVER ────────────────────────────────────────────────────── */
    case ST_GAMEOVER:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            {
                /* MID (offset 1) = Player 2 Wins → humano gana (selected==1)
                   BOT (offset 2) = Computer Wins → IA gana  (selected==0) */
                int sec = (selected == 1) ? 1 : 2;
                const u8 *spr = DDR2_SPR_GAMEOVER + sec * SPR_GO_SEC_BYTES;
                /* 160×75 ×3 = 480×225, centrado en 640×480 */
                fb_blit_scaled(80, 127, SPR_GO_W, SPR_GO_SEC_H, spr, 0, 3);
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
    /* MIG 7-series calibra solo al arrancar; calib_done sale en LED[15].
     * Sin señal accesible al CPU, se espera el peor caso documentado. */
    usleep(350000);
}

/*
 * Escribe 4 patrones en DDR2 y los lee de vuelta.
 * Muestra en LED[3:0] qué palabras pasaron (bit N = palabra N OK).
 * LED[15] = calib_done (siempre encendido si MIG calibró).
 * Resultado visible 2 segundos antes de que arranque el juego.
 */
static void ddr2_selftest(void)
{
    static const u32 pat[4] = {
        0xDEADBEEFu, 0x12345678u, 0xA5A5A5A5u, 0x0F0F0F0Fu
    };
    u32 base = DDR2_BASE + 0x20000UL;   /* 128 KB dentro de DDR2, lejos del código */
    u32 result = 0;

    for (int i = 0; i < 4; i++)
        Xil_Out32(base + (u32)(i * 4), pat[i]);

    for (int i = 0; i < 4; i++)
        if (Xil_In32(base + (u32)(i * 4)) == pat[i])
            result |= (1u << i);

    /* Mostrar en LED[3:0]: cada bit = palabra i correcta */
    XGpio_DiscreteWrite(&gpio1, 1, result);
    usleep(2000000);   /* 2 s para que el usuario vea los LEDs */
    XGpio_DiscreteWrite(&gpio1, 1, 0);
}

static void ddr2_sprite_defaults(void)
{
    /* Ball y paddle: blanco sólido (nibble COL_WHITE=1 → byte 0x11).
     * Logo: negro/transparente hasta que la SD esté lista. */
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
 * DIAG_STRIPES — test de 8 franjas iguales, cada una de FB_H/8 = 60 filas
 *   Esperado en monitor 640×480:
 *     franja 0 (azul)    : filas   0 –  59
 *     franja 1 (rojo)    : filas  60 – 119
 *     franja 2 (verde)   : filas 120 – 179
 *     franja 3 (amarillo): filas 180 – 239
 *     franja 4 (blanco)  : filas 240 – 299
 *     franja 5 (naranja) : filas 300 – 359
 *     franja 6 (gris)    : filas 360 – 419
 *     franja 7 (magenta) : filas 420 – 479
 *   Cada franja tiene un borde blanco de 2px arriba y 2px abajo como referencia.
 *   Si se ven < 8 franjas, reportar cuántas y cuántas filas ocupa la primera.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void diag_stripes(void)
{
    static const u8 col[8] = {
        COL_BLUE, COL_RED, COL_GREEN, COL_YELLOW,
        COL_WHITE, COL_ORANGE, COL_GRAY, COL_MAGENTA
    };
    int sh = FB_H / 8;                   /* 60 rows per stripe */
    for (int i = 0; i < 8; i++) {
        fb_fill_rect(0, i * sh,     FB_W, sh,     col[i]);
        fb_fill_rect(0, i * sh,     FB_W, 2,      COL_WHITE);  /* top border */
        fb_fill_rect(0, i * sh + sh - 2, FB_W, 2, COL_WHITE);  /* bottom border */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    xil_printf("\r\n=== PONG BOOT ===\r\n");

    /* GPIO0: botones (ch1 in) + SW (ch2 in) */
    XGpio_Initialize(&gpio0, XPAR_AXI_GPIO_0_BASEADDR);
    XGpio_SetDataDirection(&gpio0, 1, 0x1Fu);
    XGpio_SetDataDirection(&gpio0, 2, 0x01u);

    /* GPIO1: LEDs (ch1 out) */
    XGpio_Initialize(&gpio1, XPAR_AXI_GPIO_1_BASEADDR);
    XGpio_SetDataDirection(&gpio1, 1, 0x0000u);

    /* SPI0: inter-FPGA (maestro, polling) */
    XSpi_Config *spi_cfg = XSpi_LookupConfig(XPAR_AXI_QUAD_SPI_0_BASEADDR);
    XSpi_CfgInitialize(&spi, spi_cfg, spi_cfg->BaseAddress);
    XSpi_SetOptions(&spi, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    XSpi_SetSlaveSelect(&spi, 0x01u);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);

    /* DDR2: esperar calibración MIG, verificar R/W y cargar sprites por defecto */
    xil_printf("INFO: DDR2 init...\r\n");
    ddr2_init();
    xil_printf("INFO: DDR2 selftest...\r\n");
    ddr2_selftest();
    xil_printf("INFO: DDR2 sprites...\r\n");
    ddr2_sprite_defaults();

    /* Estado inicial */
    game_state = ST_MENU;
    selected   = 0;
    mode_2p    = 0;
    score[0]   = score[1] = 0;
    init_game();
    sd_ok = sd_run_test();
    load_sprites();

    /* Frame inicial del menú */
    render_frame();

    while (1) {
        u32 btn = btn_edge();

        switch (game_state) {

        /* ── MENÚ PRINCIPAL ─────────────────────────────────────────────── */
        case ST_MENU:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                mode_2p    = (selected == 1) ? 1 : 0;
                score[0]   = score[1] = 0;
                init_game();
                game_state = ST_PLAYING;
            }
            break;

        /* ── JUGANDO ────────────────────────────────────────────────────── */
        case ST_PLAYING:
            if (btn & BTN_C) { selected = 0; game_state = ST_PAUSE; break; }
            {
                /* Nivel (no flanco) para mover la paleta — permite mantener pulsado */
                u32 lvl = XGpio_DiscreteRead(&gpio0, 1) & 0x1Fu;
                if (lvl & BTN_U) move_local_pad(1, 1);
                if (lvl & BTN_D) move_local_pad(1, 0);
            }

            if (mode_2p && sw_on()) spi_exchange();
            else                    move_ai();

            if (++ball_tick >= BALL_TICK_DIV) { ball_tick = 0; move_ball(); }
            update_leds();

            if (check_score()) game_state = ST_GAMEOVER;
            break;

        /* ── PAUSA (0=Reanudar, 1=Salir) ───────────────────────────────── */
        case ST_PAUSE:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                game_state = (selected == 0) ? ST_PLAYING : ST_MENU;
                if (selected != 0) selected = 0;
            }
            break;

        /* ── GAMEOVER ───────────────────────────────────────────────────── */
        case ST_GAMEOVER:
            if (btn & BTN_C) { game_state = ST_MENU; selected = 0; }
            break;
        }

        wait_vsync();
        render_frame();
        usleep(8000);
    }

    return 0;
}
