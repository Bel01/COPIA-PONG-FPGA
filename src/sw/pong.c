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
#define DDR2_SPR_LOGO   ((u8 *)(DDR2_BASE + 0x40110UL))  /* 512 B */

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
#define BALL_TICK_DIV 2    /* pelota avanza 1 de cada N frames; paletas siempre a 70 Hz */
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

/* ── Estados del juego ───────────────────────────────────────────────────── */
#define ST_MENU     0
#define ST_PLAYING  1
#define ST_PAUSE    2
#define ST_GAMEOVER 3

/* ── AXI Quad SPI registros (offset desde base) ──────────────────────────── */
#define SPI_SRR   0x00u   /* Software Reset */
#define SPI_CR    0x60u   /* Control Register */
#define SPI_SR    0x64u   /* Status Register */
#define SPI_DTR   0x68u   /* TX FIFO */
#define SPI_DRR   0x6Cu   /* RX FIFO */
#define SPI_SSR   0x70u   /* Slave Select (activo bajo) */

#define SPICR_INHIBIT  (1u << 8)
#define SPICR_MANSS    (1u << 7)
#define SPICR_MASTER   (1u << 2)
#define SPICR_SPE      (1u << 1)
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
#define SD_LBA_LOGO   5   /* 512 B = 1 sector exacto */

/* ── Estado global ───────────────────────────────────────────────────────── */
static ball_t  ball;
static pad_t   pad[2];
static int     score[2];
static int     game_state;
static int     selected;
static int     mode_2p;
static int     sd_ok      = 0;
static int     sd_sdhc    = 0;
static int     sprites_ok = 0;

/* Dirty-rect state — evita fb_clear() por frame */
static int     ball_tick         = 0;
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
    {0x6, 0x9, 0x4, 0x2, 0xF},  /* 2 */
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

/* ═══════════════════════════════════════════════════════════════════════════
 * SD CARD — driver mínimo SPI manual (modo SPI 0, CPOL=0 CPHA=0)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SD_BASE  XPAR_AXI_QUAD_SPI_1_BASEADDR

static void sd_spi_setup(void)
{
    Xil_Out32(SD_BASE + SPI_SRR, 0x0Au);    /* reset IP */
    /* master, manual SS, transfer inhibited, SPE */
    Xil_Out32(SD_BASE + SPI_CR, SPICR_INHIBIT | SPICR_MANSS | SPICR_MASTER | SPICR_SPE);
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);   /* CS desasertado */
}

static u8 sd_spi_byte(u8 tx)
{
    u32 cr = Xil_In32(SD_BASE + SPI_CR);
    Xil_Out32(SD_BASE + SPI_DTR, tx);
    Xil_Out32(SD_BASE + SPI_CR, cr & ~SPICR_INHIBIT);
    for (int t = 0; t < 100000; t++)
        if (Xil_In32(SD_BASE + SPI_SR) & SPISR_TX_EMPTY) break;
    Xil_Out32(SD_BASE + SPI_CR, cr);
    return (u8)Xil_In32(SD_BASE + SPI_DRR);
}

/*
 * Inicializa la SD en modo SPI.
 * Retorna 0 si OK, -1 si falla (el juego continúa igualmente).
 */
