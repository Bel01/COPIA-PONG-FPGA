/**
 * @title Top-level — Sistema Embebido Pong Multijugador
 * @file top_pong_project.v
 * @brief Integra el SoC MicroBlaze V con el subsistema VGA de renderizado procedural.
 * @details
 *   Arquitectura de dos capas:
 *
 *   1. SoC (Block Design microblaze_v_wrapper):
 *      MicroBlaze V (RISC-V bare-metal) + MIG DDR2 128 MB + AXI GPIO/UART/SPI +
 *      BRAM dual-port 32 KB. El firmware actualiza el estado del juego escribiendo
 *      3 palabras de 32 bits en la BRAM via AXI (Port A) cada tick de juego.
 *
 *   2. Subsistema VGA 640x480 @ 60 Hz (renderizado procedural):
 *      Durante el vblank (v_count >= 480) una FSM lee las 3 palabras de estado
 *      desde BRAM Port B y las almacena en registros. Durante el frame activo,
 *      la geometría de cada pixel se calcula combinacionalmente comparando
 *      h_count/v_count contra las posiciones de pelota, paletas y red.
 *      No hay framebuffer: el MicroBlaze solo escribe ~3 palabras por frame.
 *
 *   Mapa de memoria BRAM (Port B word addr = AXI byte addr / 4):
 *     Palabra 0 (0x00): {6'b0, ball_y[9:0], 6'b0, ball_x[9:0]}
 *     Palabra 1 (0x04): {6'b0, pad2_y[9:0], 6'b0, pad1_y[9:0]}
 *     Palabra 2 (0x08): {16'b0, score2[7:0], score1[7:0]}
 *     Palabra 3 (0x0C): {28'b0, selected[1:0], game_state[1:0]}
 *
 * @author JustinAlfaro
 * @date 2026-06-10
 */

