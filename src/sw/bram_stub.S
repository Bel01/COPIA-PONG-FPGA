/* bram_stub.S — bootloader mínimo en LMB BRAM (0x0)
 * Espera ~1 s para calibración MIG, luego salta al pong_app en DDR2.
 * No usa stack ni BSS — solo registros temporales.
 */
    .section ".text"
    .global _stub_entry

_stub_entry:
    /* Bucle de espera: 50 M iter × ~2 ciclos / 100 MHz ≈ 1 s */
    li      t0, 50000000
.Lwait:
    addi    t0, t0, -1
    bnez    t0, .Lwait

    /* Saltar al entry point de pong_app.elf en DDR2 */
    li      t0, 0x80000000
    jr      t0
