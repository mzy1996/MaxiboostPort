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
#ifdef _WIN32

#include <windows.h>
#include <stdint.h>

/* The TLS index of the guest's TCB pointer. */
static DWORD tcb_index = TLS_OUT_OF_INDEXES;

/* The index the loader's rewrite reads: the program's first dynamic slot. The runtime tells
 * the loader which one it is (the loader writes it into the guest image). */
static uint32_t guest_index = 0;

void runtime_tls_init(void) {
    tcb_index = TlsAlloc();
    if (tcb_index == TLS_OUT_OF_INDEXES) {
        tcb_index = 0;
    }
    /* The loader's rewrite reads the slot at this index of the thread's array. */
    guest_index = tcb_index;
}

/* The index the loader's rewrite reads (the runtime tells the loader before the image is
 * written). */
uint32_t runtime_tls_guest_index(void) {
    return guest_index;
}

/* The guest's TCB pointer of the calling thread. */
void *runtime_tls_get_tcb(void) {
    return TlsGetValue(tcb_index);
}

void runtime_tls_set_tcb(void *tcb) {
    TlsSetValue(tcb_index, tcb);
}

#endif
