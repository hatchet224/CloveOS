/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-only allocator fault injection, no production settings policy. */
#define _GNU_SOURCE
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
extern void *__libc_malloc(size_t);
void *malloc(size_t n) {
 if(n>=33554432U && access("/tmp/test-deny-large-alloc",F_OK)==0){
  static const char line[]="TEST_REAL_ALLOCATOR_FAILURE\n";write(2,line,sizeof(line)-1U);errno=ENOMEM;return 0;
 }
 return __libc_malloc(n);
}
