package main

import (
 "archive/zip"
 "bytes"
 "encoding/binary"
 "net"
 "os"
 "path/filepath"
 "testing"
 "crypto/sha256"
 "encoding/hex"
 "hash/crc32"
 "context"
 "io"
 "time"
)

func TestOnionCookieFTP(t *testing.T) {
 folder:=t.TempDir();cookie:=[]byte("[core]: /mnt/SDCARD/RetroArch/.retroarch/cores/fceumm_libretro.so\n[rom]: /mnt/SDCARD/Roms/FC/Test.nes\n[corechksum]: ABCDEF12\n[romchksum]: 12345678\n")
 filename:=filepath.Join(folder,"cookie");if e:=os.WriteFile(filename,cookie,0600);e!=nil{t.Fatal(e)}
 mapping:=Export{Files:map[string]string{"/mnt/SDCARD/RetroArch/retroarch.cookie":filename}}
 listener,e:=net.Listen("tcp4","127.0.0.1:0");if e!=nil{t.Fatal(e)};defer listener.Close()
 go func(){for {c,e:=listener.Accept();if e!=nil{return};go servePeer(c,mapping,net.ParseIP("127.0.0.1"))}}()
 result,e:=fetch(listener.Addr().String(),"/mnt/SDCARD/RetroArch/retroarch.cookie",4096)
 if e!=nil||!bytes.Equal(result,cookie){t.Fatalf("cookie %q: %v",result,e)}
 if _,e=fetch(listener.Addr().String(),"/etc/shadow",4096);e==nil{t.Fatal("unlisted file exported")}
 if _,e=fetch(listener.Addr().String(),"/mnt/SDCARD/RetroArch/../../../etc/shadow",4096);e==nil{t.Fatal("path traversal exported")}
 if _,e=fetch(listener.Addr().String(),"/mnt/SDCARD/RetroArch/retroarch.cookie",20);e==nil{t.Fatal("size bound not enforced")}
 ftp,e:=connect(listener.Addr().String());if e!=nil{t.Fatal(e)};defer ftp.Close()
 if code,_,e:=ftp.command("CWD /mnt/SDCARD/RetroArch");e!=nil||code!=250{t.Fatal(code,e)}
 if code,_,e:=ftp.command("SIZE retroarch.cookie");e!=nil||code!=213{t.Fatal(code,e)}
 if code,_,e:=ftp.command("STOR /mnt/SDCARD/.console/accounts");e!=nil||code!=502{t.Fatal("write enabled",code,e)}
 t.Log("Onion 4.3.1-1 cookie paths, EPSV, CWD/SIZE, selected-file RETR; other files and writes denied")
}
func TestRawAndZIPCartridgeComparison(t *testing.T){
 raw:=bytes.Repeat([]byte("Original Clove fixture"),100)
 var buffer bytes.Buffer;archive:=zip.NewWriter(&buffer);entry,e:=archive.Create("Fixture.nes");if e!=nil{t.Fatal(e)}
 entry.Write(raw);archive.Close()
 uncompressed,e:=payloadCRC(raw,".nes");if e!=nil{t.Fatal(e)}
 compressed,e:=payloadCRC(buffer.Bytes(),".zip");if e!=nil||compressed!=uncompressed{t.Fatal("raw/ZIP game revision mismatch",e)}
 t.Log("Matching game content accepted across raw and single-cartridge ZIP without storing a duplicate ROM")
}
func TestArcadeLANArchiveCRC(t *testing.T){
 var buffer bytes.Buffer;archive:=zip.NewWriter(&buffer)
 for _,name:=range []string{"original-p1.bin","original-c1.bin"}{
  entry,e:=archive.Create(name);if e!=nil{t.Fatal(e)};entry.Write([]byte("original test data "+name))
 }
 archive.Close();file:=filepath.Join(t.TempDir(),"fixture.zip")
 if e:=os.WriteFile(file,buffer.Bytes(),0600);e!=nil{t.Fatal(e)}
 if _,e:=contentCRC(file);e==nil{t.Fatal("ambiguous cartridge archive accepted")}
 for _,core:=range []string{"FB Alpha 2012 Neo Geo","FinalBurn Neo"}{
  actual,e:=lanContentCRC(file,core)
  if e!=nil||actual!=crc32.ChecksumIEEE(buffer.Bytes()){t.Fatal("arcade full-archive CRC",core,e)}
 }
}
func TestLinkTemporaryUploadAndBattery(t *testing.T){
 folder:=t.TempDir();mapping:=Export{LinkInbox:folder,LinkRole:"host"}
 listener,e:=net.Listen("tcp4","127.0.0.1:0");if e!=nil{t.Fatal(e)};defer listener.Close()
 go func(){for{c,e:=listener.Accept();if e!=nil{return};go servePeer(c,mapping,net.ParseIP("127.0.0.1"))}}()
 address:=listener.Addr().String();battery:=bytes.Repeat([]byte{0x5a},8192)
 if e:=put(address,"/tmp/Player.srm",battery);e!=nil{t.Fatal(e)}
 actual,e:=os.ReadFile(filepath.Join(folder,"Player.srm"));if e!=nil||!bytes.Equal(actual,battery){t.Fatal("battery upload",e)}
 if e:=put(address,"/tmp/host_ready",nil);e!=nil{t.Fatal(e)}
 if exists,e:=remoteExists(address,"/tmp/host_ready");e!=nil||!exists{t.Fatal("uploaded ready marker invisible to SIZE",e)}
 os.Remove(filepath.Join(folder,"host_ready"))
 if exists,e:=remoteExists(address,"/tmp/host_ready");e!=nil||exists{t.Fatal("consumed ready marker still visible",e)}
 if e:=put(address,"/tmp/host_ready",[]byte{1});e==nil{t.Fatal("nonempty marker accepted")}
 for _,name:=range []string{"/etc/shadow","/tmp/../settings.srm","/mnt/SDCARD/Roms/Test.gb","/tmp/Player.srm_rcvd"}{
  if e:=put(address,name,battery);e==nil{t.Fatal("unscoped upload accepted",name)}
 }
 sum:=sha256.Sum256([]byte("cartridge"));hash:=hex.EncodeToString(sum[:]);record:=linkRecord(hash,battery,[]byte{1,2,3,4})
 file:=filepath.Join(folder,"battery.link");if e:=os.WriteFile(file,record,0600);e!=nil{t.Fatal(e)}
 sram,rtc,e:=readLinkRecord(file,hash,len(battery));if e!=nil||!bytes.Equal(sram,battery)||!bytes.Equal(rtc,[]byte{1,2,3,4}){t.Fatal("C link record ABI",e)}
 record[208]^=1;os.WriteFile(file,record,0600);if _,_,e:=readLinkRecord(file,hash,len(battery));e==nil{t.Fatal("corrupt battery accepted")}
 t.Log("Actual FTP STOR into bounded RAM link inbox only; marker size and paths checked; C link-record battery hash validated")
}
func TestDHCPDiscoverAndRequest(t *testing.T) {
 request:=make([]byte,240);request[0]=1;request[1]=1;request[2]=6
 binary.BigEndian.PutUint32(request[4:8],0x12345678);binary.BigEndian.PutUint32(request[236:240],0x63825363)
 request=append(request,53,1,1,255)
 reply:=dhcpReply(request,net.ParseIP("192.168.100.100"))
 if len(reply)!=300||reply[0]!=2||!bytes.Equal(reply[4:8],request[4:8])||!net.IP(reply[16:20]).Equal(net.ParseIP("192.168.100.101"))||reply[242]!=2{t.Fatal("bad offer")}
 request[242]=3;reply=dhcpReply(request,net.ParseIP("192.168.100.100"));if reply[242]!=5{t.Fatal("missing ACK")}
 if dhcpReply(request[:239],net.ParseIP("192.168.100.100"))!=nil{t.Fatal("short BOOTP accepted")}
 request=append(request[:243],54,4,192,168,100,99,255)
 if dhcpReply(request,net.ParseIP("192.168.100.100"))!=nil{t.Fatal("other DHCP server accepted")}
 t.Log("Exact Onion subnet and server, deterministic client address, transaction/MAC preserved, malformed/foreign requests rejected")
}
func TestRetroArchAdvertisement(t *testing.T) {
 packet:=make([]byte,688);binary.BigEndian.PutUint32(packet[:4],0x52414e53)
 binary.BigEndian.PutUint32(packet[4:8],12345);binary.BigEndian.PutUint32(packet[8:12],55435)
 copy(packet[80:112],"FCEUmm");copy(packet[112:144],"(SVN) f52037a")
 if port,ok:=advertisement(packet,12345,"FCEUmm","(SVN) f52037a");!ok||port!=55435{t.Fatal("valid room rejected")}
 copy(packet[432:688],"N/A");if _,ok:=advertisement(packet,12345,"FCEUmm","(SVN) f52037a");!ok{t.Fatal("actual RetroArch N/A subsystem rejected")}
 if _,ok:=advertisement(packet,12346,"FCEUmm","(SVN) f52037a");ok{t.Fatal("wrong ROM accepted")}
 if _,ok:=advertisement(packet,12345,"FCEUmm","other");ok{t.Fatal("wrong revision accepted")}
 binary.BigEndian.PutUint32(packet[12:16],1);if _,ok:=advertisement(packet,12345,"FCEUmm","");ok{t.Fatal("password room accepted")}
 t.Log("RetroArch 1.15.0/1.19.1 LAN packet schema; exact content/core/revision and password checks")
}

