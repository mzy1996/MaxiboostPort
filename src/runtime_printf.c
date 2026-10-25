/* The guest's printf family ------------------------------------------------
 * The game prints through the C library of the system (libSceLibcInternal), which
 * the runtime stands in for. The guest speaks the System V ABI, so its va_list is
 * the System V one: four fields, where the offsets say which saved register holds
 * the next argument. The format string and the strings it names live in the guest's
 * memory, which the runtime shares, so they are read directly.
 *
 * The host's own variadic functions use another convention (the Windows one), and
 * the two do not mix: a va_start in a function the guest calls with the System V
 * convention reads the wrong place and the program stops there. The calls with a
 * fixed number of arguments therefore go through a small piece of assembly that
 * gathers the registers the way the System V convention wants them and builds the
 * va_list there; the calls that come with a va_list are read as they are.
 */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The System V va_list: where the next argument is. */
typedef struct GuestVaList {
    unsigned gp_offset, fp_offset;
    void *overflow_arg_area, *reg_save_area;
} GuestVaList;

/* How much room a formatted text gets. The guest's own buffers are of its own
 * size, which the runtime cannot know, so the text is put together here first. */
#define PRINT_ROOM (1u<<18)
/* The largest text that is put into a buffer whose size the game did not say. */
#define PRINT_BUFFER_MAX (64u*1024)

/* The calls with a fixed number of arguments: the trampoline tells which one it is
 * by a number, and the arguments are in the registers it saved. */
enum {
    GUEST_PRINT_STDOUT = 1,
    GUEST_PRINT_STREAM,
    GUEST_PRINT_BUFFER,
    GUEST_PRINT_BUFFER_SIZED
};

/* The next integer argument: from the saved registers while there are any, from the
 * stack after that. */
static uint64_t guest_va_int(GuestVaList *ap) {
    if (ap->gp_offset<=48-8) {
        uint64_t v=0;
        memcpy(&v,(const char *)ap->reg_save_area+ap->gp_offset,sizeof(v));
        ap->gp_offset+=8;
        return v;
    }
    uint64_t v=0;
    memcpy(&v,ap->overflow_arg_area,sizeof(v));
    ap->overflow_arg_area=(char *)ap->overflow_arg_area+8;
    return v;
}
/* The next floating point argument: the same, with its own registers. */
static double guest_va_double(GuestVaList *ap) {
    if (ap->fp_offset<=176-16) {
        double v=0;
        memcpy(&v,(const char *)ap->reg_save_area+ap->fp_offset,sizeof(v));
        ap->fp_offset+=16;
        return v;
    }
    double v=0;
    memcpy(&v,ap->overflow_arg_area,sizeof(v));
    ap->overflow_arg_area=(char *)ap->overflow_arg_area+8;
    return v;
}
/* A string of the guest: it is copied here, so that a long one cannot run over the
 * room the format has. */
static void guest_copy_text(char *out, size_t room, const char *from) {
    size_t i=0;
    if (!room) return;
    if (from) while (i+1<room && from[i]) { out[i]=from[i]; ++i; }
    out[i]='\0';
}

/* One conversion: the flags, the width, the precision and the length modifier are
 * put together for the host's own formatter, and the value is taken from the
 * guest's arguments. The letter says what kind of value it is. */
