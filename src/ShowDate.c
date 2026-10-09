/*
 * ShowDate.exe  -  a tiny harmless test target for the date shim.
 *
 * Prints the date/time as seen through several different APIs, a few times a
 * second. Use it to prove the shim works WITHOUT risking SADiE:
 *
 *   1) Run ShowDate.exe on its own  -> it prints today's real date.
 *   2) Run:  SadieDate.exe "2008-01-01" ShowDate.exe
 *      -> ShowDate now prints 2008, while the Windows taskbar clock still
 *         shows today's real date. That proves the date is faked for this
 *         one process only, and the system clock is untouched.
 *   3) Watch the seconds tick up normally -> the fake clock advances at the
 *      real rate (constant offset), which is what keeps audio timing intact.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    int i;
    printf("ShowDate - what date does THIS process see?\n");
    printf("(The Windows taskbar clock should still show the real date.)\n\n");

    for (i = 0; i < 10; ++i) {
        SYSTEMTIME lt, st;
        FILETIME ft;
        SYSTEMTIME ftst;
        time_t t;

        GetLocalTime(&lt);
        GetSystemTime(&st);
        GetSystemTimeAsFileTime(&ft);
        FileTimeToSystemTime(&ft, &ftst);
        t = time(NULL);

        printf("GetLocalTime            : %04d-%02d-%02d %02d:%02d:%02d\n",
               lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
        printf("GetSystemTime (UTC)     : %04d-%02d-%02d %02d:%02d:%02d\n",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        printf("GetSystemTimeAsFileTime : %04d-%02d-%02d %02d:%02d:%02d\n",
               ftst.wYear, ftst.wMonth, ftst.wDay, ftst.wHour, ftst.wMinute, ftst.wSecond);
        printf("C runtime time()        : %s", ctime(&t));
        printf("----\n");
        Sleep(1000);
    }
    printf("\nDone. Press Enter to close...");
    (void)getchar();
    return 0;
}