`timescale 1ns / 1ps

module top_pong_project (
    // --- Clock & Reset ---
    input  wire        CLK100MHZ,       ///< Reloj principal 100 MHz (oscilador onboard)
    input  wire        CPU_RESETN,      ///< Reset global activo bajo (BTN_CPU_RESET)

    // --- Botones (jugador) ---
    input  wire        BTNU,            ///< Paleta arriba / navegar menú arriba
    input  wire        BTND,            ///< Paleta abajo  / navegar menú abajo
    input  wire        BTNC,            ///< Abrir menú / confirmar selección
    input  wire        BTNL,            ///< Reservado (declarado, sin función asignada)
    input  wire        BTNR,            ///< Reservado (declarado, sin función asignada)

    // --- Switch modo de juego ---
    input  wire        SW,              ///< SW[0]: 1 = habilita comunicación SPI en modo 2P

    // --- LEDs de estado ---
    output wire [15:0] LED,             ///< [15] = DDR2 calib done; [14:0] = controlados por firmware

    // --- VGA ---
    output wire [3:0]  VGA_R,           ///< Canal rojo VGA 4 bits
    output wire [3:0]  VGA_G,           ///< Canal verde VGA 4 bits
    output wire [3:0]  VGA_B,           ///< Canal azul VGA 4 bits
    output wire        VGA_HS,          ///< Sincronismo horizontal activo bajo
    output wire        VGA_VS,          ///< Sincronismo vertical activo bajo

    // --- UART (USB-UART integrado Nexys A7) ---
    input  wire        UART_RXD_OUT,    ///< Pin D4: datos USB → FPGA (RX del FPGA)
    output wire        UART_TXD_IN,     ///< Pin C4: datos FPGA → USB (TX del FPGA)

    // --- SPI inter-FPGA (pines PMOD por confirmar con el otro grupo) ---
    output wire        SPI_SCK,         ///< Reloj SPI — TODO: re-exponer sck_io en BD (ausente tras retargeting)
    inout  wire        SPI_MOSI,        ///< MOSI bidir — IOBUF generado internamente por el BD wrapper
    inout  wire        SPI_MISO,        ///< MISO bidir — IOBUF generado internamente por el BD wrapper
    inout  wire [0:0]  SPI_CS_N,        ///< Chip select bidir — IOBUF generado internamente por el BD wrapper

    // --- DDR2 SDRAM (MT47H64M16HR-25E, 128 MB, 16-bit) ---
    inout  wire [15:0] ddr2_dq,         ///< Bus de datos bidireccional 16 bits
    inout  wire [1:0]  ddr2_dqs_p,      ///< Data strobe diferencial positivo
    inout  wire [1:0]  ddr2_dqs_n,      ///< Data strobe diferencial negativo
    output wire [12:0] ddr2_addr,       ///< Dirección de fila/columna 13 bits
    output wire [2:0]  ddr2_ba,         ///< Selección de banco
    output wire        ddr2_ras_n,      ///< Row address strobe activo bajo
    output wire        ddr2_cas_n,      ///< Column address strobe activo bajo
    output wire        ddr2_we_n,       ///< Write enable activo bajo
    output wire [0:0]  ddr2_ck_p,       ///< Reloj DDR2 diferencial positivo
    output wire [0:0]  ddr2_ck_n,       ///< Reloj DDR2 diferencial negativo
    output wire [0:0]  ddr2_cke,        ///< Clock enable
    output wire [0:0]  ddr2_cs_n,       ///< Chip select activo bajo
    output wire [1:0]  ddr2_dm,         ///< Data mask (write byte enable)
    output wire [0:0]  ddr2_odt         ///< On-die termination
);

// -----------------------------------------------------------------------------
// SPI — tras retargeting el wrapper expone _io (inout con IOBUF interno).
// Los puertos _i/_o/_t son señales internas del wrapper, no accesibles externamente.
// SCK no quedó expuesto; se mantiene en 0 hasta corregir el Block Design.
// -----------------------------------------------------------------------------
assign SPI_SCK = 1'b0;

// DDR2 calibration status (conectar a LED[15] como indicador de arranque)
wire ddr2_calib_done;
assign LED[15] = ddr2_calib_done;

// -----------------------------------------------------------------------------
// Acondicionamiento de entradas — botones y switch
//
// Botones: debounce incluye sincronizador de 2 etapas.
//   btn_x_db  : pulso de 1 ciclo al confirmarse el flanco (no usable por polling)
//   btn_x_lvl : nivel estabilizado — lo que MicroBlaze lee via GPIO (polling)
//
// SW: sync_signal de 1 bit elimina metaestabilidad sin alterar el nivel.
// -----------------------------------------------------------------------------
wire btn_u_db,  btn_d_db,  btn_c_db,  btn_l_db,  btn_r_db;
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

// -----------------------------------------------------------------------------
// VRAM Port B — señales internas
// MicroBlaze escribe por Port A (AXI), FSM de vblank lee estado del juego
// -----------------------------------------------------------------------------
wire [31:0] vram_portb_addr;
wire        vram_portb_clk;
wire [31:0] vram_portb_din;
wire [31:0] vram_portb_dout;
wire        vram_portb_en;
wire        vram_portb_rst;
wire [3:0]  vram_portb_we;

// -----------------------------------------------------------------------------
// Instancia del Block Design MicroBlaze V SoC
// -----------------------------------------------------------------------------
microblaze_v_wrapper u_soc (
    // Reloj y reset
    .clk_100MHz            (CLK100MHZ),
    .reset_rtl_0           (CPU_RESETN),

    // DDR2 calibración completada
    .init_calib_complete_0 (ddr2_calib_done),

    // GPIO0 — botones [4:0] = {BTNR, BTNL, BTNC, BTND, BTNU} (nivel estabilizado)
    .gpio_rtl_0_tri_i      ({btn_r_lvl, btn_l_lvl, btn_c_lvl, btn_d_lvl, btn_u_lvl}),

    // GPIO0 ch2 — SW[0] (habilita SPI, sincronizado)
    .gpio_rtl_1_tri_i      (sw_sync),

    // GPIO2 — LEDs [14:0] (LED[15] reservado para DDR2 calib)
    .gpio_rtl_2_tri_o      (LED[14:0]),

    // UART
    .uart_rtl_0_rxd        (UART_RXD_OUT),
    .uart_rtl_0_txd        (UART_TXD_IN),

    // SPI — wrapper genera IOBUFs internamente; conectar _io directo al pad
    .spi_rtl_0_io0_io      (SPI_MOSI),
    .spi_rtl_0_io1_io      (SPI_MISO),
    .spi_rtl_0_ss_io       (SPI_CS_N),

    // DDR2
    .DDR2_0_dq             (ddr2_dq),
    .DDR2_0_dqs_p          (ddr2_dqs_p),
    .DDR2_0_dqs_n          (ddr2_dqs_n),
    .DDR2_0_addr           (ddr2_addr),
    .DDR2_0_ba             (ddr2_ba),
    .DDR2_0_ras_n          (ddr2_ras_n),
    .DDR2_0_cas_n          (ddr2_cas_n),
    .DDR2_0_we_n           (ddr2_we_n),
    .DDR2_0_ck_p           (ddr2_ck_p),
    .DDR2_0_ck_n           (ddr2_ck_n),
    .DDR2_0_cke            (ddr2_cke),
    .DDR2_0_cs_n           (ddr2_cs_n),
    .DDR2_0_dm             (ddr2_dm),
    .DDR2_0_odt            (ddr2_odt),

    // VRAM Port B (VGA lee píxeles directamente)
    .BRAM_PORTB_0_addr     (vram_portb_addr),
    .BRAM_PORTB_0_clk      (vram_portb_clk),
    .BRAM_PORTB_0_din      (vram_portb_din),
    .BRAM_PORTB_0_dout     (vram_portb_dout),
    .BRAM_PORTB_0_en       (vram_portb_en),
    .BRAM_PORTB_0_rst      (vram_portb_rst),
    .BRAM_PORTB_0_we       (vram_portb_we)
);

// -----------------------------------------------------------------------------
// Pixel clock 25 MHz — tick_25mhz como clock enable (1 pulso cada 4 ciclos)
// -----------------------------------------------------------------------------
wire tick_25mhz;

div_freq #(.DIVISOR(4)) u_div_25mhz (
    .clk_in  (CLK100MHZ),
    .rst     (~CPU_RESETN),
    .clk_out (tick_25mhz)
);

// -----------------------------------------------------------------------------
// Controlador VGA 640x480 @ 60 Hz
// -----------------------------------------------------------------------------
wire       vga_blank;
wire [9:0] h_count, v_count;

vga_controller u_vga_ctrl (
    .clk        (CLK100MHZ),
    .rst        (~CPU_RESETN),
    .tick_25mhz (tick_25mhz),
    .hsync      (VGA_HS),
    .vsync      (VGA_VS),
    .blank      (vga_blank),
    .h_count    (h_count),
    .v_count    (v_count)
);

// -----------------------------------------------------------------------------
// Estado del juego — registros actualizados desde BRAM durante vblank
//
// Mapa de palabras BRAM (Port B word addr = AXI byte addr / 4):
//   Palabra 0 (AXI 0x00): {6'b0, ball_y[9:0], 6'b0, ball_x[9:0]}
//   Palabra 1 (AXI 0x04): {6'b0, pad2_y[9:0], 6'b0, pad1_y[9:0]}
//   Palabra 2 (AXI 0x08): {16'b0, score2[7:0], score1[7:0]}
// -----------------------------------------------------------------------------
reg [9:0] gs_ball_x, gs_ball_y;
reg [9:0] gs_pad1_y, gs_pad2_y;
reg [7:0] gs_score1, gs_score2;
reg [1:0] gs_game_state;   // 0=menu 1=playing 2=pause 3=gameover
reg [1:0] gs_selected;     // opción seleccionada en menú/pausa; ganador en gameover

reg [2:0] bram_rd_state;

// Mapa BRAM (word addr):
//   0: {6'b0, ball_y[9:0], 6'b0, ball_x[9:0]}
//   1: {6'b0, pad2_y[9:0], 6'b0, pad1_y[9:0]}
//   2: {16'b0, score2[7:0], score1[7:0]}
//   3: {28'b0, selected[1:0], game_state[1:0]}
always @(posedge CLK100MHZ) begin
    if (!CPU_RESETN) begin
        bram_rd_state  <= 3'd0;
        gs_ball_x      <= 10'd316; gs_ball_y  <= 10'd236;
        gs_pad1_y      <= 10'd210; gs_pad2_y  <= 10'd210;
        gs_score1      <= 8'd0;    gs_score2  <= 8'd0;
        gs_game_state  <= 2'd0;    gs_selected <= 2'd0;
    end else if (v_count >= 10'd480) begin
        case (bram_rd_state)
            3'd0: bram_rd_state <= 3'd1;
            3'd1: begin
                gs_ball_x     <= vram_portb_dout[9:0];
                gs_ball_y     <= vram_portb_dout[25:16];
                bram_rd_state <= 3'd2;
            end
            3'd2: begin
                gs_pad1_y     <= vram_portb_dout[9:0];
                gs_pad2_y     <= vram_portb_dout[25:16];
                bram_rd_state <= 3'd3;
            end
            3'd3: begin
                gs_score1     <= vram_portb_dout[7:0];
                gs_score2     <= vram_portb_dout[15:8];
                bram_rd_state <= 3'd4;
            end
            3'd4: begin
                gs_game_state <= vram_portb_dout[1:0];
                gs_selected   <= vram_portb_dout[3:2];
            end
            default: bram_rd_state <= 3'd0;
        endcase
    end else begin
        bram_rd_state <= 3'd0;
    end
end

assign vram_portb_clk  = CLK100MHZ;
assign vram_portb_addr = {29'b0, bram_rd_state};
assign vram_portb_din  = 32'h0;
assign vram_portb_en   = (v_count >= 10'd480);
assign vram_portb_rst  = ~CPU_RESETN;
assign vram_portb_we   = 4'b0;

// -----------------------------------------------------------------------------
// Renderizado procedural — geometría calculada combinacionalmente cada pixel.
// El firmware escribe 4 palabras de estado en BRAM; la FSM las captura en vblank.
//
// Pantallas (gs_game_state):
//   0 = menú principal  (1P / 2P)
//   1 = jugando         (pelota, paletas, red, marcador)
//   2 = pausa           (Reanudar / Salir) + juego difuminado de fondo
//   3 = gameover        (barra verde=ganaste / roja=perdiste)
// -----------------------------------------------------------------------------

// --- Juego ---
localparam BALL_SIZE = 10'd8;
localparam PAD_W     = 10'd8;
localparam PAD_H     = 10'd60;
localparam PAD1_X    = 10'd20;
localparam PAD2_X    = 10'd612;

wire in_ball = (h_count >= gs_ball_x)  && (h_count < gs_ball_x + BALL_SIZE) &&
               (v_count >= gs_ball_y)  && (v_count < gs_ball_y + BALL_SIZE);
wire in_pad1 = (h_count >= PAD1_X)    && (h_count < PAD1_X + PAD_W) &&
               (v_count >= gs_pad1_y) && (v_count < gs_pad1_y + PAD_H);
wire in_pad2 = (h_count >= PAD2_X)    && (h_count < PAD2_X + PAD_W) &&
               (v_count >= gs_pad2_y) && (v_count < gs_pad2_y + PAD_H);
wire in_net  = (h_count >= 10'd318)   && (h_count <= 10'd321) && (v_count[3] == 1'b0);

// --- Parpadeo (~1.5 Hz) ---
reg [25:0] blink_ctr;
always @(posedge CLK100MHZ) blink_ctr <= blink_ctr + 1;
wire blink = blink_ctr[25];

// --- Opciones de menú/pausa (200×40 px, centradas en x=320) ---
wire in_opt0 = (h_count >= 10'd220) && (h_count < 10'd420) &&
               (v_count >= 10'd180) && (v_count < 10'd220);
wire in_opt1 = (h_count >= 10'd220) && (h_count < 10'd420) &&
               (v_count >= 10'd260) && (v_count < 10'd300);

wire [11:0] opt0_color = (gs_selected == 2'd0) ? (blink ? 12'hFF0 : 12'h000) : 12'h333;
wire [11:0] opt1_color = (gs_selected == 2'd1) ? (blink ? 12'hFF0 : 12'h000) : 12'h333;

// --- Barra de gameover (400×60 px, centrada) ---
wire in_winner = (h_count >= 10'd120) && (h_count < 10'd520) &&
                 (v_count >= 10'd210) && (v_count < 10'd270);

// --- Font 3×5 para marcador (8× escala → 24×40 px por dígito) ---
// Glifo: {fila0[2:0], fila1[2:0], fila2[2:0], fila3[2:0], fila4[2:0]}
// bit[2]=col izq, bit[0]=col der
function [14:0] digit_glyph;
    input [3:0] d;
    case (d)
        4'd0: digit_glyph = 15'b111_101_101_101_111;
        4'd1: digit_glyph = 15'b010_010_010_010_010;
        4'd2: digit_glyph = 15'b111_001_111_100_111;
        4'd3: digit_glyph = 15'b111_001_111_001_111;
        4'd4: digit_glyph = 15'b101_101_111_001_001;
        4'd5: digit_glyph = 15'b111_100_111_001_111;
        4'd6: digit_glyph = 15'b111_100_111_101_111;
        4'd7: digit_glyph = 15'b111_001_001_001_001;
        4'd8: digit_glyph = 15'b111_101_111_101_111;
        4'd9: digit_glyph = 15'b111_101_111_001_111;
        default: digit_glyph = 15'b0;
    endcase
endfunction

// Score 1: esquina superior izquierda en (264, 8); Score 2: (352, 8)
wire in_s1 = (h_count >= 10'd264) && (h_count < 10'd288) &&
             (v_count >= 10'd8)   && (v_count < 10'd48);
wire in_s2 = (h_count >= 10'd352) && (h_count < 10'd376) &&
             (v_count >= 10'd8)   && (v_count < 10'd48);

wire [1:0] s1_col = (h_count - 10'd264) >> 3;
wire [2:0] s1_row = (v_count - 10'd8)   >> 3;
wire [1:0] s2_col = (h_count - 10'd352) >> 3;
wire [2:0] s2_row = (v_count - 10'd8)   >> 3;

wire [14:0] s1_glyph = digit_glyph(gs_score1[3:0]);
wire [14:0] s2_glyph = digit_glyph(gs_score2[3:0]);

reg [2:0] s1_rb, s2_rb;
always @(*) begin
    case (s1_row)
        3'd0: s1_rb = s1_glyph[14:12]; 3'd1: s1_rb = s1_glyph[11:9];
        3'd2: s1_rb = s1_glyph[8:6];   3'd3: s1_rb = s1_glyph[5:3];
        3'd4: s1_rb = s1_glyph[2:0];   default: s1_rb = 3'b0;
    endcase
    case (s2_row)
        3'd0: s2_rb = s2_glyph[14:12]; 3'd1: s2_rb = s2_glyph[11:9];
        3'd2: s2_rb = s2_glyph[8:6];   3'd3: s2_rb = s2_glyph[5:3];
        3'd4: s2_rb = s2_glyph[2:0];   default: s2_rb = 3'b0;
    endcase
end

wire s1_px = in_s1 && (s1_col == 2'd0 ? s1_rb[2] : s1_col == 2'd1 ? s1_rb[1] : s1_rb[0]);
wire s2_px = in_s2 && (s2_col == 2'd0 ? s2_rb[2] : s2_col == 2'd1 ? s2_rb[1] : s2_rb[0]);

// --- Mux de color por pantalla ---
wire [11:0] pixel_data =
    (gs_game_state == 2'd0) ? (                          // MENÚ
        in_opt0 ? opt0_color :
        in_opt1 ? opt1_color : 12'h000
    ) :
    (gs_game_state == 2'd1) ? (                          // JUGANDO
        (s1_px || s2_px) ? 12'hFF0 :
        in_ball           ? 12'hFFF :
        in_pad1           ? 12'hFFF :
        in_pad2           ? 12'hFFF :
        in_net            ? 12'h888 : 12'h000
    ) :
    (gs_game_state == 2'd2) ? (                          // PAUSA
        in_opt0 ? opt0_color :
        in_opt1 ? opt1_color :
        in_ball ? 12'h444 :
        in_pad1 ? 12'h444 :
        in_pad2 ? 12'h444 :
        in_net  ? 12'h222 : 12'h111
    ) :
    (                                                     // GAMEOVER
        in_winner ? (gs_selected == 2'd0 ? 12'h0F0 : 12'hF00) : 12'h000
    );

// -----------------------------------------------------------------------------
// Salida VGA — pixel_mux apaga RGB durante blanking
// -----------------------------------------------------------------------------
pixel_mux u_pixel_mux (
    .blank      (vga_blank),
    .pixel_data (pixel_data),
    .vga_r      (VGA_R),
    .vga_g      (VGA_G),
    .vga_b      (VGA_B)
);

endmodule