static size_t guest_format_one(char *out, size_t room, const char **fmt, GuestVaList *ap,
                                size_t *count) {
    char flags[16], width[16], precision[16];
    size_t f=0, w=0, p=0;
    char length='\0';
    flags[0]='\0'; width[0]='\0'; precision[0]='\0';
    /* The flags. */
    for (;;) {
        const char c=(*fmt)[0];
        if (c!='-'&&c!='+'&&c!=' '&&c!='#'&&c!='0') break;
        if (f+1<sizeof(flags)) flags[f++]=c;
        ++*fmt;
    }
    flags[f]='\0';
    /* The width: a star means the argument is in the list. */
    if ((*fmt)[0]=='*') {
        const int value=(int)guest_va_int(ap);
        ++*fmt;
        if (value<0) {
            if (f+1<sizeof(flags)) flags[f++]='-';
            w=(size_t)snprintf(width,sizeof(width),"%d",-value);
        } else w=(size_t)snprintf(width,sizeof(width),"%d",value);
    } else {
        while ((*fmt)[0]>='0'&&(*fmt)[0]<='9'&&w+1<sizeof(width)) width[w++]=(*fmt)[0], ++*fmt;
        width[w]='\0';
    }
    /* The precision: the same. */
    if ((*fmt)[0]=='.') {
        ++*fmt;
        precision[p++]='.';
        if ((*fmt)[0]=='*') {
            const int value=(int)guest_va_int(ap);
            ++*fmt;
            p+=(size_t)snprintf(precision+p,sizeof(precision)-p,"%d",value);
        } else {
            while ((*fmt)[0]>='0'&&(*fmt)[0]<='9'&&p+1<sizeof(precision)) precision[p++]=(*fmt)[0], ++*fmt;
        }
        precision[p]='\0';
    }
    /* The length modifier: the value is cut to it, so that the host's formatter
     * does not have to know it. */
    if ((*fmt)[0]=='h'||(*fmt)[0]=='l'||(*fmt)[0]=='z'||(*fmt)[0]=='j'||
        (*fmt)[0]=='t'||(*fmt)[0]=='L'||(*fmt)[0]=='q') {
        length=(*fmt)[0];
        ++*fmt;
        if (length=='h'&&(*fmt)[0]=='h') { length='H'; ++*fmt; }
        else if (length=='l'&&(*fmt)[0]=='l') { length='q'; ++*fmt; }
    }
    const char letter=(*fmt)[0];
    if (!letter) {
        if (room) snprintf(out,room,"%s%s%s",flags,width,precision);
        return 0;
    }
    ++*fmt;
    size_t used=0;
    switch (letter) {
    case 'd': case 'i': {
        long long v=(long long)guest_va_int(ap);
        if (length=='H') v=(signed char)v;
        else if (length=='h') v=(short)v;
        used=(size_t)snprintf(out,room,"%s%s%slld",flags,width,precision,v);
        break; }
    case 'u': case 'x': case 'X': case 'o': {
        unsigned long long v=(unsigned long long)guest_va_int(ap);
        if (length=='H') v=(unsigned char)v;
        else if (length=='h') v=(unsigned short)v;
        used=(size_t)snprintf(out,room,"%s%s%sll%c",flags,width,precision,letter,v);
        break; }
    case 'c': {
        const int v=(int)guest_va_int(ap);
        used=(size_t)snprintf(out,room,"%s%sc",flags,width,(char)v);
        break; }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        const double v=guest_va_double(ap);
        used=(size_t)snprintf(out,room,"%s%s%s%c",flags,width,precision,letter,v);
        break; }
    case 'p': {
        const void *v=(const void *)(uintptr_t)guest_va_int(ap);
        used=(size_t)snprintf(out,room,"%s%s%sp",flags,width,v);
        break; }
    case 's': {
        char copy[1024];
        guest_copy_text(copy,sizeof(copy),(const char *)(uintptr_t)guest_va_int(ap));
        used=(size_t)snprintf(out,room,"%s%s%ss",flags,width,precision,copy);
        break; }
    case 'n': {
        int *where=(int *)(uintptr_t)guest_va_int(ap);
        if (where) {
            runtime_memory_prepare_cpu_write((uintptr_t)where,sizeof(int));
            *where=(int)*count;
            runtime_memory_note_cpu_write((uintptr_t)where,sizeof(int));
        }
        used=0;
        break; }
    case '%':
        used=(size_t)snprintf(out,room,"%%");
        break;
    default:
        /* An unknown conversion: it is copied as it is. */
        used=(size_t)snprintf(out,room,"%s%s%s%c",flags,width,precision,letter);
        break;
    }
    return used;
}
/* A whole format: the text is put together piece by piece. */
static int guest_format(char *out, size_t room, const char *fmt, GuestVaList *ap) {
    size_t used=0;
    if (!fmt) { if (room) out[0]='\0'; return 0; }
    while (*fmt) {
        if (*fmt!='%') {
            if (used+1<room) out[used]=*fmt;
            used++; fmt++;
            continue;
        }
        fmt++;
        if (*fmt=='%') {
            if (used+1<room) out[used]='%';
            used++; fmt++;
            continue;
        }
        char piece[2048];
        piece[0]='\0';
        const size_t length=guest_format_one(piece,sizeof(piece),&fmt,ap,&used);
        const size_t take=length<sizeof(piece)-1?length:sizeof(piece)-1;
        if (used+take+1<room) memcpy(out+used,piece,take);
        used+=take;
    }
    if (room) out[used<room?used:room-1]='\0';
    return (int)used;
}

