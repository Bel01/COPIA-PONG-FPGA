#!/usr/bin/env bash
# build_all.sh — Detecta Vivado, crea el proyecto Vivado desde HoG si no existe,
#                y lanza síntesis + implementación + bitstream.
#
# Uso (desde cualquier directorio):
#   bash scripts/build_all.sh
#
# Si Vivado no está en PATH, exporta la raíz antes de correr:
#   export VIVADO_ROOT=/ruta/a/Vivado/2024.1
#   bash scripts/build_all.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_NAME="pong_project"
XPR="$REPO_ROOT/Projects/$PROJECT_NAME/$PROJECT_NAME.xpr"

# ── 1. Detectar Vivado ────────────────────────────────────────────────────────
find_vivado_root() {
    # a) Ya en PATH — devuelve el directorio de instalación
    if command -v vivado &>/dev/null; then
        # settings64.sh está dos niveles arriba del binario (.../bin/vivado)
        local bin_dir
        bin_dir="$(dirname "$(command -v vivado)")"
        echo "$(cd "$bin_dir/.." && pwd)"
        return 0
    fi

    # b) Variable de entorno VIVADO_ROOT definida por el usuario
    if [[ -n "${VIVADO_ROOT:-}" && -f "$VIVADO_ROOT/settings64.sh" ]]; then
        echo "$VIVADO_ROOT"
        return 0
    fi

    # c) Rutas de instalación fijas comunes
    local fixed=(
        "/tools/Xilinx/Vivado/2024.1"
        "/opt/Xilinx/Vivado/2024.1"
        "/opt/xilinx/Vivado/2024.1"
        "$HOME/Xilinx/Vivado/2024.1"
        "$HOME/tools/Xilinx/Vivado/2024.1"
    )
    for r in "${fixed[@]}"; do
        if [[ -f "$r/settings64.sh" ]]; then
            echo "$r"
            return 0
        fi
    done

    # d) Buscar en medios montados (llave maya u otros)
    for mount_base in /media /mnt /run/media; do
        [[ -d "$mount_base" ]] || continue
        local found
        found="$(find "$mount_base" -maxdepth 7 -name "settings64.sh" \
                     -path "*/Vivado/*" 2>/dev/null | head -1)"
        if [[ -n "$found" ]]; then
            echo "$(dirname "$found")"
            return 0
        fi
    done

    echo ""
    return 1
}

VIVADO_ROOT="$(find_vivado_root || true)"

if [[ -z "$VIVADO_ROOT" ]]; then
    echo ""
    echo "ERROR: No se encontró Vivado 2024.1."
    echo ""
    echo "Opciones para resolverlo:"
    echo "  1) Carga el entorno manualmente y vuelve a correr:"
    echo "       source /ruta/a/Vivado/2024.1/settings64.sh"
    echo "       bash scripts/build_all.sh"
    echo ""
    echo "  2) Exporta la raíz de instalación antes de correr:"
    echo "       export VIVADO_ROOT=/ruta/a/Vivado/2024.1"
    echo "       bash scripts/build_all.sh"
    echo ""
    exit 1
fi

# Cargar entorno solo si vivado aún no está en PATH
if ! command -v vivado &>/dev/null; then
    echo "INFO: Cargando entorno desde $VIVADO_ROOT/settings64.sh ..."
    # shellcheck source=/dev/null
    source "$VIVADO_ROOT/settings64.sh"
fi

echo "INFO: $(vivado -version 2>/dev/null | head -1)"
echo "INFO: Repo: $REPO_ROOT"
mkdir -p "$REPO_ROOT/logs"

# ── 2. Crear proyecto Vivado desde HoG si no existe ──────────────────────────
if [[ ! -f "$XPR" ]]; then
    echo ""
    echo "INFO: Proyecto no encontrado — regenerando desde HoG ..."
    mkdir -p "$REPO_ROOT/Projects"
    cd "$REPO_ROOT"
    vivado -mode batch -notrace \
        -source "$REPO_ROOT/Hog/Tcl/create_project.tcl" \
        -tclargs "$PROJECT_NAME" "$REPO_ROOT" \
        -log  "$REPO_ROOT/logs/create_project.log" \
        -journal "$REPO_ROOT/logs/create_project.jou"
    echo "INFO: Proyecto creado en Projects/$PROJECT_NAME/"
else
    echo "INFO: Proyecto ya existe — omitiendo creación."
fi

# ── 3. Síntesis + implementación + bitstream ─────────────────────────────────
echo ""
echo "INFO: Lanzando síntesis, implementación y generación de bitstream ..."
cd "$REPO_ROOT"
vivado -mode batch -notrace \
    -source "$REPO_ROOT/scripts/build_bitstream.tcl" \
    -log    "$REPO_ROOT/logs/build_bitstream.log" \
    -journal "$REPO_ROOT/logs/build_bitstream.jou"

echo ""
echo "DONE: Bitstream listo en bin/build_latest/top_pong_project.bit"
