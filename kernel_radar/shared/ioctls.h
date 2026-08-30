#pragma once
#include <minwindef.h>

// Driver Cihaz Adı
#define DEVICE_NAME L"\\Device\\LoRadar3"
#define SYM_LINK_NAME L"\\DosDevices\\Global\\LoRadar3"
#define USER_DEVICE_LINK "\\\\.\\LoRadar3"

// IOCTL Kodları
#define IOCTL_READ_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GET_MODULE_BASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WRITE_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Bellek Okuma İsteği Yapısı
typedef struct _READ_MEMORY_REQUEST {
    ULONG ProcessId;       // Okunacak sürecin ID'si
    ULONGLONG Address;     // Okunacak hedef adres
    ULONGLONG Size;        // Okunacak bayt sayısı
    ULONGLONG OutputBuffer; // Verinin kopyalanacağı User-Mode bellek adresi
} READ_MEMORY_REQUEST, *PREAD_MEMORY_REQUEST;

// Bellek Yazma İsteği Yapısı
typedef struct _WRITE_MEMORY_REQUEST {
    ULONG ProcessId;
    ULONGLONG Address;
    ULONGLONG Size;
    ULONGLONG InputBuffer; // Yazılacak verinin bulunduğu User-Mode bellek adresi
} WRITE_MEMORY_REQUEST, *PWRITE_MEMORY_REQUEST;

// Modül Adresi İstek Yapısı
typedef struct _MODULE_BASE_REQUEST {
    ULONG ProcessId;
    WCHAR ModuleName[256];
    ULONGLONG BaseAddress;
    ULONGLONG ModuleSize;
} MODULE_BASE_REQUEST, *PMODULE_BASE_REQUEST;
