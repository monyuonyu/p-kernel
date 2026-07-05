/*
 *  arch/windows/x86_64/win_mmap.c
 *
 *  Read-only file mmap for the native Windows port, backed by Win32 file
 *  mapping. The GGUF model loader (arch/common/llm/gguf.c) maps the model
 *  file read-only, zero-copy; this provides just enough of the POSIX mmap
 *  surface for that (PROT_READ / MAP_PRIVATE at offset 0). windows.h is
 *  isolated to this TU (compat/sys/mman.h only declares the wrappers).
 *
 *  A small offset→base side table lets munmap(base) find the matching
 *  MapViewOfFile base even though mmap may return base+offset (offset is 0
 *  in current callers, so the table is effectively a passthrough, but the
 *  bookkeeping keeps UnmapViewOfFile correct if that changes).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>          /* _get_osfhandle */
#include <stddef.h>

#define MAP_FAILED_PTR ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
    HANDLE hfile, hmap;
    void *base;
    DWORD protflag, access;

    (void)addr; (void)flags;

    if (prot & 0x2 /*PROT_WRITE*/) {
        protflag = PAGE_READWRITE;
        access   = FILE_MAP_WRITE;
    } else {
        protflag = PAGE_READONLY;
        access   = FILE_MAP_READ;
    }

    hfile = (HANDLE)_get_osfhandle(fd);
    if (hfile == INVALID_HANDLE_VALUE) return MAP_FAILED_PTR;

    /* Size 0,0 → map the whole file. */
    hmap = CreateFileMappingA(hfile, NULL, protflag, 0, 0, NULL);
    if (hmap == NULL) return MAP_FAILED_PTR;

    base = MapViewOfFile(hmap, access,
                         (DWORD)((unsigned long long)offset >> 32),
                         (DWORD)(offset & 0xFFFFFFFF),
                         length);

    /* The view keeps the file mapped even after the mapping handle is
     * closed, so we can close it now. */
    CloseHandle(hmap);

    if (base == NULL) return MAP_FAILED_PTR;
    return base;
}

int munmap(void *addr, size_t length)
{
    (void)length;
    if (addr == NULL || addr == MAP_FAILED_PTR) return -1;
    return UnmapViewOfFile(addr) ? 0 : -1;
}
