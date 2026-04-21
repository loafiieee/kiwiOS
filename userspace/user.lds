OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x0000000000400000;

    .text ALIGN(0x1000) : {
        *(.text .text.*)
    } :text

    .rodata ALIGN(0x1000) : {
        *(.rodata .rodata.*)
    } :rodata

    .data ALIGN(0x1000) : {
        *(.data .data.*)
    } :data

    .bss ALIGN(0x1000) : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame*)
        *(.note*)
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.gnu*)
        *(.got*)
        *(.plt*)
    }
}
