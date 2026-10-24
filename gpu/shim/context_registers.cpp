// SPDX-License-Identifier: GPL-2.0-or-later
// The registers of an interrupted thread (gpu/shim/host_services.h): the fault handler and
// the page tracking read them and write them back. On Linux the signal handler gives a
// ucontext, on Windows the exception handler a CONTEXT.
#include "host_services.h"

#ifdef _WIN32

uint64_t ContextGet(const void* context, int reg) {
    if (!context) {
        return 0;
    }
    const auto* c = static_cast<const CONTEXT*>(context);
    switch (reg) {
    case RegRax: return c->Rax;
    case RegRbx: return c->Rbx;
    case RegRcx: return c->Rcx;
    case RegRdx: return c->Rdx;
    case RegRsi: return c->Rsi;
    case RegRdi: return c->Rdi;
    case RegRbp: return c->Rbp;
    case RegRsp: return c->Rsp;
    case RegRip: return c->Rip;
    case RegR14: return c->R14;
    case RegR15: return c->R15;
    case RegFlags: return c->EFlags;
    default: return 0;
    }
}

void ContextSet(void* context, int reg, uint64_t value) {
    if (!context) {
        return;
    }
    auto* c = static_cast<CONTEXT*>(context);
    switch (reg) {
    case RegRax: c->Rax = value; break;
    case RegRbx: c->Rbx = value; break;
    case RegRcx: c->Rcx = value; break;
    case RegRdx: c->Rdx = value; break;
    case RegRsi: c->Rsi = value; break;
    case RegRdi: c->Rdi = value; break;
    case RegRbp: c->Rbp = value; break;
    case RegRsp: c->Rsp = value; break;
    case RegRip: c->Rip = value; break;
    case RegR14: c->R14 = value; break;
    case RegR15: c->R15 = value; break;
    case RegFlags: c->EFlags = static_cast<DWORD>(value); break;
    default: break;
    }
}

#else

uint64_t ContextGet(const void* context, int reg) {
    if (!context) {
        return 0;
    }
    const auto* uc = static_cast<const ucontext_t*>(context);
    switch (reg) {
    case RegRax: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RAX]);
    case RegRbx: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBX]);
    case RegRcx: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RCX]);
    case RegRdx: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RDX]);
    case RegRsi: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSI]);
    case RegRdi: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RDI]);
    case RegRbp: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
    case RegRsp: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
    case RegRip: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    case RegR14: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R14]);
    case RegR15: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_R15]);
    case RegFlags: return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_EFL]);
    default: return 0;
    }
}

void ContextSet(void* context, int reg, uint64_t value) {
    if (!context) {
        return;
    }
    auto* uc = static_cast<ucontext_t*>(context);
    switch (reg) {
    case RegRax: uc->uc_mcontext.gregs[REG_RAX] = static_cast<greg_t>(value); break;
    case RegRbx: uc->uc_mcontext.gregs[REG_RBX] = static_cast<greg_t>(value); break;
    case RegRcx: uc->uc_mcontext.gregs[REG_RCX] = static_cast<greg_t>(value); break;
    case RegRdx: uc->uc_mcontext.gregs[REG_RDX] = static_cast<greg_t>(value); break;
    case RegRsi: uc->uc_mcontext.gregs[REG_RSI] = static_cast<greg_t>(value); break;
    case RegRdi: uc->uc_mcontext.gregs[REG_RDI] = static_cast<greg_t>(value); break;
    case RegRbp: uc->uc_mcontext.gregs[REG_RBP] = static_cast<greg_t>(value); break;
    case RegRsp: uc->uc_mcontext.gregs[REG_RSP] = static_cast<greg_t>(value); break;
    case RegRip: uc->uc_mcontext.gregs[REG_RIP] = static_cast<greg_t>(value); break;
    case RegR14: uc->uc_mcontext.gregs[REG_R14] = static_cast<greg_t>(value); break;
    case RegR15: uc->uc_mcontext.gregs[REG_R15] = static_cast<greg_t>(value); break;
    case RegFlags: uc->uc_mcontext.gregs[REG_EFL] = static_cast<greg_t>(value); break;
    default: break;
    }
}

#endif

uint64_t ContextFlags(const void* context) {
    return ContextGet(context, static_cast<int>(RegFlags));
}

void ContextSetFlags(void* context, uint64_t flags) {
    ContextSet(context, static_cast<int>(RegFlags), flags);
}

// The three registers the fault handling reads the most. The code above asks for them by
// name, so that the two hosts share it.
uint64_t ContextRip(const void* context) { return ContextGet(context, static_cast<int>(RegRip)); }
uint64_t ContextRsp(const void* context) { return ContextGet(context, static_cast<int>(RegRsp)); }
uint64_t ContextRbp(const void* context) { return ContextGet(context, static_cast<int>(RegRbp)); }