func TestTCPRoomMetadataOnly(t *testing.T) {
 for _,variant:=range []string{"matching","wrong-crc","wrong-core","wrong-version","password","bad-command","short-response"}{
  t.Run(variant,func(t *testing.T){
   listener,e:=net.Listen("tcp4","127.0.0.1:0");if e!=nil{t.Fatal(e)};defer listener.Close()
   done:=make(chan error,1)
   go func(){
    connection,e:=listener.Accept();if e!=nil{done<-e;return};defer connection.Close()
    connection.SetDeadline(time.Now().Add(3*time.Second))
    header:=make([]byte,24);if _,e=io.ReadFull(connection,header);e!=nil{done<-e;return}
    if binary.BigEndian.Uint32(header[:4])!=0x52414e50{done<-io.ErrUnexpectedEOF;return}
    binary.BigEndian.PutUint32(header[12:16],0);binary.BigEndian.PutUint32(header[16:20],6)
    if variant=="password"{binary.BigEndian.PutUint32(header[12:16],123)}
    // Fragment the response: TCP packet boundaries are not message boundaries.
    for _,b:=range header{if _,e=connection.Write([]byte{b});e!=nil{done<-e;return}}
    if variant=="password"{done<-nil;return}
    nick:=make([]byte,40);if _,e=io.ReadFull(connection,nick);e!=nil{done<-e;return}
    if binary.BigEndian.Uint32(nick[:4])!=0x20||binary.BigEndian.Uint32(nick[4:8])!=32{done<-io.ErrUnexpectedEOF;return}
    connection.Write(nick)
    info:=make([]byte,76);binary.BigEndian.PutUint32(info[:4],0x22);binary.BigEndian.PutUint32(info[4:8],68)
    binary.BigEndian.PutUint32(info[8:12],0x12345678);copy(info[12:44],"FB Alpha 2012 Neo Geo");copy(info[44:76],"v0.2.97.29")
    switch variant{
    case "wrong-crc":info[8]^=1
    case "wrong-core":info[12]='X'
    case "wrong-version":info[44]='X'
    case "bad-command":info[3]=0x21
    case "short-response":connection.Write(info[:30]);done<-nil;return
    }
    connection.Write(info)
    var extra [1]byte
    n,e:=connection.Read(extra[:]);if n!=0||e!=io.EOF{done<-io.ErrUnexpectedEOF;return}
    done<-nil
   }()
   ctx,cancel:=context.WithTimeout(context.Background(),4*time.Second);defer cancel()
   result:=tcpRoom(ctx,listener.Addr().String(),0x12345678,"FB Alpha 2012 Neo Geo","v0.2.97.29")
   if result!=(variant=="matching"){t.Fatalf("metadata match %v",result)}
   if e:=<-done;e!=nil{t.Fatal("probe sent player INFO or invalid handshake",e)}
  })
 }
}
func TestRoomTargetScope(t *testing.T){
 for _,target:=range []string{"8.8.8.8","example.com","not-an-ip","::1"}{if len(roomTargets(target))!=0{t.Fatal("unexpected scan target",target)}}
 for _,target:=range []string{"127.0.0.1","192.168.100.100"}{result:=roomTargets(target);if len(result)!=1||result[0]!=target{t.Fatal("explicit local target",target,result)}}
}
