"""Run the production child-lifecycle function natively on vendor Linux 4.9."""
from pathlib import Path
import gzip,hashlib,json,os,re,stat,struct,subprocess,time
work=Path(__file__).resolve().parent;root=work.parents[1]
source=(work/'source/console-supervisor/src/ports.c').read_text(encoding='utf-8')
body=source[source.index('static int run('):source.index('static bool controls_environment')]
fixture='''#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
typedef uint64_t Uint64;
static volatile sig_atomic_t stopping;
static void stopped(int value){(void)value;stopping=1;}
static Uint64 SDL_GetTicks64(void){struct timespec t;assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);return (uint64_t)t.tv_sec*1000U+(uint64_t)t.tv_nsec/1000000U;}
static void SDL_Delay(unsigned int milliseconds){struct timespec t={milliseconds/1000U,(long)(milliseconds%1000U)*1000000L};while(nanosleep(&t,&t)<0&&errno==EINTR){}}
'''+body+'''
int main(void){
 int enabled=0;
 assert(prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0)==0);
 assert(prctl(PR_GET_CHILD_SUBREAPER,&enabled,0,0,0)==0&&enabled==1);
 Uint64 began=SDL_GetTicks64();
 assert(run("normal","")==0);
 assert(waitpid(-1,&(int){0},WNOHANG)<0&&errno==ECHILD);
 assert(SDL_GetTicks64()-began<10000U);
 signal(SIGALRM,stopped);alarm(1);
 assert(run("long","")!=0);alarm(0);
 assert(waitpid(-1,&(int){0},WNOHANG)<0&&errno==ECHILD);
 puts("R50_VENDOR_NATIVE_PROCESS_GUARD_CANCEL_ORPHAN_CLEANUP_PASS");return 0;
}
'''
(work/'test/vendor-process.c').write_text(fixture,encoding='utf-8')
env=dict(os.environ,TEMP=str(root/'work/compiler-tmp'),TMP=str(root/'work/compiler-tmp'),ZIG_GLOBAL_CACHE_DIR=str(root/'work/zig-global'),ZIG_LOCAL_CACHE_DIR=str(work/'zig-cache'))
subprocess.run([str(root/'work/host-tools/ziglang/zig.exe'),'cc','-target','aarch64-linux-musl','-static','-Os',str(work/'test/vendor-process.c'),'-o',str(work/'test/vendor-process')],env=env,check=True,timeout=300)
entries={name:(b'',stat.S_IFDIR|0o755) for name in ['bin','lib','dev','proc','sys','tmp','system','system/bin']}
import tarfile
with tarfile.open(root/'work/moss-image/rootfs-files.tar') as archive:
 for item in archive:
  name=item.name.removeprefix('./')
  if name=='bin/busybox' or (name.startswith('lib/') and '/' not in name[4:] and (item.isfile() or item.issym())):
   entries[name]=((item.linkname.encode() if item.issym() else archive.extractfile(item).read()),(stat.S_IFLNK if item.issym() else stat.S_IFREG)|item.mode)
for name in ['sh','mount','sleep','setsid']:
 entries['bin/'+name]=(b'busybox',stat.S_IFLNK|0o777)
entries['test']=( (work/'test/vendor-process').read_bytes(),stat.S_IFREG|0o755)
entries['system/bin/console-portmaster-launch']=(b'''#!/bin/sh
case "$1" in
 normal)
  setsid /bin/sh -c 'trap "" TERM; sleep 60' &
  sleep 60 &
  exit 0;;
 long) trap '' TERM; sleep 60;;
esac
''',stat.S_IFREG|0o755)
entries['init']=(b'''#!/bin/sh
export PATH=/bin
mount -t devtmpfs dev /dev
mount -t proc proc /proc
mount -t sysfs sys /sys
exec >/dev/kmsg 2>&1
echo 'VENDOR_TEST: PROCESS_BEGIN'
if /test; then echo 'VENDOR_TEST: PROCESS_COMPLETE_PASS'; else echo 'VENDOR_TEST: PROCESS_FAILED'; fi
while :; do sleep 10; done
''',stat.S_IFREG|0o755)
output=bytearray()
for number,(name,(data,mode)) in enumerate(entries.items(),1):
 encoded=name.encode()+b'\0';fields=[number,mode,0,0,1,0,len(data),0,0,0,0,len(encoded),0]
 output+=b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+encoded;output+=b'\0'*(-len(output)%4);output+=data;output+=b'\0'*(-len(output)%4)
name=b'TRAILER!!!\0';fields=[0,0,0,0,1,0,0,0,0,0,0,len(name),0]
output+=b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+name;output+=b'\0'*(-len(output)%4)
init=work/'test/vendor-process.gz';init.write_bytes(gzip.compress(output,compresslevel=1));del output,entries
cmd=[str(root/'work/linux-validation/qemu/qemu-system-aarch64.exe'),'-machine','virt,gic-version=2','-cpu','cortex-a53','-smp','2','-m','512','-display','none','-serial','none','-qmp','stdio','-nic','none','-no-reboot','-kernel',str(root/'work/boot-loop-r19d/vendor-kernel.Image'),'-initrd',str(init),'-append','rdinit=/init loglevel=8 panic=0']
process=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=(work/'test/VENDOR_PROCESS.stderr').open('w'),text=True)
def reply():
 while True:
  line=process.stdout.readline()
  if not line:raise RuntimeError('Native kernel VM exited')
  value=json.loads(line)
  if 'return' in value or 'error' in value:return value
json.loads(process.stdout.readline());process.stdin.write('{"execute":"qmp_capabilities"}\n');process.stdin.flush();reply()
text=''
try:
 for attempt in range(18):
  time.sleep(10)
  ram=work/'test/vendor-process-ram.bin'
  request={'execute':'human-monitor-command','arguments':{'command-line':f'pmemsave 0x40000000 0x1400000 "{ram.as_posix()}"'}}
  process.stdin.write(json.dumps(request)+'\n');process.stdin.flush();reply()
  raw=ram.read_bytes();hits=[m.start() for m in re.finditer(b'Booting Linux on physical CPU 0x0',raw)]
  messages=[]
  if hits:
   position=hits[-1]-16
   for _ in range(3000):
    length,textlen=struct.unpack_from('<HH',raw,position+8)
    if length<16 or length>8192 or textlen>length-16:break
    messages.append(raw[position+16:position+16+textlen].decode(errors='replace'));position+=length
  text='\n'.join(messages);(work/'test/VENDOR_PROCESS.log').write_text(text,encoding='utf-8')
  print('VENDOR_PROCESS_CHECK',attempt,[m for m in messages if 'VENDOR_TEST:' in m],flush=True)
  if 'PROCESS_COMPLETE_PASS' in text or 'PROCESS_FAILED' in text or 'Kernel panic' in text:break
finally:
 process.stdin.write('{"execute":"quit"}\n');process.stdin.flush();process.wait(timeout=15)
passed='VENDOR_TEST: PROCESS_COMPLETE_PASS' in text and 'R50_VENDOR_NATIVE_PROCESS_GUARD_CANCEL_ORPHAN_CLEANUP_PASS' in text
report={'passed':passed,'kernel':'Original vendor 4.9.191','qemu_user_used':False,'subreaper_guard_weakened':False,'exact_production_run_sha256':hashlib.sha256(body.encode()).hexdigest(),'physical_handheld':False}
(work/'test/VENDOR_PROCESS.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
assert passed,text[-3500:]
print('ORIGINAL_KERNEL_NATIVE_LIFECYCLE_PASS',flush=True)
