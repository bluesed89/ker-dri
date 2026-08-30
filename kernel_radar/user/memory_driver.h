#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include "../shared/ioctls.h"

class MemoryReaderDriver {
public:
    HANDLE hDriver = INVALID_HANDLE_VALUE;
    DWORD processId = 0;
    uintptr_t unityPlayerBase = 0;
    size_t unityPlayerSize = 0;

    bool Attach(const char* processName) {
        FILE* logF = fopen("attach_log.txt", "a");
        if (logF) { fprintf(logF, "--- Pure Kernel Driver Attach attempt ---\n"); }

        // 1) Sadece Kernel Driver ile baglanti kur
        if (hDriver == INVALID_HANDLE_VALUE) {
            hDriver = CreateFileA(USER_DEVICE_LINK, GENERIC_READ | GENERIC_WRITE, 
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (hDriver == INVALID_HANDLE_VALUE) {
                if (logF) { fprintf(logF, "Kernel Driver CreateFileA failed, err: %lu\n", GetLastError()); fclose(logF); }
                return false;
            }
        }
        
        // 2) Hedef process ID bul
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            if (logF) { fprintf(logF, "CreateToolhelp32Snapshot (Process) failed\n"); fclose(logF); }
            return false;
        }

        PROCESSENTRY32 pe = { sizeof(pe) };
        if (Process32First(snap, &pe)) {
            do {
                if (_stricmp(pe.szExeFile, processName) == 0) {
                    processId = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);

        if (processId == 0) {
            if (logF) { fprintf(logF, "Process '%s' not found.\n", processName); fclose(logF); }
            return false;
        }

        if (logF) { fprintf(logF, "Found Process: %lu, DriverHandle: %p\n", processId, hDriver); }

        // 3) Kernel Driver uzerinden modul base adresini al
        unityPlayerBase = GetModuleBase("UnityPlayer.dll", unityPlayerSize);
        if (unityPlayerBase == 0) {
            unityPlayerBase = GetModuleBase("GameAssembly.dll", unityPlayerSize);
        }
        if (unityPlayerBase == 0) {
            unityPlayerBase = GetModuleBase("Albion-Online.exe", unityPlayerSize);
        }

        if (unityPlayerBase == 0) {
            if (logF) { fprintf(logF, "No game module base found via Kernel Driver!\n"); fclose(logF); }
            return false;
        }

        if (logF) { fprintf(logF, "SUCCESS! Kernel Driver GameModuleBase: %llx, Size: %zx\n", (unsigned long long)unityPlayerBase, unityPlayerSize); fclose(logF); }
        return true;
    }

    uintptr_t GetModuleBase(const char* moduleName, size_t& outSize) {
        if (hDriver != INVALID_HANDLE_VALUE && processId != 0) {
            MODULE_BASE_REQUEST req = { 0 };
            req.ProcessId = processId;
            mbstowcs(req.ModuleName, moduleName, 255);

            DWORD bytesReturned = 0;
            if (DeviceIoControl(hDriver, IOCTL_GET_MODULE_BASE, &req, sizeof(req), &req, sizeof(req), &bytesReturned, NULL)) {
                if (req.BaseAddress != 0) {
                    outSize = (size_t)req.ModuleSize;
                    if (outSize == 0) outSize = 0x5000000;
                    return (uintptr_t)req.BaseAddress;
                }
            }
        }
        return 0;
    }

    // Yalnızca Kernel Driver üzerinden bellek okuma
    bool ReadRaw(uintptr_t address, void* buffer, size_t size) {
        if (hDriver == INVALID_HANDLE_VALUE || processId == 0 || address == 0 || buffer == nullptr || size == 0) return false;

        READ_MEMORY_REQUEST req = { 0 };
        req.ProcessId = processId;
        req.Address = address;
        req.Size = size;
        req.OutputBuffer = reinterpret_cast<ULONGLONG>(buffer);

        DWORD bytesReturned = 0;
        return DeviceIoControl(hDriver, IOCTL_READ_MEMORY, &req, sizeof(req), &req, sizeof(req), &bytesReturned, NULL);
    }

    template <typename T>
    T Read(uintptr_t address) {
        T value{};
        ReadRaw(address, &value, sizeof(T));
        return value;
    }

    std::string ReadString(uintptr_t address, size_t length = 32) {
        std::vector<char> buf(length + 1, 0);
        ReadRaw(address, buf.data(), length);
        return std::string(buf.data());
    }

    void Close() {
        if (hDriver != INVALID_HANDLE_VALUE) {
            CloseHandle(hDriver);
            hDriver = INVALID_HANDLE_VALUE;
        }
        processId = 0;
        unityPlayerBase = 0;
    }

    uintptr_t FindPattern(uintptr_t moduleBase, size_t moduleSize, const char* signature) {
        auto patternToByte = [](const char* pattern) {
            std::vector<int> bytes;
            char* start = const_cast<char*>(pattern);
            char* end = const_cast<char*>(pattern) + strlen(pattern);
            for (char* current = start; current < end; ++current) {
                if (*current == '?') {
                    ++current;
                    if (*current == '?') ++current;
                    bytes.push_back(-1);
                } else {
                    bytes.push_back(strtoul(current, &current, 16));
                }
            }
            return bytes;
        };

        std::vector<int> patternBytes = patternToByte(signature);
        size_t patternSize = patternBytes.size();
        
        const size_t chunkSize = 4096 * 16; // 64 KB chunks
        std::vector<uint8_t> chunk(chunkSize);
        
        for (size_t i = 0; i < moduleSize; i += chunkSize - patternSize) {
            size_t readSize = (moduleSize - i < chunkSize) ? (moduleSize - i) : chunkSize;
            if (ReadRaw(moduleBase + i, chunk.data(), readSize)) {
                for (size_t j = 0; j <= readSize - patternSize; ++j) {
                    bool found = true;
                    for (size_t k = 0; k < patternSize; ++k) {
                        if (patternBytes[k] != -1 && chunk[j + k] != patternBytes[k]) {
                            found = false;
                            break;
                        }
                    }
                    if (found) {
                        return moduleBase + i + j;
                    }
                }
            } else {
                // Fallback: Read page-by-page (4KB) if large chunk read failed
                for (size_t p = 0; p < readSize; p += 4096) {
                    size_t pageReadSize = (readSize - p < 4096) ? (readSize - p) : 4096;
                    std::vector<uint8_t> pageBuf(pageReadSize);
                    if (ReadRaw(moduleBase + i + p, pageBuf.data(), pageReadSize)) {
                        if (pageReadSize >= patternSize) {
                            for (size_t j = 0; j <= pageReadSize - patternSize; ++j) {
                                bool found = true;
                                for (size_t k = 0; k < patternSize; ++k) {
                                    if (patternBytes[k] != -1 && pageBuf[j + k] != patternBytes[k]) {
                                        found = false;
                                        break;
                                    }
                                }
                                if (found) {
                                    return moduleBase + i + p + j;
                                }
                            }
                        }
                    }
                }
            }
        }
        return 0; 
    }
};