static int sd_init(void)
{
    sd_spi_setup();

    /* 80 pulsos de clock con CS desasertado */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);
    for (int i = 0; i < 10; i++) sd_spi_byte(0xFF);

    /* Aserta CS[0] */
    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);

    /* CMD0: GO_IDLE_STATE — espera R1=0x01 */
    const u8 cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd0[i]);
    u8 r1 = 0xFF;
    for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
    if (r1 != 0x01) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    /* CMD8: SEND_IF_COND (SDHC check) */
    const u8 cmd8[] = {0x48, 0x00, 0x00, 0x01, 0xAA, 0x87};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd8[i]);
    for (int i = 0; i < 5; i++) sd_spi_byte(0xFF);   /* consume R7 */

    /* ACMD41: espera hasta salir de idle (máx 200 intentos) */
    int tries = 0;
    do {
        const u8 cmd55[] = {0x77, 0x00, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd55[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);

        const u8 cmd41[] = {0x69, 0x40, 0x00, 0x00, 0x00, 0x01};
        for (int i = 0; i < 6; i++) sd_spi_byte(cmd41[i]);
        r1 = 0xFF;
        for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
        tries++;
    } while (r1 != 0x00 && tries < 200);

    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    /* CMD58: READ_OCR — detectar SDHC (bit CCS en OCR[30]) */
    const u8 cmd58[] = {0x7A, 0x00, 0x00, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd58[i]);
    u8 r58 = 0xFF;
    for (int i = 0; i < 10 && r58 == 0xFF; i++) r58 = sd_spi_byte(0xFF);
    u8 ocr0 = sd_spi_byte(0xFF);
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

    Xil_Out32(SD_BASE + SPI_SSR, 0xFEu);   /* CS assert */

    u8 cmd[6] = { 0x51,
        (u8)(addr >> 24), (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, 0x01 };
    for (int i = 0; i < 6; i++) sd_spi_byte(cmd[i]);

    u8 r1 = 0xFF;
    for (int i = 0; i < 10 && r1 == 0xFF; i++) r1 = sd_spi_byte(0xFF);
    if (r1 != 0x00) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    u8 tok = 0xFF;
    for (int i = 0; i < 500 && tok != 0xFE; i++) tok = sd_spi_byte(0xFF);
    if (tok != 0xFE) { Xil_Out32(SD_BASE + SPI_SSR, 0xFFu); return -1; }

    for (int i = 0; i < 512; i++) buf[i] = sd_spi_byte(0xFF);
    sd_spi_byte(0xFF); sd_spi_byte(0xFF);  /* CRC */

    Xil_Out32(SD_BASE + SPI_SSR, 0xFFu);   /* CS deassert */
    return 0;
}

/* Carga los sprites desde los sectores reservados de la SD. */
static void load_sprites(void)
{
    if (!sd_ok) return;

    /* Verificar magic "PONG" en sector cabecera */
    if (sd_read_block(SD_LBA_HDR, sd_sector_buf) != 0) return;
    u32 magic = ((u32)sd_sector_buf[0] << 24) | ((u32)sd_sector_buf[1] << 16) |
                ((u32)sd_sector_buf[2] <<  8) |  (u32)sd_sector_buf[3];
    if (magic != SD_MAGIC) return;

    u8 *dst;

    if (sd_read_block(SD_LBA_BALL, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_BALL;
    for (int i = 0; i < SPR_BALL_W * SPR_BALL_H / 2; i++) dst[i] = sd_sector_buf[i];

    if (sd_read_block(SD_LBA_PADDLE, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_PADDLE;
    for (int i = 0; i < SPR_PAD_W * SPR_PAD_H / 2; i++) dst[i] = sd_sector_buf[i];

    if (sd_read_block(SD_LBA_LOGO, sd_sector_buf) != 0) return;
    dst = DDR2_SPR_LOGO;
    for (int i = 0; i < SPR_LOGO_W * SPR_LOGO_H / 2; i++) dst[i] = sd_sector_buf[i];

    sprites_ok = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * LÓGICA DEL JUEGO
 * ═══════════════════════════════════════════════════════════════════════════ */

static void init_game(void)
{
    ball.x  = FB_W / 2;
    ball.y  = FB_H / 2;
    ball.dx = 4;
    ball.dy = 3;
    pad[0].y = (FB_H - PAD_H) / 2;
    pad[1].y = (FB_H - PAD_H) / 2;
    ball_tick = 0;
    needs_full_redraw = 1;   /* sincroniza prev_pad*_y tras salto de posición */
}

static int collide(ball_t *b, int px, int py)
{
    if (b->x + BALL_SZ <= px || b->x >= px + PAD_W) return 0;
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
    int center = pad[0].y + PAD_H / 2;
    int spd    = (ball.dy < 0 ? -ball.dy : ball.dy);
    if (spd < 2) spd = 2;

    if (ball.dx < 0) {
        if (ball.y + BALL_SZ / 2 < center) pad[0].y -= spd;
        else                               pad[0].y += spd;
    } else {
        int mid = (FB_H - PAD_H) / 2;
        if      (pad[0].y < mid) pad[0].y += spd;
        else if (pad[0].y > mid) pad[0].y -= spd;
    }
    if (pad[0].y < 0)           pad[0].y = 0;
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
        ball.dx = (ball.dx < 0) ? -(ball.dx - 1) : -(ball.dx + 1);
        ball.dy = (hit < 8) ? 4 : (hit < 16) ? 3 : (hit < 24) ? 2 :
                  (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                  (hit < 46) ? -2 : (hit < 54) ? -3 : -4;
        if (ball.x < PAD1_X + PAD_W + 2) ball.x = PAD1_X + PAD_W + 2;
    }

    /* Colisión paleta derecha */
    if (collide(&ball, PAD2_X, pad[1].y)) {
        int hit = (pad[1].y + PAD_H) - ball.y;
        ball.dx = (ball.dx > 0) ? -(ball.dx + 1) : -(ball.dx - 1);
        ball.dy = (hit < 8) ? 4 : (hit < 16) ? 3 : (hit < 24) ? 2 :
                  (hit < 30) ? 1 : (hit < 32) ? 0 : (hit < 38) ? -1 :
                  (hit < 46) ? -2 : (hit < 54) ? -3 : -4;
        if (ball.x > PAD2_X - BALL_SZ - 2) ball.x = PAD2_X - BALL_SZ - 2;
    }

    if (ball.dx >  8) ball.dx =  8;
    if (ball.dx < -8) ball.dx = -8;
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
            fb_clear(COL_BLUE);
            if (sprites_ok)
                fb_blit(288, 60, SPR_LOGO_W, SPR_LOGO_H, DDR2_SPR_LOGO, 1);
            needs_full_redraw = 0;
            prev_selected = -1;
        }
        if (prev_selected != selected) {
            fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
            fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
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
                fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
                prev_selected = selected;
            }
            break;
        }

        /* Dirty-rect: borrar posiciones anteriores */
        fb_fill_rect(prev_ball_x, prev_ball_y, BALL_SZ, BALL_SZ, COL_BLACK);
        if (prev_ball_x + BALL_SZ > 318 && prev_ball_x < 322) {
            for (int ny = (prev_ball_y / 8) * 8; ny < prev_ball_y + BALL_SZ; ny += 8)
                if (ny < FB_H) fb_fill_rect(318, ny, 4, 4, COL_GRAY);
        }
        if (prev_ball_y < 48) score_dirty = 1;  /* restaurar marcador si pelota pasó por ahí */
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

        /* Pelota en nueva posición */
        fb_fill_rect(ball.x, ball.y, BALL_SZ, BALL_SZ, COL_WHITE);
        prev_ball_x = ball.x; prev_ball_y = ball.y;

        /* Marcador: solo cuando cambió */
        if (score_dirty) {
            fb_fill_rect(220, 0, 200, 48, COL_BLACK);
            fb_draw_scores();
            score_dirty = 0;
        }

        /* Overlay de pausa: solo cuando cambió la selección */
        if (game_state == ST_PAUSE && prev_selected != selected) {
            fb_fill_rect(220, 175, 200, 45, (selected == 0) ? COL_YELLOW : COL_DGRAY);
            fb_fill_rect(220, 260, 200, 45, (selected == 1) ? COL_YELLOW : COL_DGRAY);
            prev_selected = selected;
        }
        break;

    /* ── GAMEOVER ────────────────────────────────────────────────────── */
    case ST_GAMEOVER:
        if (needs_full_redraw) {
            fb_clear(COL_BLACK);
            fb_fill_rect(120, 200, 400, 80, (selected == 0) ? COL_GREEN : COL_RED);
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
    ddr2_init();
    ddr2_selftest();
    ddr2_sprite_defaults();

    /* Estado inicial */
    game_state = ST_MENU;
    selected   = 0;
    mode_2p    = 0;
    score[0]   = score[1] = 0;
    init_game();
    /* SD desactivado temporalmente — AXI fault en SPI1, depurar después */
    sd_ok = 0;

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

        render_frame();
        usleep(8000);   /* ~8 ms/frame → ~70 Hz con overhead de render */
    }

    return 0;
}
