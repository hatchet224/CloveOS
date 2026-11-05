/* Local, explicitly enabled ROM/BIOS transfer service. No shell/CGI execution. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "transfer_pin.h"

static volatile sig_atomic_t stopping;
static void stop_signal(int sig) { (void)sig; stopping = 1; }
static bool send_all(int fd, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    while (length != 0U && !stopping) {
        ssize_t n = send(fd, p, length, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += (size_t)n; length -= (size_t)n;
    }
    return length == 0U;
}
static void response_headers(int fd, unsigned int code, const char *type, const char *body,
                             const char *extra)
{
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\nConnection: close\r\nContent-Type: %s\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'\r\n%s\r\n",
        code, code == 200U ? "OK" : "Error", type, strlen(body), extra);
    if (n > 0 && (size_t)n < sizeof(header) && send_all(fd, header, (size_t)n))
        (void)send_all(fd, body, strlen(body));
}
static void response(int fd, unsigned int code, const char *type, const char *body)
{
    response_headers(fd,code,type,body,"");
}
static bool paired_cookie(const char *cookies, const char *token)
{
    while (*cookies != '\0') {
        while (*cookies == ' ' || *cookies == ';') ++cookies;
        const char *end = strchr(cookies,';');
        size_t length = end != NULL ? (size_t)(end-cookies) : strlen(cookies);
        /* Existing browser pairings remain valid across the rename. */
        if (length == 47U && (strncmp(cookies,"clove_transfer=",15U) == 0 ||
                             strncmp(cookies,"onion_transfer=",15U) == 0) &&
            memcmp(cookies+15U,token,32U) == 0) return true;
        if (end == NULL) break;
        cookies=end+1;
    }
    return false;
}
struct pairing_state { unsigned int failures; time_t blocked_until; };
static const char pairing_page[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>Pair with CloveOS</title>"
"<style>body{font:18px system-ui;max-width:600px;margin:3em auto;background:#10243a;color:white;padding:1em}input,button{font:inherit;padding:.6em}input{width:6em;letter-spacing:.3em}</style>"
"<h1>Connect to CloveOS</h1><p>Enter the 4-digit PIN shown on its File transfer screen. This browser will remember your handheld.</p>"
"<form id=pair><label>PIN <input id=pin type=password inputmode=numeric pattern='[0-9]{4}' minlength=4 maxlength=4 autocomplete=one-time-code required autofocus></label> "
"<button>Connect</button></form><p id=status role=status></p>"
"<script>document.getElementById('pair').onsubmit=async e=>{e.preventDefault();let s=document.getElementById('status');s.textContent='Connecting…';"
"try{let r=await fetch('/pair',{method:'POST',body:document.getElementById('pin').value});if(r.ok)location.replace('/');else s.textContent=await r.text();}catch(e){s.textContent='Connection interrupted. Check the handheld and try again.';}};</script>";
static void pair_browser(int fd, const char *token, uint64_t length, struct pairing_state *state)
{
    struct timespec clock;
    if (clock_gettime(CLOCK_MONOTONIC,&clock) != 0) {response(fd,503U,"text/plain","Try again.");return;}
    if (clock.tv_sec < state->blocked_until) {response(fd,429U,"text/plain","Too many incorrect PINs. Wait 30 seconds and try again.");return;}
    if (state->blocked_until != 0) {state->failures=0U;state->blocked_until=0;}
    if (length != 4U) {response(fd,400U,"text/plain","Enter exactly 4 digits.");return;}
    char entered[5],expected[5];size_t used=0U;
    while (used < 4U && !stopping) {
        ssize_t n=recv(fd,entered+used,4U-used,0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        used+=(size_t)n;
    }
    if (used != 4U) return;
    entered[4]='\0';transfer_pin(token,expected);
    if (memcmp(entered,expected,4U) != 0) {
        if (++state->failures >= 5U) state->blocked_until=clock.tv_sec+30;
        response(fd,403U,"text/plain","Incorrect PIN. Check the four digits on the handheld.");return;
    }
    state->failures=0U;
    char cookie[192];
    (void)snprintf(cookie,sizeof(cookie),"Set-Cookie: clove_transfer=%s; Max-Age=31536000; Path=/; HttpOnly; SameSite=Strict\r\n",token);
    response_headers(fd,200U,"text/plain","Connected.",cookie);
}
static bool unescape(const char *source, char *target, size_t cap)
{
    size_t used = 0U;
    for (size_t i = 0U; source[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)source[i];
        if (c == '%') {
            unsigned int value;
            char pair[3] = {source[i+1U], 0, 0};
            if (!pair[0] || !source[i+2U]) return false;
            pair[1] = source[i+2U];
            if (strspn(pair, "0123456789abcdefABCDEF") != 2U || sscanf(pair, "%x", &value) != 1) return false;
            c = (unsigned char)value; i += 2U;
        }
        if (c < 32U || c == 127U || c == '\\' || used + 1U >= cap) return false;
        target[used++] = (char)c;
    }
    target[used] = '\0'; return true;
}
static bool component(const char *name)
{
    return name[0] != '\0' && name[0] != '.' && strchr(name, '/') == NULL &&
           strchr(name, '\\') == NULL && strchr(name, ':') == NULL &&
           strlen(name) <= 240U;
}
static int destination(int card, char *path, char **name)
{
    static const char *const systems[] = {"GB","GBC","GBA","FC","SFC","MD","MS","GG","N64","PS","DC","PSP","ARCADE","FBNEO","NEOGEO","PCE","PICO","MAME2003PLUS","ATARI"};
    int parent = -1, sub = -1;
    char *slash = strchr(path, '/');
    if (slash == NULL) return -1;
    *slash++ = '\0';
    if (strcmp(path, "BIOS") == 0) {
        parent = openat(card, "BIOS", O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        *name = slash;
        if (component(slash)) return parent;
    } else if (strcmp(path, "Roms") == 0) {
        char *file = strchr(slash, '/');
        if (file != NULL) {
            *file++ = '\0';
            bool allowed = false;
            for (size_t i = 0U; i < sizeof(systems)/sizeof(systems[0]); ++i)
                if (strcmp(slash, systems[i]) == 0) allowed = true;
            if (allowed && component(file)) {
                parent = openat(card, "Roms", O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
                if (parent >= 0) {
                    if (mkdirat(parent, slash, 0700) != 0 && errno != EEXIST) goto done;
                    sub = openat(parent, slash, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
                    *name = file;
                }
            }
        }
    }
done:
    if (parent >= 0) (void)close(parent);
    return sub;
}
#include "usb_import.h"
static bool usb_command(const char *mode)
{
    bool pc=strcmp(mode,"pc")==0,notified=false,terminated=false;
    if(pc)(void)unlink("/run/console/usb.ready");
    pid_t child=fork();if(child<0)return false;
    if(child==0){execl("/system/bin/console-usb","console-usb",mode,(char *)NULL);_exit(127);}
    int status;pid_t result;
    for(;;){
        result=waitpid(child,&status,WNOHANG);if(result==child)break;
        if(result<0 && errno!=EINTR)break;
        if(pc && !notified && access("/run/console/usb.ready",F_OK)==0){
            (void)send(3,"R",1U,MSG_NOSIGNAL);notified=true;
        }
        struct pollfd parent={.fd=3,.events=0};
        if(poll(&parent,1U,50)>0 && (parent.revents&(POLLHUP|POLLERR))!=0)stopping=1;
        if(stopping && strcmp(mode,"import")==0){
            int cancel=open("/run/console/usb.cancel",O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
            if(cancel>=0)close(cancel);
        }
        if(stopping && pc && !terminated){(void)kill(child,SIGTERM);terminated=true;}
        if(parent.revents==POLLNVAL){struct timespec delay={0,50000000L};nanosleep(&delay,NULL);}
    }
    return result==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
}
static const char page[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>CloveOS file transfer</title><style>body{font:18px system-ui;max-width:700px;margin:3em auto;background:#10243a;color:white;padding:1em}button,select,input{font:inherit;margin:.6em}</style><h1>CloveOS file transfer</h1><p>Keep the handheld on this transfer screen. Stop transfer before playing or powering down.</p><label>Destination <select id=folder><option>Roms/ARCADE</option><option>Roms/ATARI</option><option>Roms/MAME2003PLUS</option><option>Roms/NEOGEO</option><option>Roms/PCE</option><option>Roms/PICO</option><option>Roms/GG</option><option>Roms/GB</option><option>Roms/GBC</option><option>Roms/GBA</option><option>Roms/FC</option><option>Roms/SFC</option><option>Roms/MD</option><option>Roms/MS</option><option>Roms/N64</option><option>Roms/PS</option><option>Roms/PSP</option><option>Roms/DC</option><option>BIOS</option></select></label><p><input id=file type=file multiple><button id=upload onclick=upload()>Upload</button></p>\n"
"<section id=transfer hidden><label for=progress id=current></label>\n"
"<progress id=progress max=100 value=0 style='display:block;width:100%;height:28px;margin:12px 0'></progress>\n"
"<p id=amount></p></section><p id=status role=status aria-live=polite></p>\n"
"<p><a style=color:lightblue href=./diagnostics>Live diagnostics</a></p>\n"
"<script>\n"
"const picker = document.getElementById('file');\n"
"const folder = document.getElementById('folder');\n"
"const button = document.getElementById('upload');\n"
"const progress = document.getElementById('progress');\n"
"const current = document.getElementById('current');\n"
"const amount = document.getElementById('amount');\n"
"const status = document.getElementById('status');\n"
"let busy = false;\n"
"function sendFile(file, destination) {\n"
"  return new Promise((resolve, reject) => {\n"
"    const request = new XMLHttpRequest();\n"
"    request.open('PUT', location.pathname + 'upload/' + destination + '/' + encodeURIComponent(file.name));\n"
"    request.upload.onprogress = event => {\n"
"      if (!event.lengthComputable) return;\n"
"      const percent = event.total ? Math.floor(event.loaded * 100 / event.total) : 0;\n"
"      progress.value = percent;\n"
"      amount.textContent = percent + '% uploaded';\n"
"    };\n"
"    request.upload.onload = () => {\n"
"      progress.value = 100;\n"
"      amount.textContent = '100% uploaded';\n"
"      status.textContent = 'Saving to the card...';\n"
"    };\n"
"    request.onload = () => {\n"
"      if (request.status >= 200 && request.status < 300) resolve();\n"
"      else reject(new Error(request.responseText || 'Handheld returned error ' + request.status));\n"
"    };\n"
"    request.onerror = () => reject(new Error('Connection interrupted. Check the handheld before retrying.'));\n"
"    request.onabort = () => reject(new Error('Upload cancelled.'));\n"
"    request.send(file);\n"
"  });\n"
"}\n"
"async function upload() {\n"
"  if (busy) return;\n"
"  const files = Array.from(picker.files);\n"
"  if (!files.length) { status.textContent = 'Choose a file first.'; return; }\n"
"  const destination = folder.value;\n"
"  busy = true;\n"
"  picker.disabled = folder.disabled = button.disabled = true;\n"
"  document.getElementById('transfer').hidden = false;\n"
"  let saved = 0;\n"
"  try {\n"
"    for (const file of files) {\n"
"      current.textContent = 'File ' + (saved + 1) + ' of ' + files.length + ' - ' + file.name;\n"
"      progress.value = 0;\n"
"      amount.textContent = '0% uploaded';\n"
"      status.textContent = saved + ' of ' + files.length + ' files saved. Uploading...';\n"
"      await sendFile(file, destination);\n"
"      saved++;\n"
"    }\n"
"    progress.value = 100;\n"
"    amount.textContent = '100% uploaded';\n"
"    status.textContent = 'Transfer complete. ' + saved + ' of ' + files.length + ' files saved to the card.';\n"
"  } catch (error) {\n"
"    status.textContent = 'Transfer stopped. ' + saved + ' of ' + files.length + ' files saved. ' + error.message;\n"
"  } finally {\n"
"    busy = false;\n"
"    picker.disabled = folder.disabled = button.disabled = false;\n"
"  }\n"
"}\n"
"</script>\n";
static void diagnostics(int fd, int card)
{
    struct statvfs v;
    char body[512], temp[48] = "unavailable", freq[48] = "unavailable";
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f != NULL) { if (fgets(temp, sizeof(temp), f) == NULL) strcpy(temp,"unavailable"); fclose(f); }
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f != NULL) { if (fgets(freq,sizeof(freq),f) == NULL) strcpy(freq,"unavailable"); fclose(f); }
    (void)snprintf(body,sizeof(body),"CloveOS live diagnostics\nCPU kHz: %s\nTemperature millidegrees C: %s\nCard free bytes: %" PRIu64 "\n",freq,temp,
        fstatvfs(card,&v) == 0 ? (uint64_t)v.f_bavail * (uint64_t)v.f_frsize : 0U);
    response(fd,200U,"text/plain; charset=utf-8",body);
}
static void serve(int fd, int card, const char *token, struct pairing_state *pairing)
{
    char header[8193], method[8], url[2048], version[16], decoded[2048];
    size_t used = 0U;
    uint64_t length = 0U;
    bool have_length = false;
    bool paired=false,foreign_site=false;
    char host[512]="",origin[520]="";
    /* Read only through CRLFCRLF so no body byte is discarded or reinterpreted. */
    while (used < sizeof(header)-1U && !stopping) {
        ssize_t n = recv(fd,header+used,1U,0);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) return;
        ++used;
        if (used >= 4U && memcmp(header+used-4U,"\r\n\r\n",4U) == 0) break;
    }
    header[used] = '\0';
    if (used < 4U || memcmp(header+used-4U,"\r\n\r\n",4U) != 0 ||
        sscanf(header,"%7s %2047s %15s",method,url,version) != 3 || strcmp(version,"HTTP/1.1") != 0) {
        response(fd,400U,"text/plain","Invalid request"); return;
    }
    char *line = strstr(header,"\r\n");
    while (line != NULL && line[2] != '\r') {
        line += 2;
        char *end = strstr(line,"\r\n");
        if (end == NULL) break;
        *end = '\0';
        if (strncasecmp(line,"Content-Length:",15U) == 0) {
            char *value = line+15U, *tail;
            while (*value == ' ') ++value;
            errno = 0; unsigned long long parsed = strtoull(value,&tail,10);
            if (have_length || errno || *value == '\0' || *value == '-' || *tail != '\0') {
                response(fd,400U,"text/plain","Invalid length"); return;
            }
            length = (uint64_t)parsed; have_length = true;
        }
        if (strncasecmp(line,"Transfer-Encoding:",18U) == 0) {
            response(fd,400U,"text/plain","Use a fixed length upload"); return;
        }
        if (strncasecmp(line,"Cookie:",7U) == 0 && paired_cookie(line+7U,token)) paired=true;
        if (strncasecmp(line,"Host:",5U) == 0 || strncasecmp(line,"Origin:",7U) == 0) {
            bool is_host=strncasecmp(line,"Host:",5U)==0;
            char *value=line+(is_host?5U:7U),*target=is_host?host:origin;
            size_t capacity=is_host?sizeof(host):sizeof(origin);
            while (*value==' ') ++value;
            if (*target!='\0' || strlen(value)>=capacity) {response(fd,400U,"text/plain","Invalid request origin.");return;}
            strcpy(target,value);
        }
        if (strncasecmp(line,"Sec-Fetch-Site:",15U)==0) {
            const char *value=line+15U;while(*value==' ')++value;
            if(strcmp(value,"cross-site")==0)foreign_site=true;
        }
        line = end;
    }
    if (strcmp(method,"POST")==0 || strcmp(method,"PUT")==0) {
        char expected_origin[520];(void)snprintf(expected_origin,sizeof(expected_origin),"http://%s",host);
        if (foreign_site || (origin[0]!='\0' && (host[0]=='\0' || strcmp(origin,expected_origin)!=0))) {
            response(fd,403U,"text/plain","Open file transfer directly in your browser.");return;
        }
    }
    if (strcmp(method,"POST")==0 && strcmp(url,"/pair")==0 && have_length) {
        pair_browser(fd,token,length,pairing);return;
    }
    bool legacy=strlen(url)>=34U && url[0]=='/' && memcmp(url+1U,token,32U)==0 && url[33]=='/';
    if (!legacy && !paired) {
        if (strcmp(method,"GET")==0 && strcmp(url,"/")==0) response(fd,200U,"text/html; charset=utf-8",pairing_page);
        else response(fd,403U,"text/plain","Open the short address and enter the 4-digit PIN shown on the handheld.");
        return;
    }
    if (url[0]!='/') {response(fd,400U,"text/plain","Invalid path.");return;}
    const char *route = url+(legacy?34U:1U);
    if (strcmp(method,"GET") == 0 && *route == '\0') { response(fd,200U,"text/html; charset=utf-8",page); return; }
    if (strcmp(method,"GET") == 0 && strcmp(route,"diagnostics") == 0) { diagnostics(fd,card); return; }
    if (strcmp(method,"PUT") != 0 || strncmp(route,"upload/",7U) != 0 || !have_length ||
        length == 0U || length > UINT64_C(0xffffffff) || !unescape(route+7U,decoded,sizeof(decoded))) {
        response(fd,400U,"text/plain","Unsupported request"); return;
    }
    char *name = NULL;
    int parent = destination(card,decoded,&name);
    if (parent < 0) { response(fd,403U,"text/plain","Invalid destination"); return; }
    char temporary[64];
    (void)snprintf(temporary,sizeof(temporary),".upload-%ld",(long)getpid());
    int output = openat(parent,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    bool ok = output >= 0;
    if (ok) {
        unsigned char bytes[65536];
        while (length > 0U && !stopping) {
            size_t take = length > sizeof(bytes) ? sizeof(bytes) : (size_t)length;
            ssize_t n = recv(fd,bytes,take,0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            size_t at = 0U;
            while (at < (size_t)n) {
                ssize_t wrote = write(output,bytes+at,(size_t)n-at);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) { ok = false; break; }
                at += (size_t)wrote;
            }
            if (!ok) break;
            length -= (uint64_t)n;
        }
        ok = ok && length == 0U && !stopping && fdatasync(output) == 0;
        if (close(output) != 0) ok = false;
        if (ok) ok = syscall(SYS_renameat2,parent,temporary,parent,name,RENAME_NOREPLACE) == 0;
        /* FAT does not support fsync(directory) on every vendor kernel.
         * syncfs performs the volume barrier before acknowledging success. */
        if (ok) ok = syncfs(parent) == 0;
        (void)unlinkat(parent,temporary,0);
    }
    (void)close(parent);
    response(fd,ok ? 200U : 409U,"text/plain",ok ? "Saved. Refresh ROMs after stopping transfer." : "Upload incomplete, duplicate filename, or storage error. Check the card before retrying.");
}
int main(int argc, char **argv)
{
    signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal); signal(SIGPIPE,SIG_IGN);
    if(argc==4 && strcmp(argv[1],"--import")==0)return usb_import(argv[2],argv[3]);
    if ((argc != 4 && argc != 5) || strlen(argv[2]) != 32U || strspn(argv[2],"0123456789abcdef") != 32U) return 2;
    bool usb=argc==5 && strcmp(argv[4],"pc")==0,import=argc==5 && strcmp(argv[4],"import")==0;
    if(argc==5 && !usb && !import)return 2;
    if(import)return usb_command("import")?0:1;
    if(usb)return usb_command("pc")?0:1;
    char *tail; unsigned long port = strtoul(argv[3],&tail,10);
    if (*tail || port == 0UL || port > 65535UL) return 2;
    int card = open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    int server = socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    if (card < 0 || server < 0) return 1;
    int reuse = 1; (void)setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in address = {.sin_family=AF_INET,.sin_port=htons((uint16_t)port),.sin_addr={.s_addr=htonl(INADDR_ANY)}};
    if (bind(server,(struct sockaddr *)&address,sizeof(address)) != 0 || listen(server,4) != 0) {
        close(card);close(server);return 1;
    }
    int type = 0; socklen_t type_size = sizeof(type);
    bool managed = getsockopt(3,SOL_SOCKET,SO_TYPE,&type,&type_size) == 0;
    struct pairing_state pairing={0};
    if (managed) (void)send(3,"R",1U,MSG_NOSIGNAL);
    while (!stopping) {
        struct pollfd p[2] = {{.fd=server,.events=POLLIN},{.fd=managed ? 3 : -1,.events=POLLIN}};
        if (poll(p,2U,250) <= 0) continue;
        if (managed && (p[1].revents & (POLLHUP|POLLERR|POLLNVAL)) != 0) break;
        if ((p[0].revents & POLLIN) == 0) continue;
        int client = accept4(server,NULL,NULL,SOCK_CLOEXEC);
        if (client < 0) continue;
        struct timeval timeout = {.tv_sec=2,.tv_usec=0};
        (void)setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        (void)setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        serve(client,card,argv[2],&pairing);
        /* Deliver the reply before draining a rejected request's unread body.
         * Closing immediately with unread TCP data would reset the connection
         * and discard the error text on otherwise valid browser requests. */
        (void)shutdown(client,SHUT_WR);
        unsigned char discard[4096];size_t drained=0U;
        while (!stopping && drained < 65536U) {
            ssize_t n=recv(client,discard,sizeof(discard),0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            drained+=(size_t)n;
        }
        (void)close(client);
    }
    int result = syncfs(card); (void)close(card); (void)close(server);
    return result == 0 ? 0 : 1;
}
