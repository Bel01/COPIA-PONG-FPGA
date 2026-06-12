# =============================================================================
# bd_update_framebuffer_spi.tcl
#
# Actualiza el Block Design microblaze_v.bd para la nueva arquitectura:
#   1. BRAM → framebuffer 640×480×4-bit (Port A 32-bit CPU / Port B 4-bit VGA)
#   2. AXI Quad SPI 1 → microSD onboard (pines B1/C1/D1/E2)
#
# Uso: En la consola Tcl de Vivado (con el proyecto abierto):
#   source scripts/bd_update_framebuffer_spi.tcl
# =============================================================================

# Abre el Block Design si no está ya abierto
if {[llength [get_bd_designs]] == 0} {
    open_bd_design [get_files microblaze_v.bd]
}

puts "INFO: Iniciando actualización del Block Design..."

# =============================================================================
# 1. BRAM — framebuffer 640×480×4-bit
#    Port A (32-bit, MicroBlaze via AXI BRAM Ctrl): 38400 palabras × 32-bit
#    Port B  (4-bit, VGA directo):                  307200 pixels × 4-bit
#    Total: 38400 × 32 = 307200 × 4 = 1,228,800 bits ≈ 35 BRAMs (26% del A100T)
# =============================================================================
puts "INFO: Reconfigurando BRAM como framebuffer..."

set_property -dict [list \
    CONFIG.Memory_Type                                  {True_Dual_Port_RAM} \
    CONFIG.Write_Width_A                                {32}                 \
    CONFIG.Read_Width_A                                 {32}                 \
    CONFIG.Write_Depth_A                                {38400}              \
    CONFIG.Write_Width_B                                {4}                  \
    CONFIG.Read_Width_B                                 {4}                  \
    CONFIG.Enable_B                                     {Use_ENB_Pin}        \
    CONFIG.Register_PortA_Output_of_Memory_Primitives   {false}              \
    CONFIG.Register_PortB_Output_of_Memory_Primitives   {false}              \
] [get_bd_cells blk_mem_gen_0]

# Actualiza el rango del AXI BRAM Controller:
# 38400 palabras × 4 bytes = 153,600 bytes → redondear a 256 KB (potencia de 2)
set_property range 256K \
    [get_bd_addr_segs {microblaze_riscv_0/Data/SEG_axi_bram_ctrl_0_Mem0}]

puts "INFO: BRAM reconfigurada — 640×480×4-bit (256 KB address range)"

# =============================================================================
# 2. AXI Quad SPI 1 — microSD onboard
#    SCK→B1  MOSI→C1  MISO→D1  CS_N→E2
# =============================================================================
puts "INFO: Agregando AXI Quad SPI 1 para microSD..."

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_quad_spi:3.2 axi_quad_spi_1

set_property -dict [list \
    CONFIG.C_SPI_MODE    {0}   \
    CONFIG.C_NUM_SS_BITS {1}   \
    CONFIG.C_SCK_RATIO   {16}  \
    CONFIG.Multiples16   {2}   \
    CONFIG.C_FIFO_DEPTH  {16}  \
] [get_bd_cells axi_quad_spi_1]

# =============================================================================
# 3. Expande el AXI interconnect periférico de 7 a 8 masters
# =============================================================================
puts "INFO: Expandiendo AXI interconnect a 8 masters..."

set_property CONFIG.NUM_MI {8} [get_bd_cells microblaze_riscv_0_axi_periph]

# =============================================================================
# 4. Conecta SPI 1 al AXI interconnect (M07)
# =============================================================================
connect_bd_intf_net \
    [get_bd_intf_pins microblaze_riscv_0_axi_periph/M07_AXI] \
    [get_bd_intf_pins axi_quad_spi_1/AXI_LITE]

# Reloj y reset M07 — misma fuente que el resto de periféricos
connect_bd_net \
    [get_bd_pins microblaze_riscv_0_axi_periph/M07_ACLK] \
    [get_bd_pins clk_wiz_1/clk_out1]

connect_bd_net \
    [get_bd_pins microblaze_riscv_0_axi_periph/M07_ARESETN] \
    [get_bd_pins rst_clk_wiz_1_100M/peripheral_aresetn]

# SPI 1: reloj de operación y reloj AXI
connect_bd_net \
    [get_bd_pins axi_quad_spi_1/s_axi_aclk] \
    [get_bd_pins clk_wiz_1/clk_out1]

connect_bd_net \
    [get_bd_pins axi_quad_spi_1/ext_spi_clk] \
    [get_bd_pins clk_wiz_1/clk_out1]

connect_bd_net \
    [get_bd_pins axi_quad_spi_1/s_axi_aresetn] \
    [get_bd_pins rst_clk_wiz_1_100M/peripheral_aresetn]

# =============================================================================
# 5. Puerto externo para microSD (interfaz SPI RTL estándar de Xilinx)
# =============================================================================
create_bd_intf_port -mode Master -vlnv xilinx.com:interface:spi_rtl:1.0 spi_sd_0

connect_bd_intf_net \
    [get_bd_intf_pins axi_quad_spi_1/SPI_0] \
    [get_bd_intf_ports spi_sd_0]

# =============================================================================
# 6. Asigna dirección al nuevo SPI 1 (Vivado elige libre)
# =============================================================================
assign_bd_address [get_bd_addr_segs {axi_quad_spi_1/AXI_LITE/Reg}]

# =============================================================================
# 7. Valida y guarda
# =============================================================================
puts "INFO: Validando Block Design..."
set validation_result [validate_bd_design]

if {$validation_result == 0} {
    puts "INFO: Validación OK — guardando..."
    save_bd_design
    puts "INFO: Block Design actualizado y guardado."
    puts ""
    puts "PRÓXIMOS PASOS:"
    puts "  1. Actualizar XDC: descomentar pines SD (B1 C1 D1 E2)"
    puts "  2. Reescribir vga_controller.v para leer framebuffer"
    puts "  3. Regenerar wrapper: Tools → Generate Block Design"
    puts "  4. Re-sintetizar e implementar"
} else {
    puts "WARNING: Validación con advertencias — revisar el Block Design."
    save_bd_design
}
