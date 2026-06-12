# program_and_run.tcl — programa bitstream + ELF y arranca el juego
# Uso: xsdb scripts/program_and_run.tcl

# Bitstream más reciente generado por HoG
set candidates [lsort -decreasing [glob -nocomplain "bin/*/*.bit"]]
if {[llength $candidates] == 0} {
    puts "ERROR: No se encontro ningun bitstream en bin/"
    exit 1
}
set bitstream [file normalize [lindex $candidates 0]]
puts "INFO: Bitstream: $bitstream"

# ELF precompilado (generado por Vitis)
set elf [file normalize "/home/alpha/Documentos/Proyecto_2/pong_workspace/pong_app/build/pong_app.elf"]
if {![file exists $elf]} {
    set elf [file normalize "/home/alpha/Documentos/Proyecto_2/pong_app/build/pong_app.elf"]
}
puts "INFO: ELF: $elf"

connect
puts "INFO: Programando bitstream..."
fpga -f $bitstream
after 2000

targets -set -filter {name =~ "*Hart*"}
rst -processor
after 500

puts "INFO: Descargando ELF..."
dow $elf
after 500

puts "INFO: Arrancando CPU..."
con

puts "DONE: Pong corriendo."
