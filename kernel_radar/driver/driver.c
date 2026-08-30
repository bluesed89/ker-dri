#include <ntifs.h>
#include <windef.h>
#include <ntstrsafe.h>
#include "../shared/ioctls.h"

// MmCopyVirtualMemory ve IoCreateDriver için dışa aktarımlar (ntoskrnl.exe)
NTKERNELAPI NTSTATUS NTAPI MmCopyVirtualMemory(
    PEPROCESS SourceProcess,
    PVOID SourceAddress,
    PEPROCESS TargetProcess,
    PVOID TargetAddress,
    SIZE_T BufferSize,
    KPROCESSOR_MODE PreviousMode,
    PSIZE_T ReturnSize
);

NTKERNELAPI NTSTATUS NTAPI IoCreateDriver(
    PUNICODE_STRING DriverName OPTIONAL,
    PDRIVER_INITIALIZE InitializationFunction
);

NTKERNELAPI PVOID NTAPI PsGetProcessSectionBaseAddress(
    PEPROCESS Process
);

// Fonksiyon Prototipleri
NTSTATUS RealDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
DRIVER_UNLOAD DriverUnload;
NTSTATUS DeviceCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

// ── Unload ──────────────────────────────────────────────────────────
VOID DriverUnload(PDRIVER_OBJECT DriverObject) {
    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, SYM_LINK_NAME);
    IoDeleteSymbolicLink(&symLink);
    if (DriverObject && DriverObject->DeviceObject) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }
    DbgPrintEx(0, 0, "[LO_RADAR] Driver unloaded.\n");
}

// ── Create/Close ────────────────────────────────────────────────────
NTSTATUS DeviceCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

typedef struct _LDR_DATA_TABLE_ENTRY_COMPAT {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} LDR_DATA_TABLE_ENTRY_COMPAT, *PLDR_DATA_TABLE_ENTRY_COMPAT;

typedef struct _PEB_LDR_DATA_COMPAT {
    ULONG Length;
    BOOLEAN Initialized;
    HANDLE SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
} PEB_LDR_DATA_COMPAT, *PPEB_LDR_DATA_COMPAT;

typedef struct _PEB_COMPAT {
    BOOLEAN InheritedAddressSpace;
    BOOLEAN ReadImageFileExecOptions;
    BOOLEAN BeingDebugged;
    BOOLEAN BitField;
    HANDLE Mutant;
    PVOID ImageBaseAddress;
    PPEB_LDR_DATA_COMPAT Ldr;
} PEB_COMPAT, *PPEB_COMPAT;

NTKERNELAPI PPEB NTAPI PsGetProcessPeb(PEPROCESS Process);
NTKERNELAPI VOID NTAPI KeStackAttachProcess(PRKPROCESS Process, PKAPC_STATE ApcState);
NTKERNELAPI VOID NTAPI KeUnstackDetachProcess(PKAPC_STATE ApcState);

