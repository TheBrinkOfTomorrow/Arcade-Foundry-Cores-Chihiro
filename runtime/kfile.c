// Handles and the file system. Paths arrive as NT object names
// ("\??\D:\media\x.bin", "\Device\CdRom0\x", "\Device\Harddisk0\Partition1\TDATA");
// symbolic links are resolved here, then a device prefix picks a host folder:
// the game's own files for the CD-ROM (the media board's image on a Chihiro),
// save_root/partitionN for the hard disk partitions. Lookups ignore case, as
// FATX does. All I/O completes before the call returns.
#define _GNU_SOURCE
#include "xbox.h"
#include "baseboard.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define A(i) ARG(i)

// ---- handles ---------------------------------------------------------------------------------------
#define MAX_HANDLES 4096
static struct { uint32_t object; int kind; } handles[MAX_HANDLES];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

// handles are multiples of 4 from 0x100, like the kernel's
uint32_t ob_handle_new(uint32_t object, int kind)
{
    pthread_mutex_lock(&handle_lock);
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i].kind) {
            handles[i].object = object;
            handles[i].kind = kind;
            pthread_mutex_unlock(&handle_lock);
            return 0x100 + 4 * (uint32_t)i;
        }
    pthread_mutex_unlock(&handle_lock);
    host_fatal("out of handles");
}

static int handle_index(uint32_t h)
{
    if (h < 0x104 || (h & 3)) return -1;
    uint32_t i = (h - 0x100) / 4;
    return i < MAX_HANDLES ? (int)i : -1;
}

uint32_t ob_object(uint32_t h, int *kind)
{
    int i = handle_index(h);
    if (i < 0 || !handles[i].kind) {
        if (kind) *kind = 0;
        return 0;
    }
    if (kind) *kind = handles[i].kind;
    return handles[i].object;
}

void ob_handle_close(uint32_t h)
{
    int i = handle_index(h);
    if (i < 0) return;
    pthread_mutex_lock(&handle_lock);
    handles[i].kind = 0;
    handles[i].object = 0;
    pthread_mutex_unlock(&handle_lock);
}

// ---- open files ---------------------------------------------------------------------------------------
enum { F_FILE = 1, F_DIR, F_DEVICE, F_VOLUME };
typedef struct kfile {
    int type;
    int fd;
    uint64_t pos;
    char path[1024];         // host path
    DIR *dir;                // a directory being listed
    char pattern[256];
    char nt[256];            // the name the game opened, for the log
} kfile;
#define MAX_FILES 512
static kfile *files[MAX_FILES];
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t file_new(kfile *f)
{
    pthread_mutex_lock(&file_lock);
    for (uint32_t i = 1; i < MAX_FILES; i++)
        if (!files[i]) {
            files[i] = f;
            pthread_mutex_unlock(&file_lock);
            return ob_handle_new(i, f->type == F_DIR ? OB_DIR : OB_FILE);
        }
    pthread_mutex_unlock(&file_lock);
    return 0;
}

static kfile *file_of(uint32_t h)
{
    int kind;
    uint32_t i = ob_object(h, &kind);
    if ((kind != OB_FILE && kind != OB_DIR) || i >= MAX_FILES) return NULL;
    return files[i];
}

uint32_t kfile_close(uint32_t h)
{
    int kind;
    uint32_t i = ob_object(h, &kind);
    if ((kind != OB_FILE && kind != OB_DIR) || i >= MAX_FILES || !files[i]) return STATUS_INVALID_HANDLE;
    kfile *f = files[i];
    pthread_mutex_lock(&file_lock);
    files[i] = NULL;
    pthread_mutex_unlock(&file_lock);
    if (f->fd >= 0) close(f->fd);
    if (f->dir) closedir(f->dir);
    free(f);
    return STATUS_SUCCESS;
}

// ---- names --------------------------------------------------------------------------------------------
// an OBJECT_STRING (ANSI_STRING) in guest memory: u16 length, u16 maximum, char *buffer
static void ostring(uint32_t s, char *out, size_t n)
{
    out[0] = 0;
    if (!s) return;
    uint32_t len = rd16(s), buf = rd32(s + 4);
    if (len >= n) len = (uint32_t)n - 1;
    if (buf) memcpy(out, M + buf, len);
    out[len] = 0;
}

