#define WIN32_LEAN_AND_MEAN

#include <windows.h>

static void write_text(const char* text, DWORD length)
{
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}

static void write_uint(DWORD value)
{
    char digits[16];
    int  count = 0;
    if (value == 0)
    {
        digits[count++] = '0';
    }
    while (value != 0)
    {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    }
    char out[16];
    for (int index = 0; index < count; ++index)
    {
        out[index] = digits[count - 1 - index];
    }
    write_text(out, (DWORD)count);
}

int slopkit_probe_entry(void)
{
    write_text("slopkit-probe ", 14);
    write_uint(GetCurrentProcessId());
    write_text("\n", 1);
    for (;;)
    {
        Sleep(1000);
    }
}
