/* SPDX-License-Identifier: GPL-2.0-or-later
   The guest's thread-local storage on Windows.

   The guest's code reads its thread pointer with `mov rax, gs:[0]`: on Linux the runtime
   points the GS base at the guest's TCB (src/runtime_thread.c), which the kernel makes
   impossible on Windows (the GS base belongs to the kernel and cannot be set by the
   program). Windows' GS:[0] is the kernel's exception list, so the guest read whatever
   was there (nothing) and dereferenced it.

   The thread environment block has a word that belongs to the program
   (ArbitraryUserPointer, at GS:[0x28]); the runtime keeps the guest's TCB pointer in
   it, and the loader rewrites the guest's `mov rax, gs:[0]` into `mov rax, gs:[0x28]`,
   so the guest sees exactly what it sees on Linux.

   The word is per thread, so every guest thread has its own TCB, as it should. */
#ifdef _WIN32

#include <windows.h>
#include <stdint.h>

/* The offset of ArbitraryUserPointer in the thread environment block (GS:[0x28]).
 * The block's first words are the kernel's (the exception list, the stack bounds);
 * this one is the program's. */
#define TEB_ARBITRARY_USER 0x28

/* The guest's TCB pointer of the calling thread. */
void *runtime_tls_get_tcb(void) {
    uint64_t value;
    __asm__ volatile("mov %%gs:%1, %0"
                     : "=r"(value)
                     : "m"(*(volatile char *)(uintptr_t)TEB_ARBITRARY_USER));
    return (void *)(uintptr_t)value;
}

void runtime_tls_set_tcb(void *tcb) {
    uint64_t value=(uint64_t)(uintptr_t)tcb;
    __asm__ volatile("mov %0, %%gs:%1"
                     :
                     : "r"(value), "m"(*(volatile char *)(uintptr_t)TEB_ARBITRARY_USER)
                     : "memory");
}

/* The offset the loader's rewrite reads (the runtime tells the loader before the image
 * is written). */
uint32_t runtime_tls_guest_index(void) {
    return TEB_ARBITRARY_USER;
}

void runtime_tls_init(void) {
    /* Nothing to reserve: the word is a part of the thread's own block. */
}

#endif
