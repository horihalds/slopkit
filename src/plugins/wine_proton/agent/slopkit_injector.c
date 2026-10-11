#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <tlhelp32.h>

#include "slopkit_agent_protocol.h"

#define SLOPKIT_INJECTOR_MAX_ARGS    16
#define SLOPKIT_INJECTOR_PATH_CHARS  1024
#define SLOPKIT_INJECTOR_MAX_MATCHES 32
#define SLOPKIT_INJECTOR_WAIT_MS     15000u
#define SLOPKIT_INJECTOR_SNAPSHOT_TRIES 8u

/* -nostdlib leaves no CRT; the compiler still emits calls to these. */
void* memset(void* destination, int value, size_t count)
{
    unsigned char* bytes = (unsigned char*)destination;
    size_t         index = 0;
    for (; index < count; ++index)
    {
        bytes[index] = (unsigned char)value;
    }
    return destination;
}

void* memcpy(void* destination, const void* source, size_t count)
{
    unsigned char*       out = (unsigned char*)destination;
    const unsigned char* in  = (const unsigned char*)source;
    size_t               index = 0;
    for (; index < count; ++index)
    {
        out[index] = in[index];
    }
    return destination;
}

void* memmove(void* destination, const void* source, size_t count)
{
    unsigned char*       out = (unsigned char*)destination;
    const unsigned char* in  = (const unsigned char*)source;
    if (out < in)
    {
        size_t index = 0;
        for (; index < count; ++index)
        {
            out[index] = in[index];
        }
    }
    else if (out > in)
    {
        size_t index = count;
        while (index > 0)
        {
            --index;
            out[index] = in[index];
        }
    }
    return destination;
}

static size_t text_length(const wchar_t* text)
{
    size_t length = 0;
    while (text[length] != L'\0')
    {
        ++length;
    }
    return length;
}

static wchar_t* text_copy(wchar_t* destination, const wchar_t* source)
{
    wchar_t* cursor = destination;
    while ((*cursor = *source) != L'\0')
    {
        ++cursor;
        ++source;
    }
    return destination;
}

