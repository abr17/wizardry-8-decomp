/*
 * Shared in-process crash diagnostics for the runnable Wizardry 8 images.
 *
 * The matching comparison image does not link this unit. The runnable images
 * require complete native links. For an unhandled fault outside a debugger,
 * this filter records the register file and every stack word that points
 * into the main image, and the host-side MAP symbolizer names them.
 */

#include "wiz8_crash_report.h"

#include <windows.h>
#include <stdio.h>

extern "C" char gzErrorMsg[2048]; /* sgp.cpp: error text before exit() */

namespace {

const unsigned int kMaxCandidates = 200;

W8CrashContextWriter g_context_writer = 0;
unsigned long g_main_image_base = 0;
unsigned int g_candidate_count = 0;

int IsMainImageAddress(unsigned long address, unsigned long* offset)
{
    MEMORY_BASIC_INFORMATION memory;

    if (g_main_image_base == 0) {
        return 0;
    }
    if (VirtualQuery((const void*)address, &memory, sizeof(memory)) == 0) {
        return 0;
    }
    if (memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE) {
        return 0;
    }
    if ((unsigned long)memory.AllocationBase != g_main_image_base) {
        return 0;
    }
    *offset = address - g_main_image_base;
    return 1;
}

void ReportCandidate(const char* source, unsigned long address)
{
    unsigned long offset;

    if (g_candidate_count >= kMaxCandidates) {
        return;
    }
    if (!IsMainImageAddress(address, &offset)) {
        return;
    }
    fprintf(stderr, "WIZ8_RUNTIME_CANDIDATE source=%s address=%08lx offset=%08lx\n", source,
            address, offset);
    ++g_candidate_count;
}

void ReportStackCandidates(unsigned long esp)
{
    const unsigned long* stack = (const unsigned long*)esp;
    MEMORY_BASIC_INFORMATION memory;
    unsigned long available;
    unsigned int words;
    unsigned int index;
    char source[24];

    if (VirtualQuery((const void*)esp, &memory, sizeof(memory)) == 0 ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return;
    }
    available = (unsigned long)memory.BaseAddress + memory.RegionSize - esp;
    words = available / sizeof(unsigned long);
    if (words > 8192) {
        words = 8192;
    }
    for (index = 0; index < words; ++index) {
        _snprintf(source, sizeof(source), "stack+%x", index * sizeof(unsigned long));
        source[sizeof(source) - 1] = 0;
        ReportCandidate(source, stack[index]);
    }
}

} // namespace

