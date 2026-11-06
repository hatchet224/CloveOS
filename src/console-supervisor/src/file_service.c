#include "launcher/i18n.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "supervisor/file_service.h"
#include "supervisor/process.h"
#include "supervisor/wire.h"
#include "transfer_pin.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
/* Keep browser pairing across transfer restarts without exposing the key in URLs. */
static bool transfer_key(const char *card, char token[33])
{
    int root = open(card,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (root < 0) return false;
    int parent = openat(root,".console",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    (void)close(root);
    if (parent < 0) return false;
    bool ok = false;
    int fd = openat(parent,"file-transfer.key",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd >= 0) {
        struct stat st; char bytes[33];
        ok = fstat(fd,&st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1U &&
             st.st_uid == geteuid() && st.st_size == 32 && read(fd,bytes,sizeof(bytes)) == 32;
        (void)close(fd);
        if (ok) { bytes[32] = '\0'; ok = strspn(bytes,"0123456789abcdef") == 32U; if(ok) memcpy(token,bytes,33U); }
    } else if (errno == ENOENT) {
        unsigned char random[16]; char temporary[64];
        if (getrandom(random,sizeof(random),0) != (ssize_t)sizeof(random)) goto done;
        for (size_t i=0U;i<sizeof(random);++i) (void)snprintf(token+i*2U,3U,"%02x",random[i]);
        (void)snprintf(temporary,sizeof(temporary),".transfer-key-%ld.tmp",(long)getpid());
        fd = openat(parent,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if (fd < 0) goto done;
        ok = write(fd,token,32U) == 32 && fdatasync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok) ok = syscall(SYS_renameat2,parent,temporary,parent,"file-transfer.key",RENAME_NOREPLACE) == 0;
        if (ok) ok = syncfs(parent) == 0;
        (void)unlinkat(parent,temporary,0);
    }
done:
    (void)close(parent);return ok;
}
void sc_file_service_stop(struct sc_file_service *s)
{
    if (s->pid > 0) (void)sc_term_once(s->pid,&s->term);
}
void sc_file_service_reaped(struct sc_file_service *s,bool success)
{
    bool imported=s->importing && success;
    char failure[161]="";
    if(!success && s->pc){
        int fd=open("/run/console/usb.status",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        struct stat st;
        if(fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==0U && st.st_nlink==1U){
            ssize_t n=read(fd,failure,sizeof(failure)-1U);
            if(n>0){failure[(size_t)n]='\0';failure[strcspn(failure,"\r\n")]='\0';}
        }
        if(fd>=0)(void)close(fd);
        if(strncmp(failure,"USB setup failed:",17U)!=0 && strncmp(failure,"USB transfer failed:",20U)!=0)
            strcpy(failure,s->ready?"USB transfer stopped unexpectedly. Check the copy in Windows before retrying.":"USB PC connection failed during setup. No upload was started.");
    }
    if (s->fd >= 0) (void)close(s->fd);
    memset(s,0,sizeof(*s)); s->fd = -1;s->imported=imported;
    strcpy(s->failure,failure);
}
void sc_file_service_request(struct sc_file_service *s, const char *card,
    const struct cl_tool_request *r, struct cl_tool_snapshot *out)
{
    memset(out,0,sizeof(*out)); out->kind = CL_TOOL_FILE_TRANSFER; out->ready = true;
    if (r->operation == CL_TOOL_STOP_FILE_TRANSFER) sc_file_service_stop(s);
    if ((r->operation == CL_TOOL_START_FILE_TRANSFER || r->operation==CL_TOOL_START_USB_TRANSFER ||
         r->operation==CL_TOOL_IMPORT_USB_STORAGE) && s->pid <= 0) {
        int pair[2];
        if (!transfer_key(card,s->token) || !sc_pair(pair)) goto failed;
        char *args[] = {"/system/bin/console-fileserver",(char *)card,s->token,"8021",
            r->operation==CL_TOOL_START_USB_TRANSFER?"pc":r->operation==CL_TOOL_IMPORT_USB_STORAGE?"import":NULL,NULL};
        if (!sc_spawn(args[0],args,pair[1],-1,NULL,&s->pid)) {
            (void)close(pair[0]); (void)close(pair[1]); goto failed;
        }
        (void)close(pair[1]); s->fd = pair[0]; s->term = s->ready = false;
        s->failure[0]='\0';
        s->importing=r->operation==CL_TOOL_IMPORT_USB_STORAGE;s->imported=false;
        s->pc=r->operation==CL_TOOL_START_USB_TRANSFER;
    }
    if (s->pid > 0 && !s->ready) {
        char ready;
        if (recv(s->fd,&ready,1U,MSG_DONTWAIT) == 1 && ready == 'R') s->ready = true;
    }
    struct cl_tool_row *row = &out->rows[out->count++];
    strcpy(row->id,s->pid > 0 ? "stop" : "start");
    strcpy(row->label,s->pid > 0 ? "Stop transfer / import" : "Start Wi-Fi file transfer");
    strcpy(row->detail,"ROM/BIOS uploads; stop before starting a game."); row->available = true;
    row = &out->rows[out->count++]; strcpy(row->id,"refresh"); strcpy(row->label,"Refresh address / status"); row->available = true;
    row = &out->rows[out->count++];strcpy(row->id,"usb");strcpy(row->label,"USB connection to PC");
    strcpy(row->detail,"Plug into the OTG port; copy games and BIOS in Windows File Explorer.");row->available=s->pid<=0;
    row = &out->rows[out->count++];strcpy(row->id,"import");strcpy(row->label,"Import from USB storage");
    strcpy(row->detail,"FAT32 drive: Roms/<system> and BIOS. Existing files are kept.");row->available=s->pid<=0;
    if (s->pid > 0 && s->pc && !s->term) {
        strcpy(out->status,s->ready?"USB MTP active. Open This PC > CloveOS, then Games or BIOS. Finish copying before Stop.":"Preparing USB MTP. Press X for status; use the OTG port and a data cable.");
    } else if (s->pid > 0 && s->ready && !s->term) {
        struct ifaddrs *interfaces = NULL;
        if (getifaddrs(&interfaces) == 0) {
            for (struct ifaddrs *it = interfaces; it != NULL && out->count < CL_TOOL_ROWS; it = it->ifa_next) {
                if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET || (it->ifa_flags & IFF_LOOPBACK) != 0U) continue;
                char address[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET,&((struct sockaddr_in *)it->ifa_addr)->sin_addr,address,sizeof(address)) == NULL) continue;
                row = &out->rows[out->count++]; strcpy(row->id,"refresh"); row->available = true;
                (void)snprintf(row->label,sizeof(row->label),"http://%s:8021/",address);
                char pin[5];transfer_pin(s->token,pin);
                (void)snprintf(row->detail,sizeof(row->detail),CL_TR("PIN: %s (enter once per browser)"),pin);
            }
            freeifaddrs(interfaces);
        }
        strcpy(out->status,"Open the short address below. Enter the 4-digit PIN once; your browser remembers this handheld.");
    } else strcpy(out->status,s->pid <= 0 ? (s->failure[0]?s->failure:s->imported?"USB import complete. Press B to return Home and refresh the ROM list.":"Transfers are off. Choose Wi-Fi, PC connection, or USB storage import.") : s->term ? "Finishing current upload and flushing the card." : s->importing?"Importing USB games and BIOS. Press X for status; Stop cancels the import safely.":"Server starting. Press X to show its address.");
    return;
failed:
    out->error = true; out->ready = false; strcpy(out->status,"File server could not start; no transfer was enabled.");
}
