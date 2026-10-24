/* SPDX-License-Identifier: GPL-2.0-or-later
 * The hand-written assembly of the runtime, in the syntax both hosts' assemblers take.
 *
 * The labels and the directives differ: the GNU assembler (Linux) wants .globl, .type and
 * .size, the one MinGW uses (the COFF one) wants .def and no .size. The macros below pick
 * what the host's assembler understands. */
#ifndef BB_ASM_H
#define BB_ASM_H

#ifdef _WIN32
/* MinGW's assembler: .globl is understood, but .type and .size are not (the COFF object
 * format has no symbol types). The symbol is exported by the linker from the .def the
 * compiler writes for the declaration. */
#define BB_ASM_GLOBAL(name) ".globl " name "\n"
#define BB_ASM_TYPE(name) ""
#define BB_ASM_SIZE(name) ""
#define BB_ASM_TEXT ".text\n"
#else
#define BB_ASM_GLOBAL(name) ".globl " name "\n"
#define BB_ASM_TYPE(name) ".type " name ",@function\n"
#define BB_ASM_SIZE(name) ".size " name ",.-" name "\n"
#define BB_ASM_TEXT ".text\n"
#endif

#endif
