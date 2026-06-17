## =============================================================================
## Constraints : Nexys A7-100T — Pong Multijugador (MicroBlaze V)
## Board       : Digilent Nexys A7-100T (xc7a100tcsg324-1)
## Source      : Digilent Nexys-A7-100T-Master.xdc
## =============================================================================

## -----------------------------------------------------------------------------
## Clock — 100 MHz onboard oscillator
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN E3  IOSTANDARD LVCMOS33 } [get_ports { CLK100MHZ }]
create_clock -add -name sys_clk_pin -period 10.00 -waveform {0 5} [get_ports { CLK100MHZ }]

## -----------------------------------------------------------------------------
## Reset — CPU_RESETN (activo bajo)
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN C12 IOSTANDARD LVCMOS33 } [get_ports { CPU_RESETN }]

## -----------------------------------------------------------------------------
## Botones — control de jugadores y sistema
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN N17 IOSTANDARD LVCMOS33 } [get_ports { BTNC }]
set_property -dict { PACKAGE_PIN M18 IOSTANDARD LVCMOS33 } [get_ports { BTNU }]
set_property -dict { PACKAGE_PIN P18 IOSTANDARD LVCMOS33 } [get_ports { BTND }]
set_property -dict { PACKAGE_PIN M17 IOSTANDARD LVCMOS33 } [get_ports { BTNR }]
set_property -dict { PACKAGE_PIN P17 IOSTANDARD LVCMOS33 } [get_ports { BTNL }]

## -----------------------------------------------------------------------------
## Switches — SW[15:0]
## Nota: SW[8] y SW[9] estan en banco 34 (1.8 V) — usar LVCMOS18.
## -----------------------------------------------------------------------------
## SW: top declara 'input wire SW' (escalar = solo SW[0] del board)
set_property -dict { PACKAGE_PIN J15 IOSTANDARD LVCMOS33 } [get_ports { SW }]
## SW[1..15] no usados en este diseño (top solo expone el bit 0 como modo 1P/2P)
# set_property -dict { PACKAGE_PIN L16 IOSTANDARD LVCMOS33 } [get_ports { SW[1] }]
# set_property -dict { PACKAGE_PIN M13 IOSTANDARD LVCMOS33 } [get_ports { SW[2] }]
# set_property -dict { PACKAGE_PIN R15 IOSTANDARD LVCMOS33 } [get_ports { SW[3] }]
# set_property -dict { PACKAGE_PIN R17 IOSTANDARD LVCMOS33 } [get_ports { SW[4] }]
# set_property -dict { PACKAGE_PIN T18 IOSTANDARD LVCMOS33 } [get_ports { SW[5] }]
# set_property -dict { PACKAGE_PIN U18 IOSTANDARD LVCMOS33 } [get_ports { SW[6] }]
# set_property -dict { PACKAGE_PIN R13 IOSTANDARD LVCMOS33 } [get_ports { SW[7] }]
# set_property -dict { PACKAGE_PIN T8  IOSTANDARD LVCMOS18 } [get_ports { SW[8] }]
# set_property -dict { PACKAGE_PIN U8  IOSTANDARD LVCMOS18 } [get_ports { SW[9] }]
# set_property -dict { PACKAGE_PIN R16 IOSTANDARD LVCMOS33 } [get_ports { SW[10] }]
# set_property -dict { PACKAGE_PIN T13 IOSTANDARD LVCMOS33 } [get_ports { SW[11] }]
# set_property -dict { PACKAGE_PIN H6  IOSTANDARD LVCMOS33 } [get_ports { SW[12] }]
# set_property -dict { PACKAGE_PIN U12 IOSTANDARD LVCMOS33 } [get_ports { SW[13] }]
# set_property -dict { PACKAGE_PIN U11 IOSTANDARD LVCMOS33 } [get_ports { SW[14] }]
# set_property -dict { PACKAGE_PIN V10 IOSTANDARD LVCMOS33 } [get_ports { SW[15] }]

