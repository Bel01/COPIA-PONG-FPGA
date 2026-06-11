/*
 * pong.c — firmware bare-metal MicroBlaze V para Pong en Nexys A7-100T
 *
 * Periféricos (resueltos desde xparameters.h al generar el BSP en Vitis):
 *   XPAR_AXI_GPIO_0_DEVICE_ID        GPIO0 ch1: botones {BTNR,BTNL,BTNC,BTND,BTNU} (niveles)
 *                                    GPIO0 ch2: SW[0] (habilita SPI)
 *   XPAR_AXI_GPIO_1_DEVICE_ID        GPIO1 ch1: LED[14:0] (salida)
 *   XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR  VRAM compartida con VGA
 *   XPAR_AXI_QUAD_SPI_0_DEVICE_ID    SPI modo 2P
 *
 * Mapa BRAM (byte-addressed, palabras de 32 bits):
 *   0x00: {6'b0, ball_y[9:0], 6'b0, ball_x[9:0]}
 *   0x04: {6'b0, pad2_y[9:0], 6'b0, pad1_y[9:0]}
 *   0x08: {16'b0, score2[7:0], score1[7:0]}
 *   0x0C: {28'b0, selected[1:0], game_state[1:0]}
 */

#include <stdio.h>
#include "xparameters.h"
#include "xgpio.h"
#include "xspi.h"
#include "xil_io.h"
#include "sleep.h"

/* ── Geometría de pantalla ───────────────────────────────────────────────── */
#define SCR_W        640
#define SCR_H        480

/* ── Geometría del juego (debe coincidir con localparam en top_pong_project.v) */
#define BALL_SZ       8
#define PAD_W         8
#define PAD_H        60
#define PAD1_X       20
#define PAD2_X      612
#define PAD_SPEED     4

/* ── Reglas ──────────────────────────────────────────────────────────────── */
#define SCORE_WIN    10

/* ── BRAM offsets ────────────────────────────────────────────────────────── */
#define BRAM_BALL   0x00
#define BRAM_PAD    0x04
#define BRAM_SCORE  0x08
#define BRAM_STATE  0x0C

/* ── Máscaras de botones (GPIO0 canal 1: {BTNR,BTNL,BTNC,BTND,BTNU}) ────── */
#define BTN_U  0x01u
#define BTN_D  0x02u
#define BTN_C  0x04u
#define BTN_L  0x08u
#define BTN_R  0x10u

/* ── Estados del juego ───────────────────────────────────────────────────── */
#define ST_MENU      0
#define ST_PLAYING   1
#define ST_PAUSE     2
#define ST_GAMEOVER  3

/* ── Tipos ───────────────────────────────────────────────────────────────── */
typedef struct { int x, y, dx, dy; } ball_t;
typedef struct { int y;             } pad_t;

/* ── Estado global ───────────────────────────────────────────────────────── */
static ball_t  ball;
static pad_t   pad[2];          /* pad[0]=izquierda(AI/remoto), pad[1]=derecha(local) */
static int     score[2];
static int     game_state;
static int     selected;        /* cursor en menú/pausa; índice del ganador en gameover */
static int     mode_2p;         /* 0=1P (IA), 1=2P (SPI) */

static XGpio   gpio0;           /* botones + SW */
static XGpio   gpio1;           /* LEDs */
static XSpi    spi;

/* ── Helpers BRAM ────────────────────────────────────────────────────────── */
static void bram_sync(void)
{
    u32 base = XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR;
    Xil_Out32(base + BRAM_BALL,
              ((u32)(ball.y  & 0x3FF) << 16) | (u32)(ball.x  & 0x3FF));
    Xil_Out32(base + BRAM_PAD,
              ((u32)(pad[1].y & 0x3FF) << 16) | (u32)(pad[0].y & 0x3FF));
    Xil_Out32(base + BRAM_SCORE,
              ((u32)(score[1] & 0xFF) << 8)  | (u32)(score[0] & 0xFF));
    Xil_Out32(base + BRAM_STATE,
              ((u32)(selected    & 0x3) << 2) | (u32)(game_state & 0x3));
}

/* ── Lectura de botones con detección de flanco ──────────────────────────── */
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

/* ── Inicialización del juego ────────────────────────────────────────────── */
static void init_game(void)
{
    ball.x  = SCR_W / 2;
    ball.y  = SCR_H / 2;
    ball.dx = 2;
    ball.dy = 2;

    pad[0].y = (SCR_H - PAD_H) / 2;
    pad[1].y = (SCR_H - PAD_H) / 2;
}

