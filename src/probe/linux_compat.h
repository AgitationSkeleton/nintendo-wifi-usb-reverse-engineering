#ifndef NWC_LINUX_COMPAT_H
#define NWC_LINUX_COMPAT_H
#include <time.h>
#include <unistd.h>
static inline unsigned long GetTickCount(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (unsigned long)(ts.tv_sec*1000UL + ts.tv_nsec/1000000UL);
}
static inline void Sleep(unsigned int ms){ usleep((useconds_t)ms*1000u); }
#endif