static int text_equal_ci(const wchar_t* left, const wchar_t* right)
{
    while (*left != L'\0' && *right != L'\0')
    {
        wchar_t a = *left;
        wchar_t b = *right;
        if (a >= L'A' && a <= L'Z')
        {
            a = (wchar_t)(a - L'A' + L'a');
        }
        if (b >= L'A' && b <= L'Z')
        {
            b = (wchar_t)(b - L'A' + L'a');
        }
        if (a != b)
        {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

static const wchar_t* text_basename(const wchar_t* path)
{
    const wchar_t* base = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor)
    {
        if (*cursor == L'\\' || *cursor == L'/')
        {
            base = cursor + 1;
        }
    }
    return base;
}

/* A Linux path becomes the Z: drive form Wine sees, and separators become
   backslashes. A path that already names a Windows drive is left alone. */
static void path_normalize(const wchar_t* source, wchar_t* destination, size_t capacity)
{
    size_t out = 0;
    if (source[0] == L'/' && capacity >= 3)
    {
        destination[out++] = L'Z';
        destination[out++] = L':';
    }
    for (size_t index = 0; source[index] != L'\0' && out + 1 < capacity; ++index)
    {
        destination[out++] = source[index] == L'/' ? L'\\' : source[index];
    }
    destination[out] = L'\0';
}

static wchar_t* path_append(wchar_t* buffer, size_t capacity, const wchar_t* text)
{
    size_t used = text_length(buffer);
    while (*text != L'\0' && used + 1 < capacity)
    {
        buffer[used++] = *text++;
    }
    buffer[used] = L'\0';
    return buffer;
}

static wchar_t* path_append_number(wchar_t* buffer, size_t capacity, uint64_t value, int uppercase_hex)
{
    wchar_t digits[32];
    int     count = 0;
    const wchar_t* alphabet = uppercase_hex ? L"0123456789ABCDEF" : L"0123456789abcdef";
    const uint64_t base     = uppercase_hex ? 16u : 10u;
    if (value == 0)
    {
        digits[count++] = L'0';
    }
    while (value != 0 && count < 32)
    {
        digits[count++] = alphabet[value % base];
        value /= base;
    }
    while (count > 0)
    {
        path_append(buffer, capacity, (wchar_t[2]) {digits[--count], L'\0'});
    }
    return buffer;
}

static void write_handle(HANDLE handle, const wchar_t* text)
{
    wchar_t chunk[512];
    size_t  offset = 0;
    for (;;)
    {
        size_t count = 0;
        while (text[offset + count] != L'\0' && count + 1 < 512)
        {
            chunk[count] = text[offset + count];
            ++count;
        }
        chunk[count] = L'\0';
        char utf8[2048];
        int  converted = WideCharToMultiByte(CP_UTF8, 0, chunk, -1, utf8, (int)sizeof(utf8), NULL, NULL);
        if (converted > 1)
        {
            DWORD written = 0;
            WriteFile(handle, utf8, (DWORD)(converted - 1), &written, NULL);
        }
        if (text[offset + count] == L'\0')
        {
            break;
        }
        offset += count;
    }
}

static void write_line(HANDLE handle, const wchar_t* text)
{
    write_handle(handle, text);
    write_handle(handle, L"\n");
}

/* Split a command line into arguments, honouring double quotes. Returns the
   count; the pieces point into `line`. */
static int split_arguments(wchar_t* line, wchar_t** arguments, int capacity)
{
    int      count  = 0;
    wchar_t* cursor = line;
    while (*cursor != L'\0' && count < capacity)
    {
        while (*cursor == L' ' || *cursor == L'\t')
        {
            ++cursor;
        }
        if (*cursor == L'\0')
        {
            break;
        }
        wchar_t* start;
        if (*cursor == L'"')
        {
            ++cursor;
            start = cursor;
            while (*cursor != L'\0' && *cursor != L'"')
            {
                ++cursor;
            }
        }
        else
        {
            start = cursor;
            while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t')
            {
                ++cursor;
            }
        }
        if (*cursor != L'\0')
        {
            *cursor = L'\0';
            ++cursor;
        }
        arguments[count++] = start;
    }
    return count;
}

typedef struct slopkit_candidate
{
    DWORD   pid;
    wchar_t path[SLOPKIT_INJECTOR_PATH_CHARS];
} slopkit_candidate;

static int process_image_path(DWORD pid, wchar_t* destination, size_t capacity, wchar_t* fallback)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != NULL)
    {
        DWORD  size = (DWORD)capacity;
        if (QueryFullProcessImageNameW(process, 0, destination, &size) != 0 && size > 0)
        {
            CloseHandle(process);
            return 1;
        }
        CloseHandle(process);
    }
    if (fallback != NULL && fallback[0] != L'\0')
    {
        text_copy(destination, fallback);
        return 1;
    }
    return 0;
}

/* Find the target: an exact image-path match first, then a basename match.
   Prints the candidate list and returns 2 when the match is ambiguous or when
   nothing matched, and 0 with *pid_out set on success. The candidate arrays
   are heap-allocated so the thread's 1 MiB stack is never at risk. */
