# create_vitis_app.tcl
# Crea la plataforma Vitis y compila pong_app desde cero usando el XSA del repo.
#
# Uso:
#   source /tools/Xilinx2/Vitis/2024.1/settings64.sh
#   xsct scripts/create_vitis_app.tcl [REPO_ROOT] [WORKSPACE_PATH]
#
# Ejemplo (desde la raíz del repo):
#   xsct scripts/create_vitis_app.tcl $(pwd) $(pwd)/../pong_workspace
#
# Resultado: <WORKSPACE_PATH>/pong_app/build/pong_app.elf

set repo_root  [lindex $argv 0]
set ws_path    [lindex $argv 1]

if {$repo_root eq "" || $ws_path eq ""} {
    puts "Uso: xsct scripts/create_vitis_app.tcl <repo_root> <workspace_path>"
    exit 1
}

set repo_root [file normalize $repo_root]
set ws_path   [file normalize $ws_path]
set xsa_path  [file join $repo_root "top_pong_project.xsa"]

if {![file exists $xsa_path]} {
    puts "ERROR: No se encontró $xsa_path"
    exit 1
}

puts "INFO: Repo   : $repo_root"
puts "INFO: Workspace: $ws_path"
puts "INFO: XSA    : $xsa_path"

# ── Workspace ──────────────────────────────────────────────────────────────
setws $ws_path

# ── Plataforma ─────────────────────────────────────────────────────────────
puts "INFO: Creando plataforma pong_platform..."
platform create -name pong_platform \
    -hw     $xsa_path \
    -os     standalone \
    -proc   microblaze_riscv_0

platform write
platform generate -domains
puts "INFO: Plataforma generada."

# ── Aplicación ─────────────────────────────────────────────────────────────
puts "INFO: Creando pong_app..."
app create -name pong_app \
    -platform pong_platform \
    -proc     microblaze_riscv_0 \
    -os       standalone \
    -template "Empty Application(C)"

# Copiar fuentes del repo al workspace
set src_dir [file join $ws_path "pong_app" "src"]
file copy -force [file join $repo_root "src" "sw" "pong.c"]    [file join $src_dir "pong.c"]
file copy -force [file join $repo_root "src" "sw" "lscript.ld"] [file join $src_dir "lscript.ld"]
puts "INFO: Fuentes copiadas a $src_dir"

# ── Compilar ───────────────────────────────────────────────────────────────
puts "INFO: Compilando pong_app..."
app build -name pong_app

set elf [file join $ws_path "pong_app" "build" "pong_app.elf"]
if {[file exists $elf]} {
    puts "DONE: ELF generado en $elf"
} else {
    puts "WARN: El ELF no se encontró en la ruta esperada."
    puts "      Verifica el workspace manualmente."
}
