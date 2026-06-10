# Script      : run_impl.tcl
# Descripcion : Abre el proyecto Vivado y lanza solo la implementación
#               (placement + routing). Asume que la síntesis ya está completa.
#               Para correr síntesis + implementación juntas usa run_synth_impl.tcl.
#               Invocado por parse_utilization.sh en modo batch.
# Uso         : vivado -mode batch -source scripts/run_impl.tcl \
#                       -tclargs <project.xpr>

set xpr [lindex $argv 0]
open_project $xpr
config_ip_cache -disable_cache

# ── Verificar que la síntesis esté completa ──────────────────────────────────
set synth_progress [get_property PROGRESS [get_runs synth_1]]
if {$synth_progress ne "100%"} {
    puts "ERROR: La síntesis no está completa (PROGRESS=$synth_progress)."
    puts "       Corre primero run_synth_impl.tcl o lanza la síntesis desde Vivado."
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
open_run impl_1
report_timing_summary -delay_type min_max -report_unconstrained \
    -check_timing_verbose -max_paths 10 -input_pins \
    -file [get_property DIRECTORY [get_runs impl_1]]/top_pong_project_timing_summary_routed.rpt

close_project
exit 0