static int find_target(const wchar_t* wanted, DWORD* pid_out)
{
    slopkit_candidate* const exact = (slopkit_candidate*)HeapAlloc(
        GetProcessHeap(), 0, sizeof(slopkit_candidate) * SLOPKIT_INJECTOR_MAX_MATCHES);
    slopkit_candidate* const loose = (slopkit_candidate*)HeapAlloc(
        GetProcessHeap(), 0, sizeof(slopkit_candidate) * SLOPKIT_INJECTOR_MAX_MATCHES);
    if (exact == NULL || loose == NULL)
    {
        HeapFree(GetProcessHeap(), 0, exact);
        HeapFree(GetProcessHeap(), 0, loose);
        write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: out of memory");
        return 3;
    }
    int               exact_count = 0;
    int               loose_count = 0;
    const DWORD       self        = GetCurrentProcessId();
    const wchar_t*    wanted_base = text_basename(wanted);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: no process snapshot");
        HeapFree(GetProcessHeap(), 0, exact);
        HeapFree(GetProcessHeap(), 0, loose);
        return 3;
    }
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == self)
            {
                continue;
            }
            wchar_t path[SLOPKIT_INJECTOR_PATH_CHARS];
            path[0] = L'\0';
            if (!process_image_path(entry.th32ProcessID, path, SLOPKIT_INJECTOR_PATH_CHARS, entry.szExeFile))
            {
                continue;
            }
            slopkit_candidate* bucket       = NULL;
            int*               bucket_count = NULL;
            if (text_equal_ci(path, wanted))
            {
                bucket       = exact;
                bucket_count = &exact_count;
            }
            else if (text_equal_ci(text_basename(path), wanted_base))
            {
                bucket       = loose;
                bucket_count = &loose_count;
            }
            if (bucket != NULL && *bucket_count < SLOPKIT_INJECTOR_MAX_MATCHES)
            {
                bucket[*bucket_count].pid = entry.th32ProcessID;
                text_copy(bucket[*bucket_count].path, path);
                ++*bucket_count;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    const slopkit_candidate* matches = exact_count > 0 ? exact : loose;
    const int                count   = exact_count > 0 ? exact_count : loose_count;
    int                      result  = 0;
    if (count == 0)
    {
        wchar_t message[SLOPKIT_INJECTOR_PATH_CHARS + 64];
        text_copy(message, L"slopkit-injector: no process matches image ");
        path_append(message, sizeof(message) / sizeof(message[0]), wanted);
        write_line(GetStdHandle(STD_ERROR_HANDLE), message);
        result = 2;
    }
    else if (count > 1)
    {
        write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: ambiguous image, candidates:");
        for (int index = 0; index < count; ++index)
        {
            wchar_t line[SLOPKIT_INJECTOR_PATH_CHARS + 32];
            line[0] = L'\0';
            path_append(line, sizeof(line) / sizeof(line[0]), L"  ");
            path_append_number(line, sizeof(line) / sizeof(line[0]), matches[index].pid, 0);
            path_append(line, sizeof(line) / sizeof(line[0]), L"  ");
            path_append(line, sizeof(line) / sizeof(line[0]), matches[index].path);
            write_line(GetStdHandle(STD_ERROR_HANDLE), line);
        }
        result = 2;
    }
    else
    {
        *pid_out = matches[0].pid;
    }

    HeapFree(GetProcessHeap(), 0, exact);
    HeapFree(GetProcessHeap(), 0, loose);
    return result;
}

/* 1 when a module whose file name is the agent DLL is already loaded. */
static int agent_module_present(DWORD pid)
{
    for (unsigned attempt = 0; attempt < SLOPKIT_INJECTOR_SNAPSHOT_TRIES; ++attempt)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_BAD_LENGTH)
            {
                continue;
            }
            return 0;
        }
        MODULEENTRY32W module;
        memset(&module, 0, sizeof(module));
        module.dwSize = sizeof(module);
        int found = 0;
        if (Module32FirstW(snapshot, &module))
        {
            do
            {
                if (text_equal_ci(module.szModule, L"slopkit-agent.dll"))
                {
                    found = 1;
                    break;
                }
            } while (Module32NextW(snapshot, &module));
        }
        CloseHandle(snapshot);
        return found;
    }
    return 0;
}

static int list_processes(void)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: no process snapshot");
        return 3;
    }
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            wchar_t path[SLOPKIT_INJECTOR_PATH_CHARS];
            path[0] = L'\0';
            if (!process_image_path(entry.th32ProcessID, path, SLOPKIT_INJECTOR_PATH_CHARS, entry.szExeFile))
            {
                text_copy(path, entry.szExeFile);
            }
            wchar_t line[SLOPKIT_INJECTOR_PATH_CHARS + 32];
            line[0] = L'\0';
            path_append_number(line, sizeof(line) / sizeof(line[0]), entry.th32ProcessID, 0);
            path_append(line, sizeof(line) / sizeof(line[0]), L"  ");
            path_append(line, sizeof(line) / sizeof(line[0]), path);
            write_line(GetStdHandle(STD_OUTPUT_HANDLE), line);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

