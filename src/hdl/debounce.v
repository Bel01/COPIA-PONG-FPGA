/**
 * @title Debouncer de botón
 * @file debounce.v
 * @brief Elimina rebotes de botones usando un contador de saturación. Genera un pulso de un ciclo en flanco estable.
 * @details
 *   Incluye un sincronizador de dos etapas para la entrada asíncrona.
 *   La salida btn_out se activa exactamente un ciclo de reloj cuando el botón
 *   permanece estable en alto durante DEBOUNCE_MS milisegundos.
 *
 * @author JustinAlfaro
 * @date 2026-04-22
 */

`timescale 1ns / 1ps

module debounce #(
    parameter integer DEBOUNCE_MS  = 20,          ///< Ventana de debounce en milisegundos
    parameter integer CLK_FREQ_HZ  = 100_000_000  ///< Frecuencia del reloj de entrada en Hz
)(
    input  wire clk,     ///< Reloj del sistema
    input  wire rst,     ///< Reset síncrono activo alto
    input  wire btn_in,   ///< Entrada cruda del botón (con rebotes)
    output reg  btn_out,  ///< Pulso de un ciclo en flanco confirmado de subida
    output wire btn_level ///< Nivel sincronizado y estabilizado (para polling)
);
    localparam integer COUNT_MAX = (CLK_FREQ_HZ / 1000) * DEBOUNCE_MS - 1;
    localparam integer CNT_BITS  = $clog2(COUNT_MAX + 1);

    (* ASYNC_REG = "TRUE" *) reg [1:0] sync_ff;
    wire      btn_sync = sync_ff[1];
    assign    btn_level = btn_sync;

    always @(posedge clk) begin
        if (rst) sync_ff <= 2'b00;
        else     sync_ff <= {sync_ff[0], btn_in};
    end

    reg [CNT_BITS-1:0] count;
    reg                btn_prev;

    always @(posedge clk) begin
        if (rst) begin
            count    <= 0;
            btn_prev <= 1'b0;
            btn_out  <= 1'b0;
        end else begin
            btn_out <= 1'b0;
            if (btn_sync != btn_prev) begin
                count    <= 0;
                btn_prev <= btn_sync;
            end else if (count < COUNT_MAX[CNT_BITS-1:0]) begin
                count <= count + 1'b1;
                if (count == COUNT_MAX[CNT_BITS-1:0] - 1'b1 && btn_sync)
                    btn_out <= 1'b1;
            end
        end
    end

endmodule