NTSTATUS GetModuleBaseInProcess(PEPROCESS Process, PCWSTR ModuleName, PULONGLONG OutBase, PULONGLONG OutSize) {
    if (!Process || !OutBase || !OutSize) return STATUS_INVALID_PARAMETER;

    PVOID sectionBase = PsGetProcessSectionBaseAddress(Process);
    *OutBase = (ULONGLONG)sectionBase;
    *OutSize = 0x8000000;

    if (!ModuleName || ModuleName[0] == L'\0') {
        return STATUS_SUCCESS;
    }

    PPEB_COMPAT peb = (PPEB_COMPAT)PsGetProcessPeb(Process);
    if (!peb) return STATUS_SUCCESS;

    KAPC_STATE apcState;
    KeStackAttachProcess((PRKPROCESS)Process, &apcState);

    UNICODE_STRING targetName;
    RtlInitUnicodeString(&targetName, ModuleName);

    __try {
        PPEB_LDR_DATA_COMPAT ldr = peb->Ldr;
        if (ldr) {
            PLIST_ENTRY head = &ldr->InLoadOrderModuleList;
            PLIST_ENTRY curr = head->Flink;

            while (curr && curr != head) {
                PLDR_DATA_TABLE_ENTRY_COMPAT entry = CONTAINING_RECORD(curr, LDR_DATA_TABLE_ENTRY_COMPAT, InLoadOrderLinks);
                if (entry && entry->BaseDllName.Buffer && entry->BaseDllName.Length > 0) {
                    if (RtlCompareUnicodeString(&entry->BaseDllName, &targetName, TRUE) == 0) {
                        *OutBase = (ULONGLONG)entry->DllBase;
                        *OutSize = (ULONGLONG)entry->SizeOfImage;
                        KeUnstackDetachProcess(&apcState);
                        return STATUS_SUCCESS;
                    }
                }
                curr = curr->Flink;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    KeUnstackDetachProcess(&apcState);
    return STATUS_SUCCESS;
}

// ── IOCTL Handler ───────────────────────────────────────────────────
NTSTATUS DeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG bytesIO = 0;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_READ_MEMORY) {
        PREAD_MEMORY_REQUEST request = (PREAD_MEMORY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        if (request && stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(READ_MEMORY_REQUEST)) {
            PEPROCESS targetProcess = NULL;
            status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)request->ProcessId, &targetProcess);
            
            if (NT_SUCCESS(status)) {
                SIZE_T bytesRead = 0;
                __try {
                    // Kernel'den user mode'a bellek kopyalama işlemi
                    status = MmCopyVirtualMemory(
                        targetProcess,
                        (PVOID)request->Address,
                        PsGetCurrentProcess(),
                        (PVOID)request->OutputBuffer,
                        request->Size,
                        KernelMode,
                        &bytesRead
                    );
                    if (NT_SUCCESS(status)) {
                        bytesIO = sizeof(READ_MEMORY_REQUEST);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    status = GetExceptionCode();
                }
                ObDereferenceObject(targetProcess);
            }
        }
    }
    else if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_WRITE_MEMORY) {
        PWRITE_MEMORY_REQUEST request = (PWRITE_MEMORY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        if (request && stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(WRITE_MEMORY_REQUEST)) {
            PEPROCESS targetProcess = NULL;
            status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)request->ProcessId, &targetProcess);
            
            if (NT_SUCCESS(status)) {
                SIZE_T bytesWritten = 0;
                __try {
                    // User mode'dan kernel'a bellek kopyalama işlemi (Zoom/Fog hack için WPM)
                    status = MmCopyVirtualMemory(
                        PsGetCurrentProcess(),
                        (PVOID)request->InputBuffer,
                        targetProcess,
                        (PVOID)request->Address,
                        request->Size,
                        KernelMode,
                        &bytesWritten
                    );
                    if (NT_SUCCESS(status)) {
                        bytesIO = sizeof(WRITE_MEMORY_REQUEST);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    status = GetExceptionCode();
                }
                ObDereferenceObject(targetProcess);
            }
        }
    }
    else if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_GET_MODULE_BASE) {
        PMODULE_BASE_REQUEST request = (PMODULE_BASE_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        if (request && stack->Parameters.DeviceIoControl.InputBufferLength >= sizeof(MODULE_BASE_REQUEST)) {
            PEPROCESS targetProcess = NULL;
            status = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)request->ProcessId, &targetProcess);
            if (NT_SUCCESS(status)) {
                ULONGLONG baseAddr = 0;
                ULONGLONG modSize = 0;
                status = GetModuleBaseInProcess(targetProcess, request->ModuleName, &baseAddr, &modSize);
                if (NT_SUCCESS(status) && baseAddr != 0) {
                    request->BaseAddress = baseAddr;
                    request->ModuleSize = modSize;
                    bytesIO = sizeof(MODULE_BASE_REQUEST);
                } else {
                    status = STATUS_NOT_FOUND;
                }
                ObDereferenceObject(targetProcess);
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesIO;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// ── Real Entry Point (IoCreateDriver tarafından çağrılır) ─────────
NTSTATUS RealDriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING devName, symLink;

    LARGE_INTEGER tick;
    KeQueryTickCount(&tick);

    WCHAR devBuf[64];
    RtlStringCbPrintfW(devBuf, sizeof(devBuf), L"\\Device\\LoRadar_%I64x", tick.QuadPart);

    RtlInitUnicodeString(&devName, devBuf);
    RtlInitUnicodeString(&symLink, SYM_LINK_NAME);

    // Eski bir sembolik link kalmışsa temizle
    IoDeleteSymbolicLink(&symLink);

    status = IoCreateDevice(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &deviceObject
    );

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(0, 0, "[LO_RADAR] IoCreateDevice failed with status 0x%X\n", status);
        return status;
    }

    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(0, 0, "[LO_RADAR] IoCreateSymbolicLink failed with status 0x%X\n", status);
        IoDeleteDevice(deviceObject);
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = DeviceCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DeviceCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    DriverObject->DriverUnload = DriverUnload;

    deviceObject->Flags |= DO_BUFFERED_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrintEx(0, 0, "[LO_RADAR] Driver loaded successfully via IoCreateDriver! For LO <3\n");
    return STATUS_SUCCESS;
}

// ── Kdmapper Entry Point ────────────────────────────────────────────
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrintEx(0, 0, "[LO_RADAR] Kdmapper DriverEntry called. Creating driver object...\n");

    LARGE_INTEGER tick;
    KeQueryTickCount(&tick);

    WCHAR drvNameBuf[64];
    RtlStringCbPrintfW(drvNameBuf, sizeof(drvNameBuf), L"\\Driver\\LoRadarDrv_%I64x", tick.QuadPart);

    UNICODE_STRING driverName;
    RtlInitUnicodeString(&driverName, drvNameBuf);

    NTSTATUS status = IoCreateDriver(&driverName, &RealDriverEntry);
    
    if (NT_SUCCESS(status)) {
        DbgPrintEx(0, 0, "[LO_RADAR] Driver Object created successfully via IoCreateDriver!\n");
    } else {
        DbgPrintEx(0, 0, "[LO_RADAR] IoCreateDriver failed with status: 0x%X\n", status);
    }

    return STATUS_SUCCESS;
}
