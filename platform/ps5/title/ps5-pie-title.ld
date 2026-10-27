/*
 * Title PIE layout: the boilerplate's ps5-pie.ld (ps5-native-app-boilerplate,
 * Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later) plus the
 * __eh_frame[_hdr]_{start,end} bounds psbc/ps5-pie-unwind.ld adds for
 * libunwind, with one change: a whole page is reserved between the RELRO
 * metadata and the data segment.
 *
 * Why the reserve (platform/ps5/viewer/Makefile links through this file):
 * ps5-native-tool link writes the PS5 process-parameter block (0x60 B of
 * process parameters plus ~0x2c0 B of parameter blocks) into the padding that
 * `ALIGN(MAXPAGESIZE)` leaves between the last RELRO/rodata section and the
 * page-aligned .dynamic. sce_module_writer.cpp puts it at
 * align_up(max(relro content end, ro end), 8) and requires the result to end
 * before .dynamic's address. That padding is whatever is left in the last
 * page (0..0x3fff B), so a binary links fine until its RELRO content happens
 * to end near a page boundary: the merged W23 viewer came within 8 B of the
 * 0x320 B the converter needs. The page below makes the headroom at least
 * MAXPAGESIZE (16 KiB on PS5) for any binary, instead of luck.
 *
 * Keep in sync with $TITLE_TOOLING/native/ps5-pie.ld: everything except the
 * reserved dot advance is that file verbatim.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS {
    text PT_LOAD FLAGS (0x5);
    ro PT_LOAD FLAGS (0x4);
    relro PT_LOAD FLAGS (0x6);
    data PT_LOAD FLAGS (0x6);
    dynamic PT_DYNAMIC FLAGS (0x6);
}

SECTIONS {
    .text 0 : ALIGN(CONSTANT(MAXPAGESIZE)) {
        QUAD(0xcccccccccccccccc)
        QUAD(0xcccccccccccccccc)
        *(.text .text.*)
        *(.plt .plt.*)
    } : text

    .eh_frame_hdr : ALIGN(CONSTANT(MAXPAGESIZE)) {
        KEEP(*(.eh_frame_hdr))
    } : ro
    .eh_frame : { KEEP(*(.eh_frame)) } : ro
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } : ro
    .rodata : { *(.rodata .rodata.*) } : ro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
    } : relro
    .got : { *(.got) } : relro
    .got.plt : { *(.got.plt) } : relro
    .preinit_array : {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } : relro
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } : relro
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } : relro

    /* Kept only long enough for the converter to read and rewrite them. */
    .dynsym : { *(.dynsym) } : relro
    .gnu.hash : { *(.gnu.hash) } : relro
    .dynstr : { *(.dynstr) } : relro
    .rela.dyn : { *(.rela.dyn) } : relro
    .rela.plt : { *(.rela.plt) } : relro

    /* Reserved for the process parameters the title converter writes between
       the last section above and .dynamic; see this file's header. */
    . = . + CONSTANT(MAXPAGESIZE);
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : {
        *(.dynamic)
    } : data : dynamic
    .data : { *(.data .data.*) } : data
    .tdata : { *(.tdata .tdata.*) } : data
    .tbss : { *(.tbss .tbss.*) } : data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } : data
}

/* psbc/ps5-pie-unwind.ld: ACO's libunwind finds the tables through these. */
PROVIDE(__eh_frame_start = ADDR(.eh_frame));
PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
