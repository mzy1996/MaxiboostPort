/* The guest's files ------------------------------------------------------------
 * The game opens and reads its files through the C library of the system
 * (libSceLibcInternal), which the runtime stands in for. The C library's own
 * streams (the FILE the game gets from fopen) are the runtime's: the game is given
 * a pointer to a small structure the runtime keeps, and the file behind it is
 * opened through the runtime's own file layer (which knows the game's paths).
 *
 * The three streams every program has (the input, the output and the error
 * output) are the ones the runtime writes to: the game's log is read there.
 */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>

/* The stream the game holds: the file behind it and a little of its own state. */
typedef struct GuestFile {
    uint64_t magic;          /* the sign that this is one of ours */
    int64_t fd;              /* the file behind it (-1: not open) */
    int eof, error, ungot;
    int ungot_char;
    char *path;              /* the guest's path, for the messages */
} GuestFile;

#define GUEST_FILE_MAGIC UINT64_C(0x474d425354524541)

/* The streams the game opened. */
static GuestFile **files;
static size_t file_count, file_capacity;
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

/* The three standard streams: they are the runtime's own output. The game asks
 * for them with fdopen or with the pointers the C library gives it; they are
 * created here so that a flush of the output reaches the game's log. */
static GuestFile standard[3];

/* A new stream: the game gets a pointer to it. */
static GuestFile *file_new(int64_t fd,const char *path) {
    pthread_mutex_lock(&file_lock);
    if (file_count==file_capacity) {
        const size_t capacity=file_capacity?file_capacity*2:16;
        GuestFile **grown=(GuestFile **)realloc(files,capacity*sizeof(*grown));
        if (!grown) { pthread_mutex_unlock(&file_lock); return NULL; }
        files=grown; file_capacity=capacity;
    }
    GuestFile *f=(GuestFile *)calloc(1,sizeof(*f));
    if (!f) { pthread_mutex_unlock(&file_lock); return NULL; }
    f->magic=GUEST_FILE_MAGIC;
    f->fd=fd;
    if (path) { f->path=strdup(path); }
    files[file_count++]=f;
    pthread_mutex_unlock(&file_lock);
    return f;
}
/* The stream behind a pointer the game holds. */
static GuestFile *file_of(void *pointer) {
    if (!pointer) return NULL;
    GuestFile *f=(GuestFile *)pointer;
    if (f->magic!=GUEST_FILE_MAGIC) return NULL;
    return f;
}
/* The stream of a number: the game keeps the numbers of its streams (the ones
 * fopen returns). */
static GuestFile *file_by_index(uint64_t index) {
    pthread_mutex_lock(&file_lock);
    GuestFile *f=index<file_count?files[index]:NULL;
    pthread_mutex_unlock(&file_lock);
    return f;
}

static void file_init(void) {
    if (file_count) return;
    for (int i=0;i<3;++i) {
        standard[i].magic=GUEST_FILE_MAGIC;
        standard[i].fd=i;   /* the host's own streams */
    }
}

/* ---- the C library's own functions ------------------------------------- */

/* The flags of fopen: the same values the C library uses. */
#define GUEST_O_RDONLY 0
#define GUEST_O_WRONLY 1
#define GUEST_O_RDWR   2
#define GUEST_O_APPEND 0x8
#define GUEST_O_CREAT  0x200
#define GUEST_O_TRUNC  0x400
#define GUEST_O_EXCL   0x800