#define MAX_LINKS 64
static struct { char name[64], target[256]; } links[MAX_LINKS];
static int nlinks;
static pthread_mutex_t link_lock = PTHREAD_MUTEX_INITIALIZER;

static void link_set(const char *name, const char *target)
{
    pthread_mutex_lock(&link_lock);
    int i;
    for (i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, name)) break;
    if (i == nlinks && nlinks < MAX_LINKS) nlinks++;
    if (i < MAX_LINKS) {
        snprintf(links[i].name, sizeof links[i].name, "%s", name);
        snprintf(links[i].target, sizeof links[i].target, "%s", target);
    }
    pthread_mutex_unlock(&link_lock);
    hle_log("link %s -> %s\n", name, target);
}

static const char *link_get(const char *name)
{
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].name, name)) return links[i].target;
    return NULL;
}

// "\??\D:\x" -> "\Device\CdRom0\x", repeatedly; a name relative to a
// directory handle is joined to that directory's NT name first
static void resolve_links(char *p, size_t n)
{
    for (int depth = 0; depth < 8; depth++) {
        int done = 1;
        pthread_mutex_lock(&link_lock);
        for (int i = 0; i < nlinks; i++) {
            size_t l = strlen(links[i].name);
            if (!strncasecmp(p, links[i].name, l) && (p[l] == '\\' || p[l] == 0)) {
                char tmp[512];
                snprintf(tmp, sizeof tmp, "%s%s", links[i].target, p + l);
                snprintf(p, n, "%s", tmp);
                done = 0;
                break;
            }
        }
        pthread_mutex_unlock(&link_lock);
        if (done) return;
    }
}

// find each component of rel under base, ignoring case; the last may not exist
static int host_path(const char *base, const char *rel, char *out, size_t n)
{
    snprintf(out, n, "%s", base);
    while (*rel == '\\') rel++;
    while (*rel) {
        const char *e = strchr(rel, '\\');
        size_t l = e ? (size_t)(e - rel) : strlen(rel);
        char comp[256];
        if (l >= sizeof comp) return -1;
        memcpy(comp, rel, l);
        comp[l] = 0;
        rel += l;
        while (*rel == '\\') rel++;
        if (!strcmp(comp, ".")) continue;
        size_t cur = strlen(out);
        if (!strcmp(comp, "..")) {
            char *s = strrchr(out, '/');
            if (s && s > out + strlen(base) - 1) *s = 0;
            continue;
        }
        DIR *d = opendir(out);
        const char *use = comp;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (!strcasecmp(de->d_name, comp)) {
                    snprintf(comp, sizeof comp, "%s", de->d_name);
                    break;
                }
            closedir(d);
        }
        snprintf(out + cur, n - cur, "/%s", use);
    }
    return 0;
}

// the NT name to a host path; returns 0, or -1 for a device we don't have,
// or 1 for the media board's communication device
static int map_name(const char *nt_in, char *out, size_t n, char *nt_out, size_t ntn)
{
    char p[512];
    snprintf(p, sizeof p, "%s", nt_in);
    for (char *q = p; *q; q++)
        if (*q == '/') *q = '\\';
    resolve_links(p, sizeof p);
    if (nt_out) snprintf(nt_out, ntn, "%s", p);
    if (!strncasecmp(p, "mbcom:", 6) || !strncasecmp(p, "\\??\\mbcom:", 10)) return 1;
    static const char cd[] = "\\Device\\CdRom0";
    static const char hd[] = "\\Device\\Harddisk0\\Partition";
    if (!strncasecmp(p, cd, sizeof cd - 1)) return host_path(game_root, p + sizeof cd - 1, out, n);
    if (!strncasecmp(p, hd, sizeof hd - 1)) {
        int part = atoi(p + sizeof hd - 1);
        const char *rest = p + sizeof hd - 1;
        while (*rest >= '0' && *rest <= '9') rest++;
        char base[1100];
        mkdir(save_root, 0755);
        if (part == 0) {
            // the whole disk, raw: its first sectors hold the refurbishment
            // record and the cache partitions' headers. A file of zeros.
            snprintf(out, n, "%s/partition0.bin", save_root);
            struct stat st;
            if (stat(out, &st)) {
                int fd = open(out, O_RDWR | O_CREAT, 0644);
                if (fd >= 0) {
                    if (ftruncate(fd, 0x80000)) {}
                    close(fd);
                }
            }
            return 0;
        }
        snprintf(base, sizeof base, "%s/partition%d", save_root, part);
        mkdir(base, 0755);
        if (!*rest) {
            // the partition itself, not its root folder: a raw volume, as the
            // cache partitions are opened to format them
            snprintf(out, n, "%s", base);
            return 2;
        }
        return host_path(base, rest, out, n);
    }
    return -1;
}

