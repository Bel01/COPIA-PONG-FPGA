# program_and_run_ddr2.tcl — firmware corriendo desde DDR2 (0x80000000)
#
# Requiere bitstream con M_AXI_IP habilitado en MicroBlaze V.
# El CPU puede fetchear instrucciones desde DDR2 via M_AXI_IP → AXI → MIG.
#
# Uso: xsdb scripts/program_and_run_ddr2.tcl

set bits [glob -nocomplain "bin/*/*.bit"]
if {[llength $bits] == 0} {
    puts "ERROR: No se encontro ningun bitstream en bin/"
    exit 1
}
# Ordenar por fecha de modificación (más reciente primero)
set bits_sorted [lsort -decreasing -command {apply {{a b} {
    expr {[file mtime $a] - [file mtime $b]}
}}} $bits]
set bitstream [file normalize [lindex $bits_sorted 0]]
puts "INFO: Bitstream: $bitstream"

set pong_elf [file normalize "/home/alpha/Documentos/Proyecto_2/pong_workspace/pong_app/build/pong_app.elf"]
if {![file exists $pong_elf]} {
    puts "ERROR: pong_app.elf no encontrado"
    exit 1
}
puts "INFO: ELF: $pong_elf (entry: 0x80000000)"

connect

puts "INFO: Programando bitstream..."
fpga -f $bitstream
puts "INFO: Esperando calibracion MIG DDR2 (3 s)..."
after 3000

targets -set -filter {name =~ "*Hart*"}
rst -processor
puts "INFO: Esperando recalibracion MIG tras reset (1.5 s)..."
after 1500

puts "INFO: Descargando ELF en DDR2 (0x80000000)..."
dow $pong_elf
after 500

puts "INFO: Estableciendo PC = 0x80000000 y arrancando..."
rwr pc 0x80000000
con

puts "DONE: Pong corriendo desde DDR2."
