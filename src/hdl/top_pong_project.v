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
    input  wire        SW,              ///< SW[0]: 0 = modo 2P (SPI), 1 = modo 1P (AI)

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
// Botones: debounce incluye sincronizador de 2 etapas. Salida = pulso de 1
// ciclo en flanco confirmado de subida (20 ms ventana). El firmware debe usar
// interrupciones AXI GPIO (canal 1, edge-detect) para capturarlos de forma
// fiable; el polling directo perdería el pulso.
//
// SW: sync_signal de 1 bit elimina metaestabilidad sin alterar el nivel.
// -----------------------------------------------------------------------------
wire btn_u_db, btn_d_db, btn_c_db, btn_l_db, btn_r_db;
wire sw_sync;

debounce u_db_btnu (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNU), .btn_out(btn_u_db));
debounce u_db_btnd (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTND), .btn_out(btn_d_db));
debounce u_db_btnc (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNC), .btn_out(btn_c_db));
debounce u_db_btnl (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNL), .btn_out(btn_l_db));
debounce u_db_btnr (.clk(CLK100MHZ), .rst(~CPU_RESETN), .btn_in(BTNR), .btn_out(btn_r_db));

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

    // GPIO0 — botones [4:0] = {BTNR, BTNL, BTNC, BTND, BTNU} (debounced)
    .gpio_rtl_0_tri_i      ({btn_r_db, btn_l_db, btn_c_db, btn_d_db, btn_u_db}),

    // GPIO1 — SW[0] (modo 1P/2P, sincronizado)
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

reg [1:0] bram_rd_state;

always @(posedge CLK100MHZ) begin
    if (!CPU_RESETN) begin
        bram_rd_state <= 2'd0;
        gs_ball_x     <= 10'd316;
        gs_ball_y     <= 10'd236;
        gs_pad1_y     <= 10'd210;
        gs_pad2_y     <= 10'd210;
        gs_score1     <= 8'd0;
        gs_score2     <= 8'd0;
    end else if (v_count >= 10'd480) begin
        // Durante vblank: leer 3 palabras secuenciales de BRAM.
        // La BRAM tiene latencia de 1 ciclo: en el estado N se presenta addr N,
        // en el estado N+1 llega el dato de addr N y se presenta addr N+1.
        case (bram_rd_state)
            2'd0: bram_rd_state <= 2'd1;            // setup addr 0, esperar dato
            2'd1: begin                              // capturar word 0 (pelota)
                gs_ball_x     <= vram_portb_dout[9:0];
                gs_ball_y     <= vram_portb_dout[25:16];
                bram_rd_state <= 2'd2;
            end
            2'd2: begin                              // capturar word 1 (paletas)
                gs_pad1_y     <= vram_portb_dout[9:0];
                gs_pad2_y     <= vram_portb_dout[25:16];
                bram_rd_state <= 2'd3;
            end
            2'd3: begin                              // capturar word 2 (marcador)
                gs_score1     <= vram_portb_dout[7:0];
                gs_score2     <= vram_portb_dout[15:8];
                // permanecer en estado 3 hasta fin de vblank
            end
        endcase
    end else begin
        bram_rd_state <= 2'd0;  // resetear FSM al inicio del frame activo
    end
end

// Port B: solo lectura. bram_rd_state indexa la palabra a leer.
assign vram_portb_clk  = CLK100MHZ;
assign vram_portb_addr = {30'b0, bram_rd_state};  // word address 0/1/2/3
assign vram_portb_din  = 32'h0;
assign vram_portb_en   = (v_count >= 10'd480);
assign vram_portb_rst  = ~CPU_RESETN;
assign vram_portb_we   = 4'b0;

// -----------------------------------------------------------------------------
// Renderizado procedural — color determinado por geometría en tiempo real
// No hay framebuffer: cada pixel se calcula combinacionalmente desde h_count/v_count.
// En este modo no hay latencia de BRAM por pixel, por lo que blank no necesita delay.
// -----------------------------------------------------------------------------
localparam BALL_SIZE = 10'd8;
localparam PAD_W     = 10'd8;
localparam PAD_H     = 10'd60;
localparam PAD1_X    = 10'd20;
localparam PAD2_X    = 10'd612;  // 640 - PAD1_X - PAD_W

wire in_ball = (h_count >= gs_ball_x) && (h_count < gs_ball_x + BALL_SIZE) &&
               (v_count >= gs_ball_y) && (v_count < gs_ball_y + BALL_SIZE);
wire in_pad1 = (h_count >= PAD1_X)   && (h_count < PAD1_X + PAD_W) &&
               (v_count >= gs_pad1_y) && (v_count < gs_pad1_y + PAD_H);
wire in_pad2 = (h_count >= PAD2_X)   && (h_count < PAD2_X + PAD_W) &&
               (v_count >= gs_pad2_y) && (v_count < gs_pad2_y + PAD_H);
wire in_net  = (h_count >= 10'd318) && (h_count <= 10'd321) && (v_count[3] == 1'b0);

wire [11:0] pixel_data = in_ball ? 12'hFFF :
                         in_pad1 ? 12'hFFF :
                         in_pad2 ? 12'hFFF :
                         in_net  ? 12'h888 :
                                   12'h000;

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
