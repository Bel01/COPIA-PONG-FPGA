# build_bitstream.tcl
# Abre el proyecto, re-sintetiza si hay cambios, implementa y escribe bitstream.
# Uso: vivado -mode batch -source scripts/build_bitstream.tcl

set proj_file [file normalize "Projects/pong_project/pong_project.xpr"]
set out_dir   [file normalize "bin/build_latest"]

puts "INFO: Abriendo proyecto $proj_file ..."
open_project $proj_file

# Fuerza re-síntesis para asegurar que el HDL nuevo se incluye
puts "INFO: Reseteando runs para forzar re-síntesis..."
reset_run synth_1
reset_run impl_1

puts "INFO: Lanzando síntesis (synth_1)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} {
    puts "ERROR: Síntesis falló."
    exit 1
}
puts "INFO: Síntesis completada."

puts "INFO: Lanzando implementación + write_bitstream (impl_1)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "ERROR: Implementación falló."
    exit 1
}
puts "INFO: Implementación completada."

# Copia el bitstream a bin/build_latest
file mkdir $out_dir
set bit_src [get_property DIRECTORY [get_runs impl_1]]/top_pong_project.bit
file copy -force $bit_src $out_dir/top_pong_project.bit
puts "INFO: Bitstream copiado a $out_dir/top_pong_project.bit"

close_project
puts "DONE: Bitstream listo."