/* The helper the calls with a va_list and the trampoline share. */
static int guest_print_va(int kind, const char *fmt, GuestVaList *ap,
                          void *stream, char *buffer, size_t room) {
    char *text=(char *)malloc(PRINT_ROOM);
    if (!text) return -1;
    const int n=guest_format(text,PRINT_ROOM,fmt,ap);
    switch (kind) {
    case GUEST_PRINT_STREAM:
        /* The guest's stream is a file of its own; the text goes to the runtime's
         * output, which is where the game's log is read. */
        fwrite(text,1,n>0?(size_t)n:0,stdout);
        fflush(stdout);
        break;
    case GUEST_PRINT_BUFFER: {
        size_t take=n>0?(size_t)n:0;
        if (take>PRINT_BUFFER_MAX) take=PRINT_BUFFER_MAX;
        runtime_memory_prepare_cpu_write((uintptr_t)buffer,take);
        memcpy(buffer,text,take);
        runtime_memory_note_cpu_write((uintptr_t)buffer,take);
        break; }
    case GUEST_PRINT_BUFFER_SIZED: {
        size_t take=n>0?(size_t)n:0;
        if (take>room) take=room;
        runtime_memory_prepare_cpu_write((uintptr_t)buffer,take);
        memcpy(buffer,text,take);
        runtime_memory_note_cpu_write((uintptr_t)buffer,take);
        if (room) buffer[take<room?take:room-1]='\0';
        break; }
    default:
        fwrite(text,1,n>0?(size_t)n:0,stdout);
        fflush(stdout);
        break;
    }
    free(text);
    return n;
}

/* The calls that come with a va_list: the guest's own va_list is read as it is. */
static ABI int guest_vprintf(const char *fmt, GuestVaList *ap) {
    return guest_print_va(GUEST_PRINT_STDOUT,fmt,ap,NULL,NULL,0);
}
static ABI int guest_vfprintf(void *stream, const char *fmt, GuestVaList *ap) {
    return guest_print_va(GUEST_PRINT_STREAM,fmt,ap,stream,NULL,0);
}
static ABI int guest_vsprintf(char *buffer, const char *fmt, GuestVaList *ap) {
    return guest_print_va(GUEST_PRINT_BUFFER,fmt,ap,NULL,buffer,0);
}
static ABI int guest_vsnprintf(char *buffer, size_t room, const char *fmt, GuestVaList *ap) {
    return guest_print_va(GUEST_PRINT_BUFFER_SIZED,fmt,ap,NULL,buffer,room);
}

/* The core the trampoline calls: the arguments that came with the call are in the
 * registers it saved, the rest is in the va_list it built. */
ABI int bb_guest_printf_core(int kind, GuestVaList *ap);
extern void bb_guest_printf(void);
extern void bb_guest_fprintf(void);
extern void bb_guest_sprintf(void);
extern void bb_guest_snprintf(void);

ABI int bb_guest_printf_core(int kind, GuestVaList *ap) {
    const uint64_t *saved=(const uint64_t *)ap->reg_save_area;
    switch (kind) {
    case GUEST_PRINT_STREAM:
        return guest_print_va(kind,(const char *)saved[1],ap,(void *)saved[0],NULL,0);
    case GUEST_PRINT_BUFFER:
        return guest_print_va(kind,(const char *)saved[1],ap,NULL,(char *)saved[0],0);
    case GUEST_PRINT_BUFFER_SIZED:
        return guest_print_va(kind,(const char *)saved[2],ap,NULL,(char *)saved[0],(size_t)saved[1]);
    default:
        return guest_print_va(kind,(const char *)saved[0],ap,NULL,NULL,0);
    }
}

/* The entry of the trampoline: the arguments are in the registers the System V
 * convention uses (the first one in rdi, the next ones in rsi, rdx, rcx, r8 and r9;
 * the floating point ones in xmm0 to xmm7), and the ones that do not fit on the
 * stack above the return address. The registers are saved the way the va_list
 * expects them (the six general ones first, the eight floating point ones after
 * them), and the va_list is built: the offsets say which saved register holds the
 * next argument, and the stack arguments follow the return address. */
