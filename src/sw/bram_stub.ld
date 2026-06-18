ENTRY(_stub_entry)
MEMORY { bram : ORIGIN = 0x0, LENGTH = 0x10000 }
SECTIONS {
    .text : { *(.text*) } > bram
}
