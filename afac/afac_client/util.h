#ifndef UTIL_H
#define UTIL_H
#include <sys/types.h>
#include <arpa/inet.h>
#include <assert.h>

#define ENDIAN_LITTLE 1
#ifndef HAVE_HTONLL
extern uint64_t htonll(uint64_t);
extern uint64_t ntohll(uint64_t);
#endif
int getpid_afac();

struct name_quota {
char name[16];
int quota;
};


/*******************************************************************/
/**********************   JAY  ***************************************/


#endif