#define GUEST_PRINTF_ENTRY(symbol, named, number)                                 \
__asm__(".text\n"                                                               \
        ".globl " #symbol "\n"                                                  \
        #symbol ":\n"                                                           \
        "  push %rbp\n"                                                         \
        "  mov %rsp, %rbp\n"                                                    \
        "  sub $0x120, %rsp\n"                                                  \
        "  mov %rdi, -0x120(%rbp)\n"                                            \
        "  mov %rsi, -0x118(%rbp)\n"                                            \
        "  mov %rdx, -0x110(%rbp)\n"                                            \
        "  mov %rcx, -0x108(%rbp)\n"                                            \
        "  mov %r8,  -0x100(%rbp)\n"                                            \
        "  mov %r9,  -0xf8(%rbp)\n"                                             \
        "  movaps %xmm0, -0xf0(%rbp)\n"                                         \
        "  movaps %xmm1, -0xe0(%rbp)\n"                                         \
        "  movaps %xmm2, -0xd0(%rbp)\n"                                         \
        "  movaps %xmm3, -0xc0(%rbp)\n"                                         \
        "  movaps %xmm4, -0xb0(%rbp)\n"                                         \
        "  movaps %xmm5, -0xa0(%rbp)\n"                                         \
        "  movaps %xmm6, -0x90(%rbp)\n"                                         \
        "  movaps %xmm7, -0x80(%rbp)\n"                                         \
        "  movl $" #named "*8, -0x40(%rbp)\n"                                   \
        "  movl $48, -0x3c(%rbp)\n"                                             \
        "  leaq 16(%rbp), %rax\n"                                               \
        "  movq %rax, -0x38(%rbp)\n"                                            \
        "  leaq -0x120(%rbp), %rax\n"                                            \
        "  movq %rax, -0x30(%rbp)\n"                                            \
        "  movl $" #number ", %edi\n"                                              \
        "  leaq -0x40(%rbp), %rsi\n"                                             \
        "  call bb_guest_printf_core\n"                                          \
        "  leave\n"                                                             \
        "  ret\n");

GUEST_PRINTF_ENTRY(bb_guest_printf, 1, 1);
GUEST_PRINTF_ENTRY(bb_guest_fprintf, 2, 2);
GUEST_PRINTF_ENTRY(bb_guest_sprintf, 2, 3);
GUEST_PRINTF_ENTRY(bb_guest_snprintf, 3, 4);

/* The parts of the family that take no arguments of their own. */
static ABI int guest_puts(const char *text) {
    if (text) { fputs(text,stdout); fputc('\n',stdout); fflush(stdout); }
    return text?(int)strlen(text):-1;
}
static ABI int guest_fputs(const char *text, void *stream) {
    (void)stream;
    if (text) { fputs(text,stdout); fflush(stdout); }
    return 0;
}
static ABI int guest_putchar(int c) { fputc(c,stdout); fflush(stdout); return c; }
static ABI int guest_fputc(int c, void *stream) { (void)stream; fputc(c,stdout); fflush(stdout); return c; }

uintptr_t runtime_printf_resolve(const char *name) {
    if (runtime_nid_is(name,"hcuQgD53UxM#q#q")) return (uintptr_t)bb_guest_printf;
    if (runtime_nid_is(name,"fffwELXNVFA#q#q")) return (uintptr_t)bb_guest_fprintf;
    if (runtime_nid_is(name,"tcVi5SivF7Q#q#q")) return (uintptr_t)bb_guest_sprintf;
    if (runtime_nid_is(name,"eLdDw6l0-bU#q#q")) return (uintptr_t)bb_guest_snprintf;
    if (runtime_nid_is(name,"GMpvxPFW924#q#q")) return (uintptr_t)guest_vprintf;
    if (runtime_nid_is(name,"pDBDcY6uLSA#q#q")) return (uintptr_t)guest_vfprintf;
    if (runtime_nid_is(name,"jbz9I9vkqkk#q#q")) return (uintptr_t)guest_vsprintf;
    if (runtime_nid_is(name,"Q2V+iqvjgC0#q#q")) return (uintptr_t)guest_vsnprintf;
    if (runtime_nid_is(name,"xEszJVGpybs#q#q")) return (uintptr_t)bb_guest_snprintf;
    if (runtime_nid_is(name,"YQ0navp+YIc#q#q")) return (uintptr_t)guest_puts;
    if (runtime_nid_is(name,"QrZZdJ8XsX0#q#q")) return (uintptr_t)guest_fputs;
    if (runtime_nid_is(name,"m5wN+SwZOR4#q#q")) return (uintptr_t)guest_putchar;
    if (runtime_nid_is(name,"aZK8lNei-Qw#q#q")) return (uintptr_t)guest_fputc;
    return 0;
}