static uint32_t errno_status(int e)
{
    switch (e) {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
    case EACCES:
    case EPERM:
    case EROFS: return STATUS_ACCESS_DENIED;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static void iosb(uint32_t io, uint32_t status, uint32_t info)
{
    if (!io) return;
    wr32(io, status);
    wr32(io + 4, info);
}

// ---- open ------------------------------------------------------------------------------------------------
enum { FILE_SUPERSEDE, FILE_OPEN, FILE_CREATE, FILE_OPEN_IF, FILE_OVERWRITE, FILE_OVERWRITE_IF };
#define FILE_DIRECTORY_FILE 0x01
#define FILE_NON_DIRECTORY_FILE 0x40

static uint32_t open_file(uint32_t ph, uint32_t access, uint32_t attrs, uint32_t io, uint32_t disposition,
                          uint32_t options)
{
    // OBJECT_ATTRIBUTES: RootDirectory, ObjectName, Attributes
    char name[512], full[512], host[1100], nt[256];
    uint32_t root = rd32(attrs);
    ostring(rd32(attrs + 4), name, sizeof name);
    if (root == 0xFFFFFFFDu) {
        // ObDosDevicesDirectory(): names like "D:\x" are under \??
        snprintf(full, sizeof full, "\\??\\%s", name);
    } else if (root) {
        kfile *d = file_of(root);
        snprintf(full, sizeof full, "%s\\%s", d ? d->nt : "", name);
    } else snprintf(full, sizeof full, "%s", name);
    int r = map_name(full, host, sizeof host, nt, sizeof nt);
    kfile *f = calloc(1, sizeof *f);
    f->fd = -1;
    snprintf(f->nt, sizeof f->nt, "%s", nt);
    if (r == 2) {
        f->type = F_VOLUME;
        snprintf(f->path, sizeof f->path, "%s", host);
        if (hle_verbose) hle_log("open %s: raw volume\n", nt);
        iosb(io, 0, 1);
    } else if (r == 1) {
        f->type = F_DEVICE;
        hle_log("open %s: media board device\n", full);
    } else if (r < 0) {
        hle_log("open %s (%s): no such device\n", full, nt);
        free(f);
        iosb(io, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    } else {
        snprintf(f->path, sizeof f->path, "%s", host);
        struct stat st;
        int exists = stat(host, &st) == 0;
        int isdir = exists && S_ISDIR(st.st_mode);
        uint32_t status = 0, info = 0;   // info: FILE_OPENED 1, FILE_CREATED 2, FILE_OVERWRITTEN 3
        if ((options & FILE_DIRECTORY_FILE) || isdir) {
            if (!exists) {
                if (disposition == FILE_CREATE || disposition == FILE_OPEN_IF) {
                    if (mkdir(host, 0755)) status = errno_status(errno);
                    info = 2;
                } else status = STATUS_OBJECT_NAME_NOT_FOUND;
            } else if (!isdir) status = STATUS_OBJECT_PATH_NOT_FOUND;   // NOT_A_DIRECTORY
            else if (disposition == FILE_CREATE) status = STATUS_OBJECT_NAME_COLLISION;
            else info = 1;
            if (!status && (options & FILE_NON_DIRECTORY_FILE)) status = 0xC00000BAu;   // FILE_IS_A_DIRECTORY
            f->type = F_DIR;
        } else {
            int fl = O_RDWR;
            switch (disposition) {
            case FILE_OPEN: info = 1; break;
            case FILE_CREATE: fl |= O_CREAT | O_EXCL; info = 2; break;
            case FILE_OPEN_IF: fl |= O_CREAT; info = exists ? 1 : 2; break;
            case FILE_OVERWRITE: fl |= O_TRUNC; info = 3; break;
            case FILE_SUPERSEDE:
            case FILE_OVERWRITE_IF: fl |= O_CREAT | O_TRUNC; info = exists ? 3 : 2; break;
            }
            f->fd = open(host, fl, 0644);
            if (f->fd < 0 && (errno == EACCES || errno == EROFS) && !(fl & (O_CREAT | O_TRUNC)))
                f->fd = open(host, O_RDONLY);
            if (f->fd < 0) status = errno_status(errno);
            f->type = F_FILE;
        }
        if (hle_verbose) hle_log("open %s -> %s: 0x%x\n", nt, host, status);
        if (status) {
            if (f->fd >= 0) close(f->fd);
            free(f);
            iosb(io, status, 0);
            return status;
        }
        iosb(io, 0, info);
    }
    uint32_t h = file_new(f);
    if (!h) {
        if (f->fd >= 0) close(f->fd);
        free(f);
        return STATUS_NO_MEMORY;
    }
    wr32(ph, h);
    (void)access;
    return STATUS_SUCCESS;
}

uint32_t k_NtCreateFile(CPU *c) { return open_file(A(0), A(1), A(2), A(3), A(7), A(8)); }
uint32_t k_NtOpenFile(CPU *c) { return open_file(A(0), A(1), A(2), A(3), FILE_OPEN, A(5)); }

// ---- completion: event and APC ---------------------------------------------------------------------------
static void complete(CPU *c, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t io)
{
    if (event) {
        int kind;
        uint32_t ev = ob_object(event, &kind);
        if (kind == OB_EVENT) ke_set_event(ev);
    }
    if (apc) {
        uint32_t args[3] = { apc_ctx, io, 0 };
        guest_call(c, apc, 3, args);
    }
}

static uint64_t offset_arg(kfile *f, uint32_t p)
{
    if (!p) return f->pos;
    uint64_t o = rd64(p);
    if ((o >> 32) == 0xFFFFFFFFu && ((uint32_t)o == 0xFFFFFFFEu || (uint32_t)o == 0xFFFFFFFFu)) return f->pos;
    return o;
}

uint32_t k_NtReadFile(CPU *c)
{
    // handle, event, apc, apc context, iosb, buffer, length, *offset
    kfile *f = file_of(A(0));
    uint32_t io = A(4), buf = A(5), len = A(6);
    if (!f) return STATUS_INVALID_HANDLE;
    if (f->type == F_VOLUME) {
        // nothing was written there that the game reads back: zeros
        memset(M + buf, 0, len);
        iosb(io, 0, len);
        complete(c, A(1), A(2), A(3), io);
        return STATUS_SUCCESS;
    }
    if (f->type == F_DEVICE) {
        uint64_t at = offset_arg(f, A(7));
        mediaboard_read(at, M + buf, len);
        f->pos = at + len;
        iosb(io, 0, len);
        complete(c, A(1), A(2), A(3), io);
        return STATUS_SUCCESS;
    }
    if (f->type != F_FILE) return STATUS_INVALID_DEVICE_REQUEST;
    uint64_t at = offset_arg(f, A(7));
    ssize_t n = pread(f->fd, M + buf, len, (off_t)at);
    uint32_t st = 0;
    if (n < 0) st = errno_status(errno), n = 0;
    else if (n == 0 && len) st = STATUS_END_OF_FILE;
    f->pos = at + (uint64_t)n;
    iosb(io, st, (uint32_t)n);
    complete(c, A(1), A(2), A(3), io);
    return st;
}

uint32_t k_NtWriteFile(CPU *c)
{
    kfile *f = file_of(A(0));
    uint32_t io = A(4), buf = A(5), len = A(6);
    if (!f) return STATUS_INVALID_HANDLE;
    if (f->type == F_VOLUME) {
        iosb(io, 0, len);   // a format of a cache partition: its folder stands in for it
        complete(c, A(1), A(2), A(3), io);
        return STATUS_SUCCESS;
    }
    if (f->type == F_DEVICE) {
        uint64_t at = offset_arg(f, A(7));
        mediaboard_write(at, M + buf, len);
        f->pos = at + len;
        iosb(io, 0, len);
        complete(c, A(1), A(2), A(3), io);
        return STATUS_SUCCESS;
    }
    if (f->type != F_FILE) return STATUS_INVALID_DEVICE_REQUEST;
    uint64_t at = offset_arg(f, A(7));
    ssize_t n = pwrite(f->fd, M + buf, len, (off_t)at);
    uint32_t st = 0;
    if (n < 0) st = errno_status(errno), n = 0;
    f->pos = at + (uint64_t)n;
    iosb(io, st, (uint32_t)n);
    complete(c, A(1), A(2), A(3), io);
    return st;
}

uint32_t k_NtFlushBuffersFile(CPU *c)
{
    kfile *f = file_of(A(0));
    if (!f) return STATUS_INVALID_HANDLE;
    if (f->fd >= 0) fsync(f->fd);
    iosb(A(1), 0, 0);
    return STATUS_SUCCESS;
}

// ---- information ----------------------------------------------------------------------------------------
static uint64_t nt_time(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }

static uint32_t attributes(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? 0x10 : 0x80;   // DIRECTORY or NORMAL
}

static int file_stat(kfile *f, struct stat *st)
{
    if (f->fd >= 0) return fstat(f->fd, st);
    if (f->path[0]) return stat(f->path, st);
    memset(st, 0, sizeof *st);
    return 0;
}

uint32_t k_NtQueryInformationFile(CPU *c)
{
    kfile *f = file_of(A(0));
    uint32_t io = A(1), out = A(2), len = A(3), cls = A(4);
    if (!f) return STATUS_INVALID_HANDLE;
    struct stat st;
    file_stat(f, &st);
    uint32_t n = 0;
    memset(M + out, 0, len);
    switch (cls) {
    case 4:   // FileBasicInformation
        n = 40;
        if (len < n) break;
        wr64(out, nt_time(st.st_ctime));
        wr64(out + 8, nt_time(st.st_atime));
        wr64(out + 16, nt_time(st.st_mtime));
        wr64(out + 24, nt_time(st.st_mtime));
        wr32(out + 32, attributes(&st));
        break;
    case 5:   // FileStandardInformation
        n = 24;
        if (len < n) break;
        wr64(out, ((uint64_t)st.st_size + 4095) & ~4095ull);
        wr64(out + 8, (uint64_t)st.st_size);
        wr32(out + 16, 1);
        wr8(out + 21, S_ISDIR(st.st_mode));
        break;
    case 14:   // FilePositionInformation
        n = 8;
        if (len < n) break;
        wr64(out, f->pos);
        break;
    case 34:   // FileNetworkOpenInformation
        n = 56;
        if (len < n) break;
        wr64(out, nt_time(st.st_ctime));
        wr64(out + 8, nt_time(st.st_atime));
        wr64(out + 16, nt_time(st.st_mtime));
        wr64(out + 24, nt_time(st.st_mtime));
        wr64(out + 32, ((uint64_t)st.st_size + 4095) & ~4095ull);
        wr64(out + 40, (uint64_t)st.st_size);
        wr32(out + 48, attributes(&st));
        break;
    default:
        hle_log("NtQueryInformationFile: class %u for %s\n", cls, f->nt);
        iosb(io, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }
    if (len < n) {
        iosb(io, STATUS_BUFFER_TOO_SMALL, 0);
        return STATUS_BUFFER_TOO_SMALL;
    }
    iosb(io, 0, n);
    return STATUS_SUCCESS;
}

uint32_t k_NtSetInformationFile(CPU *c)
{
    kfile *f = file_of(A(0));
    uint32_t io = A(1), in = A(2), cls = A(4);
    if (!f) return STATUS_INVALID_HANDLE;
    switch (cls) {
    case 14:   // FilePositionInformation
        f->pos = rd64(in);
        break;
    case 19:   // FileAllocationInformation
    case 20:   // FileEndOfFileInformation
        if (f->fd >= 0 && (cls == 20 || rd64(in) < (uint64_t)lseek(f->fd, 0, SEEK_END)))
            if (ftruncate(f->fd, (off_t)rd64(in))) {}
        break;
    case 13:   // FileDispositionInformation: delete on close
        if (rd8(in) && f->path[0]) {
            hle_log("delete %s\n", f->path);
            if (f->type == F_DIR) rmdir(f->path);
            else unlink(f->path);
        }
        break;
    case 4:   // FileBasicInformation: times and attributes, ignored
        break;
    default:
        hle_log("NtSetInformationFile: class %u for %s\n", cls, f->nt);
        break;
    }
    iosb(io, 0, 0);
    return STATUS_SUCCESS;
}

uint32_t k_NtQueryVolumeInformationFile(CPU *c)
{
    kfile *f = file_of(A(0));
    uint32_t io = A(1), out = A(2), len = A(3), cls = A(4), n = 0;
    if (!f) return STATUS_INVALID_HANDLE;
    memset(M + out, 0, len);
    switch (cls) {
    case 3:   // FileFsSizeInformation: 1 GB in 16 KB clusters, half free
        n = 24;
        if (len < n) break;
        wr64(out, 65536);
        wr64(out + 8, 32768);
        wr32(out + 16, 32);
        wr32(out + 20, 512);
        break;
    case 7:   // FileFsFullSizeInformation
        n = 32;
        if (len < n) break;
        wr64(out, 65536);
        wr64(out + 8, 32768);
        wr64(out + 16, 32768);
        wr32(out + 24, 32);
        wr32(out + 28, 512);
        break;
    case 1:   // FileFsVolumeInformation: time, serial, label length, objects, label
        n = 18;
        if (len < n) break;
        wr32(out + 8, 0x43484952);
        break;
    case 4:   // FileFsDeviceInformation: type, characteristics
        n = 8;
        if (len < n) break;
        wr32(out, strstr(f->nt, "CdRom") ? 2 : 7);
        break;
    case 5:   // FileFsAttributeInformation: attributes, max name, name length, "FATX"
        n = 16;
        if (len < n) break;
        wr32(out, 0x2);
        wr32(out + 4, 42);
        wr32(out + 8, 4);
        memcpy(M + out + 12, "FATX", 4);
        break;
    default:
        hle_log("NtQueryVolumeInformationFile: class %u for %s\n", cls, f->nt);
        iosb(io, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }
    if (len < n) {
        iosb(io, STATUS_BUFFER_TOO_SMALL, 0);
        return STATUS_BUFFER_TOO_SMALL;
    }
    iosb(io, 0, n);
    return STATUS_SUCCESS;
}

// '*' and '?' as the kernel matches them, ignoring case
static int wild(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') {
        for (;; s++) {
            if (wild(p + 1, s)) return 1;
            if (!*s) return 0;
        }
    }
    if (!*s) return 0;
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) return wild(p + 1, s + 1);
    return 0;
}

uint32_t k_NtQueryDirectoryFile(CPU *c)
{
    // handle, event, apc, apc context, iosb, info, length, class, *name, restart
    kfile *f = file_of(A(0));
    uint32_t io = A(4), out = A(5), len = A(6), cls = A(7), name = A(8), restart = A(9) & 0xff;
    if (!f || f->type != F_DIR) return STATUS_INVALID_HANDLE;
    if (cls != 1) hle_log("NtQueryDirectoryFile: class %u\n", cls);
    if (name) ostring(name, f->pattern, sizeof f->pattern);
    if (!f->pattern[0]) snprintf(f->pattern, sizeof f->pattern, "*");
    if (restart || !f->dir) {
        if (f->dir) closedir(f->dir);
        f->dir = opendir(f->path);
        if (!f->dir) return STATUS_NO_MORE_FILES;
    }
    struct dirent *de;
    while ((de = readdir(f->dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!wild(f->pattern, de->d_name)) continue;
        char p[1400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", f->path, de->d_name);
        if (stat(p, &st)) continue;
        uint32_t nl = (uint32_t)strlen(de->d_name);
        if (len < 64 + nl) {
            iosb(io, STATUS_BUFFER_TOO_SMALL, 0);
            return STATUS_BUFFER_TOO_SMALL;
        }
        // FILE_DIRECTORY_INFORMATION with an ANSI name
        memset(M + out, 0, 64);
        wr64(out + 8, nt_time(st.st_ctime));
        wr64(out + 16, nt_time(st.st_atime));
        wr64(out + 24, nt_time(st.st_mtime));
        wr64(out + 32, nt_time(st.st_mtime));
        wr64(out + 40, (uint64_t)st.st_size);
        wr64(out + 48, ((uint64_t)st.st_size + 4095) & ~4095ull);
        wr32(out + 56, attributes(&st));
        wr32(out + 60, nl);
        memcpy(M + out + 64, de->d_name, nl);
        iosb(io, 0, 64 + nl);
        complete(c, A(1), A(2), A(3), io);
        return STATUS_SUCCESS;
    }
    iosb(io, STATUS_NO_MORE_FILES, 0);
    return STATUS_NO_MORE_FILES;
}

// ---- device control ---------------------------------------------------------------------------------------
uint32_t k_NtDeviceIoControlFile(CPU *c)
{
    // handle, event, apc, apc context, iosb, code, in, in length, out, out length
    kfile *f = file_of(A(0));
    uint32_t io = A(4), code = A(5), out = A(8), outlen = A(9);
    if (!f) return STATUS_INVALID_HANDLE;
    hle_log("NtDeviceIoControlFile(%s, 0x%x, in %u, out %u)\n", f->nt, code, A(7), outlen);
    switch (code) {
    case 0x70000:   // IOCTL_DISK_GET_DRIVE_GEOMETRY: a 750 MB partition of 512-byte sectors
        if (outlen >= 24) {
            memset(M + out, 0, 24);
            wr64(out, 750ull * 1024 * 1024 / 512);
            wr32(out + 8, 12);      // FixedMedia
            wr32(out + 12, 1);
            wr32(out + 16, 1);
            wr32(out + 20, 512);
        }
        break;
    case 0x74004:   // IOCTL_DISK_GET_PARTITION_INFO
        if (outlen >= 32) {
            memset(M + out, 0, 32);
            wr64(out + 8, 750ull * 1024 * 1024);
            wr8(out + 26, 1);       // recognised
        }
        break;
    default:
        if (out && outlen) memset(M + out, 0, outlen);
        break;
    }
    iosb(io, 0, 0);
    complete(c, A(1), A(2), A(3), io);
    return STATUS_SUCCESS;
}

uint32_t k_NtFsControlFile(CPU *c)
{
    kfile *f = file_of(A(0));
    if (!f) return STATUS_INVALID_HANDLE;
    hle_log("NtFsControlFile(%s, 0x%x)\n", f->nt, A(5));
    if (A(8) && A(9)) memset(M + A(8), 0, A(9));
    iosb(A(4), 0, 0);
    complete(c, A(1), A(2), A(3), A(4));
    return STATUS_SUCCESS;
}

// ---- symbolic links ------------------------------------------------------------------------------------------
uint32_t k_IoCreateSymbolicLink(CPU *c)
{
    char name[64], target[256];
    ostring(A(0), name, sizeof name);
    ostring(A(1), target, sizeof target);
    link_set(name, target);
    return STATUS_SUCCESS;
}

static uint32_t link_objects[MAX_LINKS];

uint32_t k_NtOpenSymbolicLinkObject(CPU *c)
{
    char name[256];
    ostring(rd32(A(1) + 4), name, sizeof name);
    pthread_mutex_lock(&link_lock);
    const char *t = link_get(name);
    int i = t ? (int)((const char *)t - links[0].target) / (int)sizeof links[0] : -1;
    pthread_mutex_unlock(&link_lock);
    if (!t) {
        hle_log("NtOpenSymbolicLinkObject(%s): none\n", name);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    link_objects[i] = (uint32_t)i + 1;
    wr32(A(0), ob_handle_new((uint32_t)i, OB_SYMLINK));
    return STATUS_SUCCESS;
}

uint32_t k_NtQuerySymbolicLinkObject(CPU *c)
{
    int kind;
    uint32_t i = ob_object(A(0), &kind), s = A(1);
    if (kind != OB_SYMLINK || i >= MAX_LINKS) return STATUS_INVALID_HANDLE;
    const char *t = links[i].target;
    uint32_t l = (uint32_t)strlen(t), max = rd16(s + 2);
    if (A(2)) wr32(A(2), l);
    if (l > max) return STATUS_BUFFER_TOO_SMALL;
    memcpy(M + rd32(s + 4), t, l);
    wr16(s, (uint16_t)l);
    return STATUS_SUCCESS;
}

// every file closed, every name forgotten
void kfile_reset(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        kfile *f = files[i];
        if (!f) continue;
        if (f->fd >= 0) close(f->fd);
        if (f->dir) closedir(f->dir);
        free(f);
        files[i] = NULL;
    }
    memset(handles, 0, sizeof handles);
    memset(links, 0, sizeof links);
    nlinks = 0;
    memset(link_objects, 0, sizeof link_objects);
}

void kfile_init(void)
{
    // what the kernel sets up before the game starts: D: is the game's folder
    link_set("\\??\\D:", "\\Device\\CdRom0");
}
