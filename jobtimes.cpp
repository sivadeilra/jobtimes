#include <stdint.h>
#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ProcessInfo {
    HANDLE processHandle{};
    std::wstring imageName;
    ULONGLONG ticksStarted;
};


constexpr uint64_t KIB = 1 << 10;
constexpr uint64_t MIB = 1 << 20;
constexpr uint64_t GIB = 1 << 30;


class FriendlyBytesStr {
public:
    std::string s;

    const char* c_str() const {
        return s.c_str();
    }

    explicit FriendlyBytesStr(uint64_t n) {
        char buffer[60];

        if (n < KIB) {
            sprintf_s(buffer, "%u", (unsigned)n);
        }
        else if (n < MIB) {
            sprintf_s(buffer, "%0.1f KB", (double)n / (double)KIB);
        }
        else if (n < GIB) {
            sprintf_s(buffer, "%0.1f MB", (double)n / (double)MIB);
        }
        else {
            sprintf_s(buffer, "%0.1f GB", (double)n / (double)GIB);
        }
        s = buffer;
    }
};

void ShowError(DWORD error)
{
    PWSTR message = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, // lpSource
        error,
        LANG_NEUTRAL,
        (LPWSTR)&message,
        0,
        nullptr);
    if (len != 0) {
        fprintf(stderr, "%.*ws\n", len, message);
        LocalFree(message);
    }
    else {
        fprintf(stderr, "Unknown error: %u\n", error);
    }
}

[[noreturn]]
void ShowLastErrorAndExit(PCSTR message) {
    DWORD error = GetLastError();
    fprintf(stderr, "error: %s\n", message);
    ShowError(error);
    exit(1);
}

