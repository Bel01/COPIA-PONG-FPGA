# Script      : regen_mig_resynth.tcl
# Descripcion : Regenera el IP MIG 7-series tras actualizar mig_a.prj y
#               relanza síntesis + implementación completas.
#               Necesario cuando InputClkFreq/TimePeriod del MIG cambian
#               porque el DCP pre-sintetizado queda obsoleto.
# Uso         : vivado -mode batch -source scripts/regen_mig_resynth.tcl \
#                       -tclargs <project.xpr>

set xpr [lindex $argv 0]
open_project $xpr
config_ip_cache -disable_cache

# ── Regenerar todos los outputs del Block Design (incluye MIG) ───────────────
# Los IPs dentro de un BD son "nested sub-designs" y deben regenerarse vía el BD,
# no directamente con get_ips. generate_target all sobre el BD regenera el MIG DCP.
set bd_files [get_files -quiet "*.bd"]
if {[llength $bd_files] == 0} {
    puts "ERROR: No se encontró ningún .bd en el proyecto."
    close_project
    exit 1
}
set bd [lindex $bd_files 0]
puts "INFO: Regenerando todos los outputs del Block Design (incluye MIG — ~10 min)..."
generate_target all $bd
puts "INFO: Block Design regenerado."

# Verificar/crear wrapper
set wrapper_files [get_files -quiet "*wrapper*"]
if {[llength $wrapper_files] == 0} {
    puts "INFO: Wrapper no encontrado — creando..."
    set wrapper [make_wrapper -files $bd -top]
    add_files -norecurse $wrapper
    update_compile_order -fileset sources_1
    puts "INFO: Wrapper creado."
} else {
    puts "INFO: Wrapper ya existe."
}

# ── Re-síntesis OOC del clk_wiz (forzar si XCI cambió) ──────────────────────
set clkwiz_run [get_runs -quiet microblaze_v_clk_wiz_1_0_synth_1]
if {[llength $clkwiz_run] > 0} {
    puts "INFO: Reseteando y relanzando síntesis OOC de clk_wiz_1..."
    reset_run $clkwiz_run
    launch_runs $clkwiz_run -jobs 2
    wait_on_run $clkwiz_run
    puts "INFO: clk_wiz_1 OOC synthesis completada."
}

# ── Síntesis ─────────────────────────────────────────────────────────────────
puts "INFO: Lanzando síntesis..."
reset_run synth_1
launch_runs synth_1 -jobs 2
wait_on_run synth_1
if {[get_property PROGRESS [get_runs synth_1]] ne "100%"} {
    puts "ERROR: La síntesis falló."
    close_project
    exit 1
}
puts "INFO: Síntesis completada."

# ── Implementación ───────────────────────────────────────────────────────────
puts "INFO: Lanzando implementación..."
reset_run impl_1
launch_runs impl_1 -to_step route_design -jobs 2
wait_on_run impl_1
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    puts "ERROR: La implementación falló."
    close_project
    exit 1
}
puts "INFO: Implementación completada."

# ── Reporte de timing ─────────────────────────────────────────────────────────
open_run impl_1
report_timing_summary -delay_type min_max -report_unconstrained \
    -check_timing_verbose -max_paths 10 -input_pins \
    -file [get_property DIRECTORY [get_runs impl_1]]/top_pong_project_timing_summary_routed.rpt

close_project
exit 0