/* ── Colisión AABB ───────────────────────────────────────────────────────── */
static int collide(ball_t *b, int px, int py)
{
    if (b->x + BALL_SZ <= px)           return 0;
    if (b->x            >= px + PAD_W)  return 0;
    if (b->y + BALL_SZ <= py)           return 0;
    if (b->y            >= py + PAD_H)  return 0;
    return 1;
}

/* ── Verificación de puntuación; retorna 1=gana p1, 2=gana p2, 0=sigue ──── */
static int check_score(void)
{
    for (int i = 0; i < 2; i++) {
        if (score[i] >= SCORE_WIN) {
            selected = i;           /* índice del ganador para VGA */
            score[0] = score[1] = 0;
            return i + 1;
        }
    }
    return 0;
}

/* ── Paleta local (arriba=1, abajo=0) ────────────────────────────────────── */
static void move_local_pad(int p, int dir)
{
    pad[p].y += (dir) ? -PAD_SPEED : PAD_SPEED;
    if (pad[p].y < 0)              pad[p].y = 0;
    if (pad[p].y > SCR_H - PAD_H) pad[p].y = SCR_H - PAD_H;
}

/* ── IA (paleta izquierda sigue la pelota) ───────────────────────────────── */
static void move_ai(void)
{
    int center = pad[0].y + PAD_H / 2;
    int spd    = (ball.dy < 0) ? -ball.dy : ball.dy;
    if (spd < 2) spd = 2;

    if (ball.dx < 0) {                          /* pelota viene hacia la IA */
        if (ball.y + BALL_SZ / 2 < center) pad[0].y -= spd;
        else                               pad[0].y += spd;
    } else {                                    /* vuelve al centro */
        int mid = (SCR_H - PAD_H) / 2;
        if      (pad[0].y < mid) pad[0].y += spd;
        else if (pad[0].y > mid) pad[0].y -= spd;
    }
    if (pad[0].y < 0)              pad[0].y = 0;
    if (pad[0].y > SCR_H - PAD_H) pad[0].y = SCR_H - PAD_H;
}

/* ── Movimiento de la pelota ─────────────────────────────────────────────── */
static void move_ball(void)
{
    ball.x += ball.dx;
    ball.y += ball.dy;

    /* Rebote vertical */
    if (ball.y < 0) {
        ball.y  = 0;
        ball.dy = -ball.dy;
    }
    if (ball.y > SCR_H - BALL_SZ) {
        ball.y  = SCR_H - BALL_SZ;
        ball.dy = -ball.dy;
    }

    /* Punto */
    if (ball.x < 0) {
        score[1]++;
        init_game();
        return;
    }
    if (ball.x > SCR_W - BALL_SZ) {
        score[0]++;
        init_game();
        return;
    }

    /* Colisión con paleta 0 (izquierda) */
    if (collide(&ball, PAD1_X, pad[0].y)) {
        int hit = (pad[0].y + PAD_H) - ball.y;
        ball.dx = (ball.dx < 0) ? -(ball.dx - 1) : -(ball.dx + 1);
        if      (hit <  8) ball.dy =  4;
        else if (hit < 16) ball.dy =  3;
        else if (hit < 24) ball.dy =  2;
        else if (hit < 30) ball.dy =  1;
        else if (hit < 32) ball.dy =  0;
        else if (hit < 38) ball.dy = -1;
        else if (hit < 46) ball.dy = -2;
        else if (hit < 54) ball.dy = -3;
        else               ball.dy = -4;
        if (ball.x < PAD1_X + PAD_W + 2) ball.x = PAD1_X + PAD_W + 2;
    }

    /* Colisión con paleta 1 (derecha) */
    if (collide(&ball, PAD2_X, pad[1].y)) {
        int hit = (pad[1].y + PAD_H) - ball.y;
        ball.dx = (ball.dx > 0) ? -(ball.dx + 1) : -(ball.dx - 1);
        if      (hit <  8) ball.dy =  4;
        else if (hit < 16) ball.dy =  3;
        else if (hit < 24) ball.dy =  2;
        else if (hit < 30) ball.dy =  1;
        else if (hit < 32) ball.dy =  0;
        else if (hit < 38) ball.dy = -1;
        else if (hit < 46) ball.dy = -2;
        else if (hit < 54) ball.dy = -3;
        else               ball.dy = -4;
        if (ball.x > PAD2_X - BALL_SZ - 2) ball.x = PAD2_X - BALL_SZ - 2;
    }

    /* Clamp velocidad para evitar aceleración infinita */
    if (ball.dx >  8) ball.dx =  8;
    if (ball.dx < -8) ball.dx = -8;
}

