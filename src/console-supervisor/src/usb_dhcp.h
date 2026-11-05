#ifndef CONSOLE_USB_DHCP_H
#define CONSOLE_USB_DHCP_H
/* A one-client, link-local DHCP lease. Never advertises a default gateway/DNS. */
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
static int usb_dhcp_open(void)
{
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0),yes=1;
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(67),.sin_addr={.s_addr=INADDR_ANY}};
    if (fd<0) return -1;
    if (setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes))!=0 ||
        setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,"usb0",5U)!=0 ||
        bind(fd,(struct sockaddr *)&address,sizeof(address))!=0) {close(fd);return -1;}
    return fd;
}
static void usb_dhcp_reply(int fd)
{
    unsigned char request[1500],reply[300]={0};
    struct sockaddr_in peer; socklen_t peer_size=sizeof(peer);
    ssize_t n=recvfrom(fd,request,sizeof(request),MSG_DONTWAIT,(struct sockaddr *)&peer,&peer_size);
    if (n<240 || peer.sin_port!=htons(68) || request[0]!=1U || request[1]!=1U || request[2]!=6U ||
        memcmp(request+236,"\x63\x82\x53\x63",4U)!=0) return;
    unsigned int type=0U;bool ours=true;
    const unsigned char server[4]={192,168,28,1},client[4]={192,168,28,2};
    for(size_t at=240U;at<(size_t)n;) {
        unsigned int code=request[at++];if(code==255U)break;if(code==0U)continue;
        if(at>=(size_t)n)return;size_t length=request[at++];if(length>(size_t)n-at)return;
        if(code==53U && length==1U)type=request[at];
        if(code==54U && (length!=4U || memcmp(request+at,server,4U)!=0))ours=false;
        if(code==50U && (length!=4U || memcmp(request+at,client,4U)!=0))ours=false;
        at+=length;
    }
    if(!ours || (type!=1U && type!=3U))return;
    reply[0]=2U;reply[1]=1U;reply[2]=6U;memcpy(reply+4,request+4,4U);
    reply[10]=0x80U;memcpy(reply+16,client,4U);memcpy(reply+20,server,4U);
    memcpy(reply+28,request+28,16U);memcpy(reply+236,request+236,4U);
    unsigned char options[]={53,1,0,54,4,192,168,28,1,51,4,0,0,14,16,1,4,255,255,255,252,255};
    options[2]=type==1U?2U:5U;memcpy(reply+240,options,sizeof(options));
    peer.sin_family=AF_INET;peer.sin_port=htons(68);peer.sin_addr.s_addr=INADDR_BROADCAST;
    (void)sendto(fd,reply,sizeof(reply),0,(struct sockaddr *)&peer,sizeof(peer));
}
#endif
