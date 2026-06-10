# Script      : run_synth_impl.tcl
# Descripcion : Abre el proyecto HoG/Vivado, genera el wrapper del Block Design
#               si no existe, lanza síntesis e implementación y cierra.
#               Invocado por parse_utilization.sh en modo batch.
# Uso         : vivado -mode batch -source scripts/run_synth_impl.tcl \
#                       -tclargs <project.xpr>

set xpr [lindex $argv 0]
open_project $xpr
config_ip_cache -disable_cache

# ── Wrapper del Block Design ─────────────────────────────────────────────────
# En un clon limpio el wrapper no existe; hay que generarlo antes de sintetizar.
set bd_files [get_files -quiet "*.bd"]
if {[llength $bd_files] > 0} {
    set bd [lindex $bd_files 0]
    set wrapper_files [get_files -quiet "*wrapper*"]
    if {[llength $wrapper_files] == 0} {
        puts "INFO: Wrapper no encontrado — generando outputs del Block Design..."
        generate_target all $bd
        set wrapper [make_wrapper -files $bd -top]
        add_files -norecurse $wrapper
        update_compile_order -fileset sources_1
        puts "INFO: Wrapper generado y agregado al proyecto."
    } else {
        puts "INFO: Wrapper ya existe — omitiendo generación."
    }
}

# ── Síntesis ─────────────────────────────────────────────────────────────────
reset_run synth_1
launch_runs synth_1 -jobs 2
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} {
    puts "ERROR: La síntesis falló."
    close_project
    exit 1
}

# ── Implementación ───────────────────────────────────────────────────────────
reset_run impl_1
launch_runs impl_1 -to_step route_design -jobs 2
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    puts "ERROR: La implementación falló."
    close_project
    exit 1
}

# ── Reporte de timing post-route ─────────────────────────────────────────────
# Genera el reporte explícitamente para garantizar que parse_utilization.sh
# siempre encuentre el archivo, independiente de la configuración del run.
open_run impl_1
report_timing_summary -delay_type min_max -report_unconstrained \
    -check_timing_verbose -max_paths 10 -input_pins \
    -file [get_property DIRECTORY [get_runs impl_1]]/top_pong_project_timing_summary_routed.rpt

close_project
exit 0