static ABI void *guest_fopen(const char *path, const char *mode) {
    file_init();
    if (!path || !mode) return NULL;
    int flags=0, mode_bits=0;
    switch (mode[0]) {
    case 'r': flags=GUEST_O_RDONLY; mode_bits=0; break;
    case 'w': flags=GUEST_O_WRONLY|GUEST_O_CREAT|GUEST_O_TRUNC; mode_bits=0666; break;
    case 'a': flags=GUEST_O_WRONLY|GUEST_O_CREAT|GUEST_O_APPEND; mode_bits=0666; break;
    default: return NULL;
    }
    if (strchr(mode,'+')) flags=(flags&~(GUEST_O_RDONLY|GUEST_O_WRONLY))|GUEST_O_RDWR;
    const int64_t fd=runtime_file_open(path,flags,mode_bits);
    if (fd<0) { errno=(int)-fd; return NULL; }
    GuestFile *f=file_new(fd,path);
    if (!f) { runtime_file_close(fd); return NULL; }
    return f;
}
static ABI int guest_fclose(void *file) {
    GuestFile *f=file_of(file);
    if (!f) { errno=EBADF; return EOF; }
    if (f->fd>=0 && f->fd>2) runtime_file_close(f->fd);
    f->fd=-1;
    return 0;
}
static ABI int guest_fflush(void *file) {
    if (!file) { fflush(stdout); return 0; }
    GuestFile *f=file_of(file);
    if (!f) { errno=EBADF; return EOF; }
    /* The standard streams are the runtime's own output: a flush of them puts
     * what the game printed into the log. */
    if (f->fd>=0 && f->fd<3) { fflush(f->fd==1?stdout:stderr); return 0; }
    return 0;
}
static ABI size_t guest_fwrite(const void *text, size_t size, size_t count, void *file) {
    GuestFile *f=file_of(file);
    if (!f || !size || !count) return 0;
    const uint64_t total=(uint64_t)size*count;
    if (f->fd>=0 && f->fd<3) {
        /* The standard output: the game's log. */
        fwrite(text,1,total,stdout);
        fflush(stdout);
        return count;
    }
    const int64_t n=runtime_file_write(f->fd,text,total);
    if (n<0) { errno=(int)-n; return 0; }
    return (size_t)n/size;
}
static ABI size_t guest_fread(void *buffer, size_t size, size_t count, void *file) {
    GuestFile *f=file_of(file);
    if (!f || !size || !count) return 0;
    const uint64_t total=(uint64_t)size*count;
    const int64_t n=runtime_file_read(f->fd,buffer,total);
    if (n<0) { errno=(int)-n; f->error=1; return 0; }
    if (n==0) { f->eof=1; return 0; }
    return (size_t)n/size;
}
static ABI int guest_fseek(void *file, long offset, int whence) {
    GuestFile *f=file_of(file);
    if (!f) { errno=EBADF; return -1; }
    const int64_t n=runtime_file_lseek(f->fd,offset,whence);
    if (n<0) { errno=(int)-n; return -1; }
    f->eof=0;
    return 0;
}
static ABI long guest_ftell(void *file) {
    GuestFile *f=file_of(file);
    if (!f) { errno=EBADF; return -1; }
    const int64_t n=runtime_file_lseek(f->fd,0,SEEK_CUR);
    if (n<0) { errno=(int)-n; return -1; }
    return (long)n;
}
static ABI void guest_rewind(void *file) {
    GuestFile *f=file_of(file);
    if (!f) return;
    runtime_file_lseek(f->fd,0,SEEK_SET);
    f->eof=0; f->error=0; f->ungot=0;
}
static ABI int guest_fgetc(void *file) {
    GuestFile *f=file_of(file);
    if (!f) { errno=EBADF; return EOF; }
    if (f->ungot) { f->ungot=0; return f->ungot_char; }
    char c;
    const int64_t n=runtime_file_read(f->fd,&c,1);
    if (n!=1) { f->eof=1; return EOF; }
    return (unsigned char)c;
}
static ABI int guest_getc(void *file) { return guest_fgetc(file); }
static ABI int guest_ungetc(int c, void *file) {
    GuestFile *f=file_of(file);
    if (!f || c==EOF) return EOF;
    f->ungot=1; f->ungot_char=(unsigned char)c;
    f->eof=0;
    return (unsigned char)c;
}
static ABI char *guest_fgets(char *buffer, int size, void *file) {
    GuestFile *f=file_of(file);
    if (!f || !buffer || size<=0) return NULL;
    int i=0;
    while (i<size-1) {
        const int c=guest_fgetc(file);
        if (c==EOF) break;
        buffer[i++]=(char)c;
        if (c=='\n') break;
    }
    if (!i) return NULL;
    buffer[i]='\0';
    return buffer;
}
static ABI int guest_fgetpos(void *file, void *position) {
    GuestFile *f=file_of(file);
    if (!f || !position) { errno=EINVAL; return -1; }
    const long n=guest_ftell(file);
    if (n<0) return -1;
    memcpy(position,&n,sizeof(n));
    return 0;
}
static ABI int guest_fsetpos(void *file, const void *position) {
    GuestFile *f=file_of(file);
    if (!f || !position) { errno=EINVAL; return -1; }
    long n=0;
    memcpy(&n,position,sizeof(n));
    return guest_fseek(file,n,SEEK_SET);
}
static ABI void guest_clearerr(void *file) {
    GuestFile *f=file_of(file);
    if (!f) return;
    f->eof=0; f->error=0;
}
static ABI int guest_ferror(void *file) {
    GuestFile *f=file_of(file);
    return f?f->error:0;
}
static ABI int guest_setvbuf(void *file, char *buffer, int mode, size_t size) {
    (void)file; (void)buffer; (void)mode; (void)size;
    return 0;   /* the runtime's own streams are not buffered by the game */
}
static ABI void guest_perror(const char *text) {
    if (text) fprintf(stderr,"%s: %s\n",text,strerror(errno));
    else fprintf(stderr,"%s\n",strerror(errno));
}
static ABI int guest_remove(const char *path) {
    if (!path) { errno=EINVAL; return -1; }
    char host[1024];
    if (runtime_file_translate(path,host,sizeof(host))) { errno=ENOENT; return -1; }
    if (remove(host)) { errno=errno; return -1; }
    return 0;
}
static ABI int guest_rename(const char *from, const char *to) {
    if (!from || !to) { errno=EINVAL; return -1; }
    char a[1024], b[1024];
    if (runtime_file_translate(from,a,sizeof(a)) || runtime_file_translate(to,b,sizeof(b))) { errno=ENOENT; return -1; }
    if (rename(a,b)) return -1;
    return 0;
}
static ABI void *guest_fdopen(int fd, const char *mode) {
    file_init();
    if (fd<0 || fd>2) {
        /* A file of its own: the runtime's file layer has the number already. */
        GuestFile *f=file_new(fd,NULL);
        return f;
    }
    return &standard[fd];
}
static ABI int guest_fileno(void *file) {
    GuestFile *f=file_of(file);
    return f?(int)f->fd:-1;
}
static ABI void *guest_freopen(const char *path, const char *mode, void *file) {
    GuestFile *f=file_of(file);
    if (!f || !path || !mode) return NULL;
    if (f->fd>=0 && f->fd>2) runtime_file_close(f->fd);
    void *fresh=guest_fopen(path,mode);
    if (!fresh) return NULL;
    GuestFile *g=file_of(fresh);
    f->fd=g->fd;
    g->fd=-1;   /* the new stream takes the file over */
    return file;
}

