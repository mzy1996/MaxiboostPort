/* SPDX-License-Identifier: GPL-2.0-or-later
   The guest's thread-local storage on Windows.

   The guest's code reads its thread pointer with `mov rax, gs:[0]`: on Linux the runtime points the
   GS base at the guest's TCB (src/runtime_thread.c), which the kernel makes impossible on
   Windows (the GS base belongs to the kernel and cannot be set by the program).

   The thread environment block has a word of its own for the program
   (ArbitraryUserPointer, at GS:[0x28]); the runtime keeps the guest's TCB pointer in it,
   and the loader rewrites the guest's read to that word (probe.c), so the guest sees
   exactly what it sees on Linux. The word is per thread. */
#ifndef BB_RUNTIME_TLS_H
#define BB_RUNTIME_TLS_H

#ifdef _WIN32

#include <stdint.h>

/* The guest's TCB pointer of the calling thread. */
void *runtime_tls_get_tcb(void);
/* Sets the guest's TCB pointer of the calling thread. */
void runtime_tls_set_tcb(void *tcb);
/* Reserves the slot (the runtime calls it once, at the start). */
void runtime_tls_init(void);
/* The offset the loader's rewrite reads (the runtime tells the loader before the image is
 * written). */
uint32_t runtime_tls_guest_index(void);

#endif

#endif /* BB_RUNTIME_TLS_H */