int wmain(int argc, const WCHAR** argv)
{
    LPCWSTR commandLine = GetCommandLineW();

    std::wstring cmd;

    for (int i = 1; i < argc; ++i)
    {
        if (!cmd.empty()) {
            cmd.push_back(L' ');
        }

        std::wstring_view arg{ argv[i] };

        bool needQuotes = arg.find(L' ') != std::wstring::npos;
        if (needQuotes)
        {
            cmd.push_back(L'"');
        }
        cmd.append(arg);
        if (needQuotes) {
            cmd.push_back(L'"');
        }
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        ShowLastErrorAndExit("Failed to create job object.\n");
    }

    HANDLE ioCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (ioCompletionPort == nullptr) {
        ShowLastErrorAndExit("Failed to create I/O completion port.\n");
    }

    JOBOBJECT_ASSOCIATE_COMPLETION_PORT associateCompletionPort{};
    associateCompletionPort.CompletionKey = 0;
    associateCompletionPort.CompletionPort = ioCompletionPort;
    if (!SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &associateCompletionPort, sizeof(associateCompletionPort))) {
        ShowLastErrorAndExit("Failed to associate I/O completion port to job.");
    }


    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION rootProcessInfo{};

    std::wstring cmdCopy{ cmd };

    if (!CreateProcessW(
        nullptr,
        cmdCopy.data(),
        nullptr, // lpProcessAttributes
        nullptr, // lpThreadAttributes
        FALSE, // bInheritHandles
        CREATE_SUSPENDED, // dwCreationFlags,
        nullptr, // lpEnvironment
        nullptr, // lpCurrentDirectory,
        &startupInfo, // lpStartupInfo,
        &rootProcessInfo // lpProcessInformation
    )) {
        ShowLastErrorAndExit("Failed to create process.\n");
        return 1;
    }

    if (!AssignProcessToJobObject(job, rootProcessInfo.hProcess)) {
        ShowLastErrorAndExit("Failed to assign process to job.\n");
        return 1;
    }

    std::unordered_map<DWORD, ProcessInfo> activeProcesses;

    ULONGLONG ticksStarted = GetTickCount64();

    // Manually insert the root process to avoid race conditions on what is
    // often the most important process.
    activeProcesses[rootProcessInfo.dwProcessId] = ProcessInfo{
        rootProcessInfo.hProcess,
        cmd.c_str(),
        ticksStarted,
    };

    ResumeThread(rootProcessInfo.hThread);

    bool quit = false;
    while (!quit) {
        DWORD bytesTransferred;
        ULONG_PTR completionKey;
        LPOVERLAPPED overlapped;
        if (!GetQueuedCompletionStatus(ioCompletionPort, &bytesTransferred, &completionKey, &overlapped, INFINITE)) {
            ShowLastErrorAndExit("Failed to dequeue event from I/O completion port.");
        }

        // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port
        switch (bytesTransferred) {
        case JOB_OBJECT_MSG_NEW_PROCESS: {
            DWORD processId = (DWORD)reinterpret_cast<ULONG_PTR>(overlapped);
            fprintf(stderr, "JOB_OBJECT_MSG_NEW_PROCESS: pid = %u\n", processId);
            if (processId != rootProcessInfo.dwProcessId) {
                HANDLE otherProcessHandle = OpenProcess(GENERIC_READ, FALSE, processId);
                if (otherProcessHandle != nullptr) {

                    WCHAR imageNameBuffer[MAX_PATH + 1];
                    DWORD imageNameLength = GetProcessImageFileNameW(otherProcessHandle, imageNameBuffer, MAX_PATH);
                    imageNameBuffer[imageNameLength] = 0;

                    // Need to add a new entry to the table.
                    activeProcesses[processId] = ProcessInfo{
                        otherProcessHandle,
                        std::wstring{ imageNameBuffer },
                        GetTickCount64()
                    };
                }
                else {
                    fprintf(stderr, "warning: failed to open process %u\n", processId);
                }
            }
            break;
        }

        case JOB_OBJECT_MSG_EXIT_PROCESS: {
            DWORD processId = (DWORD)reinterpret_cast<ULONG_PTR>(overlapped);
            // fprintf(stderr, "JOB_OBJECT_MSG_EXIT_PROCESS: pid = %u\n", processId);

            auto iter = activeProcesses.find(processId);
            if (iter != activeProcesses.end()) {
                ProcessInfo* childInfo = &iter->second;

                fprintf(stderr, "pid %u terminated: %ws\n", processId, childInfo->imageName.c_str());

                // ULONGLONG childTicksEnded = GetTickCount64();
                // ULONGLONG childTicksElapsed = childTicksEnded - childInfo->ticksStarted;

                PROCESS_MEMORY_COUNTERS processMemoryCounters{};

                if (GetProcessMemoryInfo(childInfo->processHandle, &processMemoryCounters, sizeof(processMemoryCounters))) {
                    fprintf(stderr, "    Peak working set: %s\n", FriendlyBytesStr(processMemoryCounters.PeakWorkingSetSize).c_str());
                }
                else {
                    fprintf(stderr, "warning: failed to get process memory info for child process.\n");
                }

                if (processId != rootProcessInfo.dwProcessId) {
                    CloseHandle(childInfo->processHandle);
                }

                activeProcesses.erase(iter);
            }
            else {
                fprintf(stderr, "warning: did not find process %u in active process table\n", processId);
            }

            if (processId == rootProcessInfo.dwProcessId) {
                quit = true;
            }


            break;
        }
        default:
            break;
        }
    }

    WaitForSingleObject(rootProcessInfo.hProcess, INFINITE);

    ULONGLONG ticksEnded = GetTickCount64();
    ULONGLONG ticksElapsed = ticksEnded - ticksStarted;


    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION basicAndIo{};
    DWORD returnLength = 0;
    if (QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation, &basicAndIo, sizeof(basicAndIo), &returnLength)) {

        fprintf(stderr, "Job stats:\n");
        fprintf(stderr, "    Elapsed (wall) time:    %10.3f s\n", (double)ticksElapsed / 1000.0);
        fprintf(stderr, "    Total CPU time:         %10.3f s\n", (double)(basicAndIo.BasicInfo.TotalUserTime.QuadPart + basicAndIo.BasicInfo.TotalKernelTime.QuadPart) / 1.0e7);
        fprintf(stderr, "    User CPU time:          %10.3f s\n", (double)(basicAndIo.BasicInfo.TotalUserTime.QuadPart) / 1.0e7);
        fprintf(stderr, "    Kernel CPU time:        %10.3f s\n", (double)(basicAndIo.BasicInfo.TotalKernelTime.QuadPart) / 1.0e7);
        fprintf(stderr, "Memory:\n");
        fprintf(stderr, "    Total page faults:      %u\n", basicAndIo.BasicInfo.TotalPageFaultCount);
        fprintf(stderr, "I/O:\n");
        fprintf(stderr, "    Reads:   %10I64u ops, %10s bytes\n", basicAndIo.IoInfo.ReadOperationCount, FriendlyBytesStr(basicAndIo.IoInfo.ReadTransferCount).c_str());
        fprintf(stderr, "    Writes:  %10I64u ops, %10s bytes\n", basicAndIo.IoInfo.WriteOperationCount, FriendlyBytesStr(basicAndIo.IoInfo.WriteTransferCount).c_str());
        fprintf(stderr, "    Other:   %10I64u ops, %10s bytes\n", basicAndIo.IoInfo.OtherOperationCount, FriendlyBytesStr(basicAndIo.IoInfo.OtherTransferCount).c_str());

    }
    else {
        DWORD error = GetLastError();
        fprintf(stderr, "Failed to query information from job object.\n");
        ShowError(error);
        return 1;
    }
}

