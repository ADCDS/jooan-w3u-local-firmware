/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "jipc_watchdog.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static int fail_stage,stage,closed,actual_timeout=180,initial_writes,feeds;
int __wrap_open(const char *name,int flags,...)
{assert(!strcmp(name,"/dev/watchdog")&&flags==(O_RDWR|O_CLOEXEC));if(fail_stage==-1){errno=EBUSY;return -1;}return 19;}
int __wrap_open64(const char *name,int flags,...){return __wrap_open(name,flags);}
int __wrap_close(int fd){assert(fd==19);++closed;return 0;}
ssize_t __wrap_write(int fd,const void *p,size_t n)
{assert(fd==19&&n==1&&*(const char *)p==0);++initial_writes;if(fail_stage==5){errno=EIO;return -1;}return 1;}
int __wrap_ioctl(int fd,unsigned long request,...)
{
    assert(fd==19);++stage;if(stage==fail_stage){errno=EIO;return -1;}
    if(request==0x40045705ul){++feeds;return 0;}
    va_list ap;va_start(ap,request);int32_t *p=va_arg(ap,int32_t *);va_end(ap);
    if(request==0x40045704ul)assert(*p==2);
    else if(request==0xc0045706ul)assert(*p==180);
    else{assert(request==0x40045707ul);*p=actual_timeout;}
    return 0;
}
int main(void)
{
    assert(jipc_watchdog_open()==19&&initial_writes==1&&feeds==1&&!closed);
    assert(!jipc_watchdog_feed(19)&&feeds==2);
    fail_stage=-1;assert(jipc_watchdog_open()<0&&errno==EBUSY&&!closed);
    for(int failure=1;failure<=5;++failure){fail_stage=failure;stage=0;int before=closed;assert(jipc_watchdog_open()<0&&errno==EIO&&closed==before+1);}
    fail_stage=0;stage=0;actual_timeout=90;int before=closed;
    assert(jipc_watchdog_open()<0&&errno==EPROTO&&closed==before+1);
    puts("JA-A12 watchdog: exact 180-second protocol, exclusive ownership and failure cleanup passed");return 0;
}