## -----------------------------------------------------------------------------
## LEDs — LED[15:0]
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN H17 IOSTANDARD LVCMOS33 } [get_ports { LED[0] }]
set_property -dict { PACKAGE_PIN K15 IOSTANDARD LVCMOS33 } [get_ports { LED[1] }]
set_property -dict { PACKAGE_PIN J13 IOSTANDARD LVCMOS33 } [get_ports { LED[2] }]
set_property -dict { PACKAGE_PIN N14 IOSTANDARD LVCMOS33 } [get_ports { LED[3] }]
set_property -dict { PACKAGE_PIN R18 IOSTANDARD LVCMOS33 } [get_ports { LED[4] }]
set_property -dict { PACKAGE_PIN V17 IOSTANDARD LVCMOS33 } [get_ports { LED[5] }]
set_property -dict { PACKAGE_PIN U17 IOSTANDARD LVCMOS33 } [get_ports { LED[6] }]
set_property -dict { PACKAGE_PIN U16 IOSTANDARD LVCMOS33 } [get_ports { LED[7] }]
set_property -dict { PACKAGE_PIN V16 IOSTANDARD LVCMOS33 } [get_ports { LED[8] }]
set_property -dict { PACKAGE_PIN T15 IOSTANDARD LVCMOS33 } [get_ports { LED[9] }]
set_property -dict { PACKAGE_PIN U14 IOSTANDARD LVCMOS33 } [get_ports { LED[10] }]
set_property -dict { PACKAGE_PIN T16 IOSTANDARD LVCMOS33 } [get_ports { LED[11] }]
set_property -dict { PACKAGE_PIN V15 IOSTANDARD LVCMOS33 } [get_ports { LED[12] }]
set_property -dict { PACKAGE_PIN V14 IOSTANDARD LVCMOS33 } [get_ports { LED[13] }]
set_property -dict { PACKAGE_PIN V12 IOSTANDARD LVCMOS33 } [get_ports { LED[14] }]
set_property -dict { PACKAGE_PIN V11 IOSTANDARD LVCMOS33 } [get_ports { LED[15] }]

## -----------------------------------------------------------------------------
## VGA Connector
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN A3  IOSTANDARD LVCMOS33 } [get_ports { VGA_R[0] }]
set_property -dict { PACKAGE_PIN B4  IOSTANDARD LVCMOS33 } [get_ports { VGA_R[1] }]
set_property -dict { PACKAGE_PIN C5  IOSTANDARD LVCMOS33 } [get_ports { VGA_R[2] }]
set_property -dict { PACKAGE_PIN A4  IOSTANDARD LVCMOS33 } [get_ports { VGA_R[3] }]
set_property -dict { PACKAGE_PIN C6  IOSTANDARD LVCMOS33 } [get_ports { VGA_G[0] }]
set_property -dict { PACKAGE_PIN A5  IOSTANDARD LVCMOS33 } [get_ports { VGA_G[1] }]
set_property -dict { PACKAGE_PIN B6  IOSTANDARD LVCMOS33 } [get_ports { VGA_G[2] }]
set_property -dict { PACKAGE_PIN A6  IOSTANDARD LVCMOS33 } [get_ports { VGA_G[3] }]
set_property -dict { PACKAGE_PIN B7  IOSTANDARD LVCMOS33 } [get_ports { VGA_B[0] }]
set_property -dict { PACKAGE_PIN C7  IOSTANDARD LVCMOS33 } [get_ports { VGA_B[1] }]
set_property -dict { PACKAGE_PIN D7  IOSTANDARD LVCMOS33 } [get_ports { VGA_B[2] }]
set_property -dict { PACKAGE_PIN D8  IOSTANDARD LVCMOS33 } [get_ports { VGA_B[3] }]
set_property -dict { PACKAGE_PIN B11 IOSTANDARD LVCMOS33 } [get_ports { VGA_HS }]
set_property -dict { PACKAGE_PIN B12 IOSTANDARD LVCMOS33 } [get_ports { VGA_VS }]

## -----------------------------------------------------------------------------
## UART — USB-UART integrado (mismo cable USB de programacion)
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN C4  IOSTANDARD LVCMOS33 } [get_ports { UART_TXD_IN }]
set_property -dict { PACKAGE_PIN D4  IOSTANDARD LVCMOS33 } [get_ports { UART_RXD_OUT }]

## -----------------------------------------------------------------------------
## DDR2 — administrado por MIG 7 Series IP (pines incluidos en el BD)
## Los pines DDR2 se restringen automaticamente por el IP core MIG.
## No agregar constraints manuales para DDR2 aqui.
## -----------------------------------------------------------------------------

