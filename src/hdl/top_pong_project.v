/**
 * @title Top-level — Sistema Embebido Pong Multijugador (Framebuffer)
 * @file top_pong_project.v
 * @brief Integra el SoC MicroBlaze V con el subsistema VGA basado en framebuffer BRAM.
 * @details
 *   Arquitectura:
 *     SD card ──SPI──► MicroBlaze ──AXI──► BRAM framebuffer ──Port B──► VGA ──► monitor
 *                           └──AXI──► DDR2 (sprites + variables)
 *
 *   Framebuffer: 640×480 × 4-bit = 38 400 palabras de 32 bits en BRAM True Dual Port.
 *   MicroBlaze escribe por Port A (32-bit AXI). El VGA lee por Port B en cada pixel.
 *   Empaquetado: word[31:28]=píxel 0 ... word[3:0]=píxel 7 (big-endian).
 *
 *   Pipeline VGA (1 ciclo a 100 MHz):
 *     Ciclo 0: addr = f(h_count, v_count) combinacional → BRAM inicia lectura
 *     Ciclo 1: dout disponible → se registra junto con nibble_sel, blank, hsync, vsync
 *     Ciclo 1+: paleta aplica y pixel_mux saca RGB
 *
 * @author JustinAlfaro
 * @date 2026-06-11
 */

