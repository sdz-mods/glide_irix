/*
 * linutil.c - IRIX keyboard input stubs for Glide test programs
 *
 * Provides lin_kbhit() and lin_getch() using POSIX select()/read().
 * Compile alongside tlib.c when building tests with -D__linux__.
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

int
lin_kbhit(void)
{
    struct timeval tv;
    fd_set fds;

    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, (fd_set *)0, (fd_set *)0, &tv) > 0;
}

char
lin_getch(void)
{
    char c = 0;
    read(0, &c, 1);
    return c;
}