## -----------------------------------------------------------------------------
## SPI inter-FPGA — PMOD JA (provisional; confirmar pines con el otro grupo)
## JA1=C17(SCK) JA2=D18(MOSI) JA3=E18(MISO) JA4=G17(CS_N)
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN C17 IOSTANDARD LVCMOS33 } [get_ports { SPI_SCK }]
set_property -dict { PACKAGE_PIN D18 IOSTANDARD LVCMOS33 } [get_ports { SPI_MOSI }]
set_property -dict { PACKAGE_PIN E18 IOSTANDARD LVCMOS33 } [get_ports { SPI_MISO }]
set_property -dict { PACKAGE_PIN G17 IOSTANDARD LVCMOS33 } [get_ports { SPI_CS_N[0] }]

## -----------------------------------------------------------------------------
## microSD — conector integrado de la Nexys A7
## SPI mode: SCK=B1(sd_sck) MOSI=C1(sd_cmd) MISO=C2(sd_dat[0]) CS_N=D2(sd_dat[3])
## SD_RESET=E2: mantener en 0 para liberar la SD al usuario tras el arranque de la FPGA
## -----------------------------------------------------------------------------
set_property -dict { PACKAGE_PIN B1  IOSTANDARD LVCMOS33 } [get_ports { SD_SCK }]
set_property -dict { PACKAGE_PIN C1  IOSTANDARD LVCMOS33 } [get_ports { SD_MOSI }]
set_property -dict { PACKAGE_PIN C2  IOSTANDARD LVCMOS33 } [get_ports { SD_MISO }]
set_property -dict { PACKAGE_PIN D2  IOSTANDARD LVCMOS33 } [get_ports { SD_CS_N[0] }]
set_property -dict { PACKAGE_PIN E2  IOSTANDARD LVCMOS33 } [get_ports { SD_RESET }]

## =============================================================================
## Excepciones de temporización — puertos sin requisito de tiempo externo
## Elimina las 44 violaciones TIMING-18 (missing input/output delay).
## Todos estos puertos son asíncronos o usan protocolos con su propio reloj:
##   Botones/SW/RESETN : entradas mecánicas asíncronas (debounce en fabric)
##   LEDs              : indicadores, sin referencia externa de reloj
##   VGA               : manejado por reloj de pixel dividido internamente
##   UART / SPI        : protocolos serie con su propio baud/SCK
## =============================================================================

## Entradas asíncronas
set_false_path -from [get_ports {BTNC BTND BTNL BTNR BTNU}]
set_false_path -from [get_ports {SW}]
set_false_path -from [get_ports {CPU_RESETN}]
set_false_path -from [get_ports {UART_RXD_OUT}]
set_false_path -from [get_ports {SPI_MOSI SPI_MISO}]
set_false_path -from [get_ports {SD_MISO}]

## Salidas asíncronas
set_false_path -to [get_ports {LED[*]}]
set_false_path -to [get_ports {VGA_R[*] VGA_G[*] VGA_B[*] VGA_HS VGA_VS}]
set_false_path -to [get_ports {UART_TXD_IN}]
set_false_path -to [get_ports {SPI_MOSI SPI_MISO SPI_CS_N[*]}]
set_false_path -to [get_ports {SD_MOSI SD_SCK SD_CS_N[*] SD_RESET}]

## =============================================================================
## Waivers — artefactos conocidos del IP Clock Wizard en Block Design
## TIMING-4  : El XDC no-OOC del IP clk_wiz_1 define create_clock en clk_in1,
##             que Vivado ve como redefinición de sys_clk_pin. El timing cierra
##             correctamente (WNS > 0). Artefacto documentado de Xilinx BD.
## TIMING-27 : Misma causa: primary clock en pin jerárquico interno del BD.
## =============================================================================
create_waiver -id TIMING-4  -description {BD clk_wiz_1 IP: create_clock redundante en clk_in1 downstream de sys_clk_pin (artefacto Xilinx BD)}
create_waiver -id TIMING-27 -description {BD clk_wiz_1 IP: primary clock en pin jerarquico clk_wiz_1/inst/clk_in1 (artefacto Xilinx BD)}
