/* Diagnostic: does GetMappedFileNameW report the name of a pagefile-backed section? */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>

int main(void) {
    wchar_t name[512];
    HANDLE named=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,0x100000,
                                     L"bb-guest-memory-probe");
    if (!named) { printf("named create failed %lu\n",(unsigned long)GetLastError()); return 1; }
    void *vn=MapViewOfFileEx(named,FILE_MAP_ALL_ACCESS,0,0,0x100000,NULL);
    DWORD n=vn ? GetMappedFileNameW(GetCurrentProcess(),vn,name,512) : 0;
    printf("named  view=%p n=%lu name=%ls\n",vn,(unsigned long)n,n?name:L"(none)");

    HANDLE anon=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,0x100000,NULL);
    void *va=MapViewOfFileEx(anon,FILE_MAP_ALL_ACCESS,0,0,0x100000,NULL);
    DWORD m=va ? GetMappedFileNameW(GetCurrentProcess(),va,name,512) : 0;
    printf("anon   view=%p n=%lu name=%ls\n",va,(unsigned long)m,m?name:L"(none)");

    /* A second view of the same named section: does it report the same name? */
    void *vn2=MapViewOfFileEx(named,FILE_MAP_ALL_ACCESS,0,0x1000,0x1000,NULL);
    DWORD k=vn2 ? GetMappedFileNameW(GetCurrentProcess(),vn2,name,512) : 0;
    printf("named2 view=%p n=%lu name=%ls\n",vn2,(unsigned long)k,k?name:L"(none)");
    return 0;
}