LONG WINAPI W8ReportUnhandledException(EXCEPTION_POINTERS* exception)
{
    EXCEPTION_RECORD* record = exception->ExceptionRecord;
    CONTEXT* context = exception->ContextRecord;
    const char* operation = "unknown";
    unsigned long access_address = 0;

    /* Mod: a GUI program's stderr goes nowhere, so the report is written to
       Wiz8Runtime.crash.txt and the minidump Wiz8Runtime.dmp next to the exe. */
    {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(0, path, MAX_PATH);
        while (n > 0 && path[n - 1] != '\\') --n;
        path[n] = 0;
        char txt[MAX_PATH], dmp[MAX_PATH];
        _snprintf(txt, MAX_PATH, "%sWiz8Runtime.crash.txt", path);
        _snprintf(dmp, MAX_PATH, "%sWiz8Runtime.dmp", path);
        txt[MAX_PATH - 1] = dmp[MAX_PATH - 1] = 0;
        freopen(txt, "a", stderr);
        typedef BOOL(WINAPI * MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        MiniDumpWriteDump_t write = dbghelp ? (MiniDumpWriteDump_t)GetProcAddress(dbghelp, "MiniDumpWriteDump") : 0;
        HANDLE file = write ? CreateFileA(dmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0) : INVALID_HANDLE_VALUE;
        if (file != INVALID_HANDLE_VALUE) {
            struct { DWORD tid; EXCEPTION_POINTERS* ex; BOOL client; } info = {GetCurrentThreadId(), exception, FALSE};
            write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1 /* MiniDumpWithDataSegs */, &info, 0, 0);
            CloseHandle(file);
        }
    }

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        switch (record->ExceptionInformation[0]) {
        case 0:
            operation = "read";
            break;
        case 1:
            operation = "write";
            break;
        case 8:
            operation = "execute";
            break;
        default:
            operation = "access";
            break;
        }
        access_address = (unsigned long)record->ExceptionInformation[1];
    }

    g_candidate_count = 0;
    g_main_image_base = (unsigned long)GetModuleHandleA(0);

    fprintf(stderr,
            "WIZ8_RUNTIME_CRASH code=%08lx thread=%08lx operation=%s "
            "access=%08lx eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ebx=%08lx "
            "ecx=%08lx edx=%08lx esi=%08lx edi=%08lx\n",
            (unsigned long)record->ExceptionCode, (unsigned long)GetCurrentThreadId(), operation,
            access_address, (unsigned long)context->Eip, (unsigned long)context->Esp,
            (unsigned long)context->Ebp, (unsigned long)context->Eax, (unsigned long)context->Ebx,
            (unsigned long)context->Ecx, (unsigned long)context->Edx, (unsigned long)context->Esi,
            (unsigned long)context->Edi);

    ReportCandidate("reg:eax", (unsigned long)context->Eax);
    ReportCandidate("reg:ebx", (unsigned long)context->Ebx);
    ReportCandidate("reg:ecx", (unsigned long)context->Ecx);
    ReportCandidate("reg:edx", (unsigned long)context->Edx);
    ReportCandidate("reg:esi", (unsigned long)context->Esi);
    ReportCandidate("reg:edi", (unsigned long)context->Edi);
    ReportCandidate("reg:ebp", (unsigned long)context->Ebp);
    ReportStackCandidates((unsigned long)context->Esp);

    if (g_context_writer != 0) {
        g_context_writer(stderr);
    }
    fprintf(stderr, "WIZ8_RUNTIME_CRASH_END\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace {

/* Mod: if the game ends via exit() (not a crash), record who
   called it: return addresses from the stack, their offsets are looked up in Wiz8Runtime.map. */
void LogExitStack()
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(0, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') --n;
    path[n] = 0;
    char txt[MAX_PATH];
    _snprintf(txt, MAX_PATH, "%sWiz8Runtime.exit.txt", path);
    txt[MAX_PATH - 1] = 0;
    if (!freopen(txt, "w", stderr)) return;
    fprintf(stderr, "exit() called; message: %s\n", gzErrorMsg);
    fprintf(stderr, "stack words pointing into the exe:\n");
    g_main_image_base = (unsigned long)GetModuleHandleA(0);
    g_candidate_count = 0;
    volatile unsigned long marker = 0;
    ReportStackCandidates((unsigned long)&marker);
    fflush(stderr);
}

struct CrashReportInstaller {
    CrashReportInstaller()
    {
        SetUnhandledExceptionFilter(W8ReportUnhandledException);
        atexit(LogExitStack);
    }
};

CrashReportInstaller g_crash_report_installer;

} // namespace

void W8SetCrashContextWriter(W8CrashContextWriter writer)
{
    g_context_writer = writer;
}

/* Mod: stack snapshot (addresses inside the exe) to a file next to the exe, on request. */
void W8DumpStackToFile(const char* file_name)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(0, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') --n;
    path[n] = 0;
    char txt[MAX_PATH];
    _snprintf(txt, MAX_PATH, "%s%s", path, file_name);
    txt[MAX_PATH - 1] = 0;
    if (!freopen(txt, "w", stderr)) return;
    g_main_image_base = (unsigned long)GetModuleHandleA(0);
    g_candidate_count = 0;
    volatile unsigned long marker = 0;
    ReportStackCandidates((unsigned long)&marker);
    fflush(stderr);
}