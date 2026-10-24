/* SPDX-License-Identifier: GPL-2.0-or-later
   The guest's thread-local storage on Windows.

   The guest's code reads its TCB through `mov rax, gs:[0]`: on Linux the runtime points the
   GS base at the guest's TCB (src/runtime_thread.c), which the kernel makes impossible on
   Windows (the GS base belongs to the kernel and cannot be set by the program).

   Windows' own TLS is used instead. The thread's array of slots is at GS:[0x58], and the
   loader rewrites the guest's `mov rax, fs:[0]` into a load of the slot the runtime
   reserves. The runtime keeps the guest's TCB pointer in that slot, so the guest sees
   exactly what it sees on Linux.

   The slot is a TLS index of the program (TlsAlloc). The guest's rewrite reads the array
   directly (GS:[0x58] plus the index), so the index has to be known when the image is
   written: the runtime passes it to the loader (scripts/link_libc.py). */
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
/* The index of the slot the loader's rewrite reads (the runtime tells the loader before the
 * image is written). */
uint32_t runtime_tls_guest_index(void);

#endif

#endif /* BB_RUNTIME_TLS_H */