/* ── Intercambio SPI 2P ──────────────────────────────────────────────────── */
/*
 * Protocolo: 2 bytes por frame.
 *   TX: posición Y de la paleta local (esta FPGA = paleta derecha, pad[1].y)
 *   RX: posición Y de la paleta remota (otra FPGA = paleta izquierda, pad[0].y)
 *
 * La otra FPGA debe configurarse como esclavo SPI con el protocolo inverso.
 * Ajustar CPOL/CPHA según lo que acuerden con el otro grupo.
 */
static void spi_exchange(void)
{
    u8 tx[2], rx[2];
    int local_y = pad[1].y;
    tx[0] = (u8)((local_y >> 8) & 0xFF);
    tx[1] = (u8)( local_y       & 0xFF);
    if (XSpi_Transfer(&spi, tx, rx, 2) == XST_SUCCESS) {
        int remote_y = ((int)rx[0] << 8) | rx[1];
        if (remote_y >= 0 && remote_y <= SCR_H - PAD_H)
            pad[0].y = remote_y;
    }
}

/* ── LEDs: muestra marcador en LED[7:4]=score[1], LED[3:0]=score[0] ─────── */
static void update_leds(void)
{
    u32 val = ((u32)(score[1] & 0xF) << 4) | (u32)(score[0] & 0xF);
    XGpio_DiscreteWrite(&gpio1, 1, val);
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    /* GPIO0: botones (ch1 input) + SW (ch2 input) */
    XGpio_Initialize(&gpio0, XPAR_AXI_GPIO_0_DEVICE_ID);
    XGpio_SetDataDirection(&gpio0, 1, 0x1Fu);
    XGpio_SetDataDirection(&gpio0, 2, 0x01u);

    /* GPIO1: LEDs (ch1 output) */
    XGpio_Initialize(&gpio1, XPAR_AXI_GPIO_1_DEVICE_ID);
    XGpio_SetDataDirection(&gpio1, 1, 0x0000u);

    /* SPI: maestro, polling, sin interrupciones */
    XSpi_Config *spi_cfg = XSpi_LookupConfig(XPAR_AXI_QUAD_SPI_0_DEVICE_ID);
    XSpi_CfgInitialize(&spi, spi_cfg, spi_cfg->BaseAddress);
    XSpi_SetOptions(&spi, XSP_MASTER_OPTION | XSP_MANUAL_SSELECT_OPTION);
    XSpi_SetSlaveSelect(&spi, 0x01u);
    XSpi_Start(&spi);
    XSpi_IntrGlobalDisable(&spi);

    /* Estado inicial */
    game_state = ST_MENU;
    selected   = 0;
    mode_2p    = 0;
    score[0]   = score[1] = 0;
    init_game();
    bram_sync();

    while (1) {
        u32 btn = btn_edge();

        switch (game_state) {

        /* ── MENÚ PRINCIPAL (0=1P, 1=2P) ────────────────────────────────── */
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

        /* ── JUGANDO ─────────────────────────────────────────────────────── */
        case ST_PLAYING:
            /* Pausa */
            if (btn & BTN_C) {
                selected   = 0;
                game_state = ST_PAUSE;
                break;
            }

            /* Paleta derecha (jugador local) */
            if (btn & BTN_U) move_local_pad(1, 1);
            if (btn & BTN_D) move_local_pad(1, 0);

            /* Paleta izquierda: IA o SPI */
            if (mode_2p && sw_on())
                spi_exchange();
            else
                move_ai();

            move_ball();
            update_leds();

            if (check_score())
                game_state = ST_GAMEOVER;
            break;

        /* ── PAUSA (0=Reanudar, 1=Salir) ────────────────────────────────── */
        case ST_PAUSE:
            if (btn & BTN_U) selected = 0;
            if (btn & BTN_D) selected = 1;
            if (btn & BTN_C) {
                if (selected == 0) {
                    game_state = ST_PLAYING;
                } else {
                    game_state = ST_MENU;
                    selected   = 0;
                }
            }
            break;

        /* ── GAMEOVER ────────────────────────────────────────────────────── */
        case ST_GAMEOVER:
            if (btn & BTN_C) {
                game_state = ST_MENU;
                selected   = 0;
            }
            break;
        }

        bram_sync();
        usleep(16667);  /* ~60 Hz */
    }

    return 0;
}