`timescale 1ns / 1ps

module top_pong_project (
    // Clock & Reset
    input  wire        CLK100MHZ,
    input  wire        CPU_RESETN,

    // Botones (jugador + sistema)
    input  wire        BTNU,
    input  wire        BTND,
    input  wire        BTNC,
    input  wire        BTNL,
    input  wire        BTNR,

    // Switch modo de juego
    input  wire        SW,              ///< SW[0]: 1 = habilita SPI modo 2P

    // LEDs
    output wire [15:0] LED,             ///< [15] = DDR2 calib done

    // VGA 640×480 @ 60 Hz
    output wire [3:0]  VGA_R,
    output wire [3:0]  VGA_G,
    output wire [3:0]  VGA_B,
    output wire        VGA_HS,
    output wire        VGA_VS,

    // UART (USB integrado Nexys A7)
    input  wire        UART_RXD_OUT,
    output wire        UART_TXD_IN,

    // SPI inter-FPGA (PMOD JA — pines por confirmar con el otro grupo)
    inout  wire        SPI_SCK,
    inout  wire        SPI_MOSI,
    inout  wire        SPI_MISO,
    inout  wire [0:0]  SPI_CS_N,

    // SPI microSD (conector onboard Nexys A7: B1/C1/C2/D2/E2)
    inout  wire        SD_SCK,
    inout  wire        SD_MOSI,
    inout  wire        SD_MISO,
    inout  wire [0:0]  SD_CS_N,
    output wire        SD_RESET,   // debe mantenerse en 0; en 1 la SD queda en reset por el config controller

    // DDR2 SDRAM (MT47H64M16HR-25E, 128 MB, 16-bit)
    inout  wire [15:0] ddr2_dq,
    inout  wire [1:0]  ddr2_dqs_p,
    inout  wire [1:0]  ddr2_dqs_n,
    output wire [12:0] ddr2_addr,
    output wire [2:0]  ddr2_ba,
    output wire        ddr2_ras_n,
    output wire        ddr2_cas_n,
    output wire        ddr2_we_n,
    output wire [0:0]  ddr2_ck_p,
    output wire [0:0]  ddr2_ck_n,
    output wire [0:0]  ddr2_cke,
    output wire [0:0]  ddr2_cs_n,
    output wire [1:0]  ddr2_dm,
    output wire [0:0]  ddr2_odt
);

// LED[15] = DDR2 calib done; LED[14:0] = firmware via GPIO2 (16-bit wrapper)
wire ddr2_calib_done;
wire [15:0] led_gpio;
assign LED[15]  = ddr2_calib_done;
assign LED[14:0] = led_gpio[14:0];

// SPI inter-FPGA: SCK en Z (sck_io del wrapper pendiente de verificar en modo 2P)
assign SPI_SCK  = 1'bz;
// SD_RESET debe estar en 0: el config controller libera la SD tras arrancar y esta señal la saca de reset
assign SD_RESET = 1'b0;

// -------------------------------------------------------------------------
// Debounce + sincronización de entradas
// -------------------------------------------------------------------------
wire btn_u_db, btn_d_db, btn_c_db, btn_l_db, btn_r_db;
wire btn_u_lvl, btn_d_lvl, btn_c_lvl, btn_l_lvl, btn_r_lvl;
wire sw_sync;

debounce u_db_btnu (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNU), .btn_out(btn_u_db), .btn_level(btn_u_lvl));
debounce u_db_btnd (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTND), .btn_out(btn_d_db), .btn_level(btn_d_lvl));
debounce u_db_btnc (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNC), .btn_out(btn_c_db), .btn_level(btn_c_lvl));
debounce u_db_btnl (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNL), .btn_out(btn_l_db), .btn_level(btn_l_lvl));
debounce u_db_btnr (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNR), .btn_out(btn_r_db), .btn_level(btn_r_lvl));

sync_signal #(.WIDTH(1)) u_sync_sw (
    .clk      (CLK100MHZ),
    .rst      (~CPU_RESETN),
    .async_in (SW),
    .sync_out (sw_sync)
);

// -------------------------------------------------------------------------
// VRAM Port B — señales hacia el SoC
// -------------------------------------------------------------------------
wire [31:0] vram_portb_addr;
wire        vram_portb_clk;
wire [31:0] vram_portb_din;
wire [31:0] vram_portb_dout;
wire        vram_portb_en;
wire        vram_portb_rst;
wire [3:0]  vram_portb_we;

// -------------------------------------------------------------------------
// SoC MicroBlaze V
// -------------------------------------------------------------------------
microblaze_v_wrapper u_soc (
    // Reloj y reset
    .clk_100MHz             (CLK100MHZ),
    .reset_rtl_0            (CPU_RESETN),

    // DDR2 calib done
    .init_calib_complete_0  (ddr2_calib_done),

    // GPIO0 — [5]=vsync  [4:0]={BTNR,BTNL,BTNC,BTND,BTNU}
    .gpio_rtl_0_tri_i       ({26'b0, vga_vs_int, btn_r_lvl, btn_l_lvl, btn_c_lvl, btn_d_lvl, btn_u_lvl}),

    // GPIO1 — SW[0] modo 2P
    .gpio_rtl_1_tri_i       (sw_sync),

    // GPIO2 — LEDs [15:0] (wrapper 16-bit; LED[15] override por calib_done arriba)
    .gpio_rtl_2_tri_o       (led_gpio),

    // UART
    .uart_rtl_0_rxd         (UART_RXD_OUT),
    .uart_rtl_0_txd         (UART_TXD_IN),

    // SPI inter-FPGA (AXI Quad SPI 0) — sck_io no existe en wrapper (Vivado omite SCK)
    .spi_rtl_0_io0_io       (SPI_MOSI),
    .spi_rtl_0_io1_io       (SPI_MISO),
    .spi_rtl_0_ss_io        (SPI_CS_N),

    // SPI microSD (AXI Quad SPI 1)
    .spi_sd_0_sck_io        (SD_SCK),
    .spi_sd_0_io0_io        (SD_MOSI),
    .spi_sd_0_io1_io        (SD_MISO),
    .spi_sd_0_ss_io         (SD_CS_N),

    // DDR2
    .DDR2_0_dq              (ddr2_dq),
    .DDR2_0_dqs_p           (ddr2_dqs_p),
    .DDR2_0_dqs_n           (ddr2_dqs_n),
    .DDR2_0_addr            (ddr2_addr),
    .DDR2_0_ba              (ddr2_ba),
    .DDR2_0_ras_n           (ddr2_ras_n),
    .DDR2_0_cas_n           (ddr2_cas_n),
    .DDR2_0_we_n            (ddr2_we_n),
    .DDR2_0_ck_p            (ddr2_ck_p),
    .DDR2_0_ck_n            (ddr2_ck_n),
    .DDR2_0_cke             (ddr2_cke),
    .DDR2_0_cs_n            (ddr2_cs_n),
    .DDR2_0_dm              (ddr2_dm),
    .DDR2_0_odt             (ddr2_odt),

    // VRAM Port B (lectura del framebuffer por el VGA)
    .BRAM_PORTB_0_addr      (vram_portb_addr),
    .BRAM_PORTB_0_clk       (vram_portb_clk),
    .BRAM_PORTB_0_din       (vram_portb_din),
    .BRAM_PORTB_0_dout      (vram_portb_dout),
    .BRAM_PORTB_0_en        (vram_portb_en),
    .BRAM_PORTB_0_rst       (vram_portb_rst),
    .BRAM_PORTB_0_we        (vram_portb_we)
);

// -------------------------------------------------------------------------
// Pixel clock 25 MHz (clock enable 1 pulso cada 4 ciclos a 100 MHz)
// -------------------------------------------------------------------------
wire tick_25mhz;

div_freq #(.DIVISOR(4)) u_div_25mhz (
    .clk_in  (CLK100MHZ),
    .rst     (~CPU_RESETN),
    .clk_out (tick_25mhz)
);

// -------------------------------------------------------------------------
// Controlador VGA 640×480 @ 60 Hz
// -------------------------------------------------------------------------
wire       vga_blank;
wire [9:0] h_count, v_count;
wire       vga_hs_int, vga_vs_int;

vga_controller u_vga_ctrl (
    .clk        (CLK100MHZ),
    .rst        (~CPU_RESETN),
    .tick_25mhz (tick_25mhz),
    .hsync      (vga_hs_int),
    .vsync      (vga_vs_int),
    .blank      (vga_blank),
    .h_count    (h_count),
    .v_count    (v_count)
);

// -------------------------------------------------------------------------
// Lectura del framebuffer BRAM
//
// Cada palabra de 32 bits almacena 8 píxeles de 4 bits (big-endian):
//   word[31:28] = píxel 0 (x%8==0) ... word[3:0] = píxel 7 (x%8==7)
//
// pixel_idx  = v_count × 640 + h_count   (0 … 307199)
// word_addr  = pixel_idx >> 3            (0 … 38399)
// nibble_sel = pixel_idx[2:0]            (0 … 7)
// -------------------------------------------------------------------------
wire [18:0] pixel_idx  = ({9'b0, v_count} * 19'd640) + {9'b0, h_count};
wire [15:0] word_addr  = pixel_idx[18:3];
wire [2:0]  nibble_sel = pixel_idx[2:0];

assign vram_portb_addr = {14'b0, word_addr, 2'b00}; /* byte-addressed: word × 4 */
assign vram_portb_clk  = CLK100MHZ;
assign vram_portb_din  = 32'h0;
assign vram_portb_en   = 1'b1;
assign vram_portb_rst  = ~CPU_RESETN;
assign vram_portb_we   = 4'b0;

// Pipeline 1 ciclo — compensa la latencia de lectura de 1 ciclo de la BRAM.
// Se registra también hsync/vsync para mantener alineación temporal con el color.
reg [31:0] bram_dout_r;
reg [2:0]  nibble_r;
reg        blank_r;
reg        hs_r, vs_r;

always @(posedge CLK100MHZ) begin
    bram_dout_r <= vram_portb_dout;
    nibble_r    <= nibble_sel;
    blank_r     <= vga_blank;
    hs_r        <= vga_hs_int;
    vs_r        <= vga_vs_int;
end

// Extracción del nibble (big-endian: nibble 0 en bits [31:28])
reg [3:0] pixel_nibble;
always @(*) begin
    case (nibble_r)
        3'd0: pixel_nibble = bram_dout_r[31:28];
        3'd1: pixel_nibble = bram_dout_r[27:24];
        3'd2: pixel_nibble = bram_dout_r[23:20];
        3'd3: pixel_nibble = bram_dout_r[19:16];
        3'd4: pixel_nibble = bram_dout_r[15:12];
        3'd5: pixel_nibble = bram_dout_r[11:8];
        3'd6: pixel_nibble = bram_dout_r[7:4];
        3'd7: pixel_nibble = bram_dout_r[3:0];
        default: pixel_nibble = 4'h0;
    endcase
end

// Paleta 16 colores: índice 4-bit → RGB 12-bit {R[3:0], G[3:0], B[3:0]}
reg [11:0] pixel_rgb;
always @(*) begin
    case (pixel_nibble)
        4'h0: pixel_rgb = 12'h000;  // Negro    — fondo
        4'h1: pixel_rgb = 12'hFFF;  // Blanco   — pelota, paletas
        4'h2: pixel_rgb = 12'hF00;  // Rojo     — score J2 / derrota
        4'h3: pixel_rgb = 12'h00F;  // Azul     — score J1
        4'h4: pixel_rgb = 12'hFF0;  // Amarillo — cursor menú
        4'h5: pixel_rgb = 12'h0F0;  // Verde    — victoria
        4'h6: pixel_rgb = 12'hF80;  // Naranja
        4'h7: pixel_rgb = 12'h888;  // Gris medio — net
        4'h8: pixel_rgb = 12'h444;  // Gris oscuro — pausa overlay
        4'h9: pixel_rgb = 12'hF0F;  // Magenta
        4'hA: pixel_rgb = 12'h0FF;  // Cyan
        4'hB: pixel_rgb = 12'hFFA;  // Amarillo claro
        4'hC: pixel_rgb = 12'hA00;  // Rojo oscuro
        4'hD: pixel_rgb = 12'h0A0;  // Verde oscuro
        4'hE: pixel_rgb = 12'h00A;  // Azul oscuro
        4'hF: pixel_rgb = 12'hAAA;  // Gris claro
        default: pixel_rgb = 12'h000;
    endcase
end

// -------------------------------------------------------------------------
// Salida VGA — pixel_mux apaga RGB durante blanking
// -------------------------------------------------------------------------
pixel_mux u_pixel_mux (
    .blank      (blank_r),
    .pixel_data (pixel_rgb),
    .vga_r      (VGA_R),
    .vga_g      (VGA_G),
    .vga_b      (VGA_B)
);

assign VGA_HS = hs_r;
assign VGA_VS = vs_r;

endmodule
