# ping_pong_game_project

Sistema embebido multijugador en Nexys A7-100T. Implementa el juego Pong con un SoC MicroBlaze V (RISC-V), framebuffer VGA en BRAM, y soporte para carga de sprites desde microSD.

---

## Arquitectura actual

```
microSD ──SPI──► MicroBlaze V ──AXI──► BRAM framebuffer (Port A)
                                              │
                                         Port B (nativo)
                                              │
                                         VGA 640×480 @ 60 Hz ──► Monitor
```

| Componente | Detalles |
|---|---|
| CPU | MicroBlaze V (RISC-V rv32i), corre desde **LMB BRAM** (0x0, 64 KB) |
| Framebuffer | BRAM True Dual Port, 640×480×4-bit = 38 400 palabras × 32-bit |
| Paleta | 16 colores (4-bit índice → RGB 12-bit, hardcoded en HDL) |
| VGA | 640×480 @ 60 Hz, `pixel_idx = v_count × 640 + h_count` |
| DDR2 | Disponible (0x80000000, 128 MB) — **pendiente integrar como buffer de assets** |
| microSD | AXI Quad SPI 1 — driver SPI manual en firmware (pendiente depurar AXI fault) |
| SPI inter-FPGA | AXI Quad SPI 0 — modo maestro para 2 jugadores |

---

## Estado funcional

- [x] Framebuffer BRAM operativo (franjas de test confirmadas en monitor)
- [x] Juego Pong: menú, jugando, pausa, game over
- [x] Botones (BTNL/R/U/D/C) con debounce y sincronización
- [x] LEDs reflejan marcador
- [x] Modo 1P (IA) y 2P por switch
- [ ] Sprites desde SD (sd_ok = 0, carga desactivada)
- [ ] DDR2 como buffer de assets

---

## Setup para compañeros (primera vez)

El repo incluye el XSA (`top_pong_project.xsa`) y las fuentes (`src/sw/`), pero **no** el workspace de Vitis, ya que contiene rutas absolutas y archivos generados. Un script recrea todo automáticamente:

```bash
# Requiere: Vivado 2024.1 en /tools/Xilinx  +  Vitis 2024.1 en /tools/Xilinx2
source /tools/Xilinx2/Vitis/2024.1/settings64.sh
xsct scripts/create_vitis_app.tcl $(pwd) $(pwd)/../pong_workspace
```

Esto crea `../pong_workspace/` con la plataforma BSP completa y genera `pong_app.elf`.
Si Vitis está en otra ruta, ajusta `settings64.sh` y el argumento de workspace.

> **¿Por qué no está el workspace en el repo?**  
> Los archivos generados contienen rutas absolutas (`/home/usuario/...`) que rompen
> la compilación en otra máquina. El XSA sí está versionado porque es el contrato
> hardware–software y no tiene rutas absolutas.

---

## Cómo programar (JTAG)

### 1. Sintetizar hardware (solo si cambiaste HDL o BD)

```bash
source /tools/Xilinx/Vivado/2024.1/settings64.sh
vivado -mode batch -source scripts/build_bitstream.tcl \
       -log logs/build_bitstream.log
```

El bitstream queda en `bin/pong_project-v<TAG>/`.

### 2. Compilar firmware

Si ya tienes el workspace creado (ver Setup arriba):

```bash
source /tools/Xilinx2/Vitis/2024.1/settings64.sh
cd ../pong_workspace/pong_app/build
make -j4
# El error de mb-size al final es cosmético; pong_app.elf se genera igual.
```

### 3. Programar FPGA y correr

```bash
source /tools/Xilinx/Vivado/2024.1/settings64.sh
xsdb scripts/program_and_run.tcl
```

---

## Nota sobre DDR2

Se intentó correr el ELF desde DDR2 (cambiando `lscript.ld` de `lmb_bram_0` a `mig_0`).
El ELF enlaza correctamente con entry point en `0x80000000`, pero la pantalla queda negra.

**Causa probable:** el MicroBlaze V necesita que la DDR2 esté inicializada (calibración MIG)
antes de que el CPU pueda ejecutar desde ahí. Al cargar por JTAG sin pasar por el bootloader,
la calibración puede no estar completa en el momento que `con` arranca el CPU.

**Para revertir** (volver a LMB BRAM):
```bash
# En src/sw/lscript.ld, todas las secciones deben apuntar a lmb_bram_0:
sed -i 's/> mig_0/> lmb_bram_0/g' src/sw/lscript.ld
# Recompilar y recargar
```

---

## Estructura del repositorio

```
src/
  hdl/          — top_pong_project.v, vga_controller.v, etc.
  sw/
    pong.c      — firmware completo (juego + driver SD + renderer)
    lscript.ld  — script de enlace (CPU → LMB BRAM, FB → axi_bram_0)
BD/
  microblaze_v.bd          — Block Design (MicroBlaze V + periféricos)
  ip/                      — IPs generados por Vivado
constraints/
  nexys_a7_100t.xdc        — restricciones de pines
scripts/
  build_bitstream.tcl      — síntesis + implementación + bitstream
  program_and_run.tcl      — programar FPGA + cargar ELF por JTAG
  bd_update_framebuffer_spi.tcl — actualizar BD (referencia)
bin/                       — bitstreams generados (HoG)
```
