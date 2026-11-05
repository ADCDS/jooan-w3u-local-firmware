/* SPDX-License-Identifier: MIT
 * Retained JA-A12 watchdog ABI. Ownership must be arranged by the launcher. */
#define _GNU_SOURCE
#include "jipc_watchdog.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if (defined(__mips__) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(JIPC_TEST_WATCHDOG)
int jipc_watchdog_feed(int fd)
{
    return ioctl(fd,0x40045705ul,0);
}
int jipc_watchdog_open(void)
{
    int fd=open("/dev/watchdog",O_RDWR|O_CLOEXEC);
    if(fd<0)return -1;
    int32_t enable=2,timeout=180,actual=-1;
    const char initial=0;
    if(ioctl(fd,0x40045704ul,&enable)||ioctl(fd,0xc0045706ul,&timeout)||
       ioctl(fd,0x40045707ul,&actual)||actual!=180||write(fd,&initial,1)!=1||jipc_watchdog_feed(fd)){
        int saved=actual!=180&&actual!=-1?EPROTO:(errno?errno:EIO);
        close(fd);errno=saved;return -1;
    }
    return fd;
}
#else
int jipc_watchdog_open(void){errno=ENOTSUP;return -1;}
int jipc_watchdog_feed(int fd){(void)fd;errno=ENOTSUP;return -1;}
#endif
