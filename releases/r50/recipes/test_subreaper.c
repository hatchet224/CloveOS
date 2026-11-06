/* SPDX-License-Identifier: GPL-3.0-or-later */
/* VM-only x86 launcher: qemu-user does not implement the subreaper prctl. */
#define _GNU_SOURCE
#include <sys/prctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
int main(int argc,char **argv)
{
 int enabled=0;
 if(argc<2 || prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)!=0 ||
    prctl(PR_GET_CHILD_SUBREAPER,&enabled,0,0,0)!=0 || enabled!=1)return 1;
 if(setenv("CLOVE_VM_SUBREAPER","1",1)!=0)return 1;
 execvp(argv[1],argv+1);perror("subreaper exec");return 1;
}