static int inject(DWORD pid, const wchar_t* dll_path)
{
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                         PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (process == NULL)
    {
        wchar_t message[128];
        text_copy(message, L"slopkit-injector: OpenProcess failed, error ");
        path_append_number(message, sizeof(message) / sizeof(message[0]), GetLastError(), 0);
        write_line(GetStdHandle(STD_ERROR_HANDLE), message);
        return 3;
    }

    const size_t  bytes    = (text_length(dll_path) + 1) * sizeof(wchar_t);
    void*         remote   = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int           result   = 3;
    if (remote == NULL)
    {
        write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: VirtualAllocEx failed");
    }
    else if (!WriteProcessMemory(process, remote, dll_path, bytes, NULL))
    {
        write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: WriteProcessMemory failed");
    }
    else
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        LPTHREAD_START_ROUTINE load = kernel32 != NULL
                                          ? (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(kernel32, "LoadLibraryW")
                                          : NULL;
        if (load == NULL)
        {
            write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: LoadLibraryW not found");
        }
        else
        {
            HANDLE thread = CreateRemoteThread(process, NULL, 0, load, remote, 0, NULL);
            if (thread == NULL)
            {
                write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: CreateRemoteThread failed");
            }
            else
            {
                const DWORD wait = WaitForSingleObject(thread, SLOPKIT_INJECTOR_WAIT_MS);
                DWORD       module = 0;
                if (wait != WAIT_OBJECT_0)
                {
                    write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: the remote load timed out");
                }
                else if (!GetExitCodeThread(thread, &module) || module == 0)
                {
                    write_line(GetStdHandle(STD_ERROR_HANDLE), L"slopkit-injector: the remote LoadLibrary returned no module");
                }
                else
                {
                    wchar_t message[16 + SLOPKIT_INJECTOR_PATH_CHARS + 32];
                    text_copy(message, L"slopkit-injector: loaded into pid ");
                    path_append_number(message, sizeof(message) / sizeof(message[0]), pid, 0);
                    path_append(message, sizeof(message) / sizeof(message[0]), L", module 0x");
                    path_append_number(message, sizeof(message) / sizeof(message[0]), module, 1);
                    write_line(GetStdHandle(STD_OUTPUT_HANDLE), message);
                    result = 0;
                }
                CloseHandle(thread);
            }
        }
    }
    if (remote != NULL)
    {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    return result;
}

int slopkit_injector_entry(void)
{
    wchar_t command_line[SLOPKIT_INJECTOR_PATH_CHARS];
    command_line[0] = L'\0';
    const wchar_t* raw = GetCommandLineW();
    if (raw != NULL)
    {
        text_copy(command_line, raw);
    }
    wchar_t* arguments[SLOPKIT_INJECTOR_MAX_ARGS];
    const int count = split_arguments(command_line, arguments, SLOPKIT_INJECTOR_MAX_ARGS);

    wchar_t image[SLOPKIT_INJECTOR_PATH_CHARS];
    wchar_t dll[SLOPKIT_INJECTOR_PATH_CHARS];
    int     list = 0;
    int     have_image = 0;
    int     have_dll   = 0;
    image[0] = L'\0';
    dll[0]   = L'\0';

    for (int index = 1; index < count; ++index)
    {
        const wchar_t* argument = arguments[index];
        if (text_equal_ci(argument, L"--list"))
        {
            list = 1;
        }
        else if (text_equal_ci(argument, L"--image") && index + 1 < count)
        {
            path_normalize(arguments[++index], image, SLOPKIT_INJECTOR_PATH_CHARS);
            have_image = 1;
        }
        else if (text_equal_ci(argument, L"--dll") && index + 1 < count)
        {
            path_normalize(arguments[++index], dll, SLOPKIT_INJECTOR_PATH_CHARS);
            have_dll = 1;
        }
        else
        {
            wchar_t message[SLOPKIT_INJECTOR_PATH_CHARS + 64];
            text_copy(message, L"slopkit-injector: unknown argument ");
            path_append(message, sizeof(message) / sizeof(message[0]), argument);
            write_line(GetStdHandle(STD_ERROR_HANDLE), message);
            ExitProcess(1);
        }
    }

    if (list)
    {
        ExitProcess((UINT)list_processes());
    }
    if (!have_image || !have_dll)
    {
        write_line(GetStdHandle(STD_ERROR_HANDLE),
                   L"usage: slopkit-injector.exe --image <target> --dll <agent.dll> | --list");
        ExitProcess(1);
    }

    DWORD pid = 0;
    const int found = find_target(image, &pid);
    if (found != 0)
    {
        ExitProcess((UINT)found);
    }
    if (agent_module_present(pid))
    {
        wchar_t message[64];
        text_copy(message, L"slopkit-injector: agent already loaded, pid ");
        path_append_number(message, sizeof(message) / sizeof(message[0]), pid, 0);
        write_line(GetStdHandle(STD_OUTPUT_HANDLE), message);
        ExitProcess(0);
    }
    ExitProcess((UINT)inject(pid, dll));
}