/* The streams of the program: the game asks for them by name (they are data, not
 * functions), and it hands them back to the functions of the C library. */
void *runtime_stdio_stream(int which) {
    file_init();
    if (which<0 || which>2) return NULL;
    return &standard[which];
}

uintptr_t runtime_stdio_resolve(const char *name) {
    if (runtime_nid_is(name,"xeYO4u7uyJ0#q#q")) return (uintptr_t)guest_fopen;
    if (runtime_nid_is(name,"uodLYyUip20#q#q")) return (uintptr_t)guest_fclose;
    if (runtime_nid_is(name,"MUjC4lbHrK4#q#q")) return (uintptr_t)guest_fflush;
    if (runtime_nid_is(name,"MpxhMh8QFro#q#q")) return (uintptr_t)guest_fwrite;
    if (runtime_nid_is(name,"lbB+UlZqVG0#q#q")) return (uintptr_t)guest_fread;
    if (runtime_nid_is(name,"rQFVBXp-Cxg#q#q")) return (uintptr_t)guest_fseek;
    if (runtime_nid_is(name,"Qazy8LmXTvw#q#q")) return (uintptr_t)guest_ftell;
    if (runtime_nid_is(name,"3QIPIh-GDjw#q#q")) return (uintptr_t)guest_rewind;
    if (runtime_nid_is(name,"AEuF3F2f8TA#q#q")) return (uintptr_t)guest_fgetc;
    if (runtime_nid_is(name,"8Q60JLJ6Rv4#q#q")) return (uintptr_t)guest_getc;
    if (runtime_nid_is(name,"-LFO7jhD5CE#q#q")) return (uintptr_t)guest_ungetc;
    if (runtime_nid_is(name,"KdP-nULpuGw#q#q")) return (uintptr_t)guest_fgets;
    if (runtime_nid_is(name,"SHlt7EhOtqA#q#q")) return (uintptr_t)guest_fgetpos;
    if (runtime_nid_is(name,"QMFyLoqNxIg#q#q")) return (uintptr_t)guest_setvbuf;
    if (runtime_nid_is(name,"St9nbxSoezk#q#q")) return (uintptr_t)guest_clearerr;
    if (runtime_nid_is(name,"AHxyhN96dy4#q#q")) return (uintptr_t)guest_ferror;
    if (runtime_nid_is(name,"EMutwaQ34Jo#q#q")) return (uintptr_t)guest_perror;
    if (runtime_nid_is(name,"MZO7FXyAPU8#q#q")) return (uintptr_t)guest_remove;
    if (runtime_nid_is(name,"NN01qLRhiqU#I#J")) return (uintptr_t)guest_rename;
    if (runtime_nid_is(name,"gkWgn0p1AfU#q#q")) return (uintptr_t)guest_freopen;
    return 0;
}
