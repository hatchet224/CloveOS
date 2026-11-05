//go:build linux

// vmtest exercises the production ARM64 service with independent clients.
package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/hirochachacha/go-smb2"
	"github.com/pkg/sftp"
	"golang.org/x/crypto/ssh"
	"golang.org/x/sys/unix"
)

const card = "/mnt/test-card"
const secret = "EXAMPLE23456"
const changed = "UPDATED23456"
const smbAddr = "127.0.0.1:1445"
const sftpAddr = "127.0.0.1:2222"
const smb21 = 0x0210
const smb30 = 0x0300

var payload = bytes.Repeat([]byte("CloveOS LAN test data\n"), 7777)

type service struct {
	cmd         *exec.Cmd
	control     *os.File
	fingerprint string
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func check(ok bool, what string) {
	if !ok {
		panic(what)
	}
}

func start(protocol, addr, password string) *service {
	const server = "/system/bin/console-lan-server"
	args := []string{protocol, addr, card}
	var cmd *exec.Cmd
	if len(os.Args) == 2 {
		cmd = exec.Command(os.Args[1], append([]string{server}, args...)...)
	} else {
		cmd = exec.Command(server, args...)
	}
	pair, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
	must(err)
	parent, child := os.NewFile(uintptr(pair[0]), "control"), os.NewFile(uintptr(pair[1]), "service")
	defer child.Close()
	cmd.ExtraFiles = []*os.File{child}
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	_, err = parent.WriteString(password)
	must(err)
	must(cmd.Start())
	timer := time.AfterFunc(10*time.Second, func() { _ = cmd.Process.Kill() })
	defer timer.Stop()
	var response [80]byte
	n, err := parent.Read(response[:])
	must(err)
	check(n > 0 && response[0] == 'R', "service did not acknowledge readiness")
	return &service{cmd, parent, string(response[1:n])}
}

func (s *service) stop(protocol, addr string) {
	must(s.control.Close())
	timer := time.AfterFunc(15*time.Second, func() { _ = s.cmd.Process.Kill() })
	defer timer.Stop()
	must(s.cmd.Wait())
	_, err := os.Stat("/run/console/lan-" + protocol)
	check(errors.Is(err, os.ErrNotExist), "share directory remained after shutdown")
	conn, err := net.DialTimeout("tcp4", addr, time.Second)
	if conn != nil {
		_ = conn.Close()
	}
	check(err != nil, "listener remained after shutdown")
}

func smbLogin(user, password string, dialect uint16) (*smb2.Session, net.Conn, error) {
	conn, err := net.DialTimeout("tcp4", smbAddr, 3*time.Second)
	if err != nil {
		return nil, nil, err
	}
	dialer := smb2.Dialer{
		Negotiator: smb2.Negotiator{RequireMessageSigning: true, SpecifiedDialect: dialect},
		Initiator:  &smb2.NTLMInitiator{User: user, Password: password},
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	session, err := dialer.DialContext(ctx, conn)
	if err != nil {
		_ = conn.Close()
		return nil, nil, err
	}
	return session, conn, nil
}

func visible(entries []os.FileInfo) {
	var names []string
	for _, entry := range entries {
		if entry.Name() != "." && entry.Name() != ".." {
			names = append(names, entry.Name())
		}
	}
	sort.Strings(names)
	check(strings.Join(names, ",") == "BIOS,Roms", fmt.Sprintf("unexpected share contents: %v", names))
}

func verifyFile(name string, expected []byte) {
	data, err := os.ReadFile(filepath.Join(card, name))
	must(err)
	check(bytes.Equal(data, expected), "card file differs: "+name)
}

func testSMB() {
	session, conn, err := smbLogin("clove", secret, smb21)
	must(err)
	defer conn.Close()
	defer session.Logoff()
	share, err := session.Mount("CloveOS")
	must(err)
	defer share.Umount()
	entries, err := share.ReadDir(".")
	must(err)
	visible(entries)
	for _, name := range []string{`.console\private`, `Saves\private`, `..\.console\private`} {
		_, err := share.ReadFile(name)
		check(err != nil, "SMB disclosed "+name)
	}
	check(share.WriteFile("escaped", payload, 0600) != nil, "SMB wrote to virtual share root")
	check(share.Rename("Roms", "Moved") != nil, "SMB renamed a top-level folder")
	must(share.Mkdir(`Roms\SMB`, 0700))
	must(share.WriteFile(`Roms\SMB\fixture.rom`, payload, 0600))
	must(share.Rename(`Roms\SMB\fixture.rom`, `Roms\SMB\renamed.rom`))
	data, err := share.ReadFile(`Roms\SMB\renamed.rom`)
	must(err)
	check(bytes.Equal(data, payload), "SMB download differs")
	must(share.WriteFile(`BIOS\smb.bin`, payload[:20], 0600))
	verifyFile("Roms/SMB/renamed.rom", payload)
	verifyFile("BIOS/smb.bin", payload[:20])
}

func testSMB3() {
	session, conn, err := smbLogin("clove", secret, smb30)
	must(err)
	defer conn.Close()
	defer session.Logoff()
	share, err := session.Mount("CloveOS")
	must(err)
	defer share.Umount()
	data, err := share.ReadFile(`Roms\SMB\renamed.rom`)
	must(err)
	check(bytes.Equal(data, payload), "signed SMB 3.0 download differs")
	fmt.Println("LAN_SIGNED_SMB_3_0_PASS")
}

func sshLogin(user, password, fingerprint string) (*ssh.Client, error) {
	return ssh.Dial("tcp4", sftpAddr, &ssh.ClientConfig{
		User: user, Auth: []ssh.AuthMethod{ssh.Password(password)}, Timeout: 5 * time.Second,
		HostKeyCallback: func(_ string, _ net.Addr, key ssh.PublicKey) error {
			if ssh.FingerprintSHA256(key) != fingerprint {
				return errors.New("unexpected SFTP host key")
			}
			return nil
		},
	})
}

func testSFTP(fingerprint string) *ssh.Client {
	client, err := sshLogin("clove", secret, fingerprint)
	must(err)
	shell, err := client.NewSession()
	must(err)
	check(shell.Run("id") != nil, "SFTP offered a shell")
	_ = shell.Close()
	// A rejected exec is allowed to close the original SSH connection.
	_ = client.Close()
	client, err = sshLogin("clove", secret, fingerprint)
	must(err)
	_, err = client.Dial("tcp4", smbAddr)
	check(err != nil, "SFTP offered port forwarding")
	transfer, err := sftp.NewClient(client)
	must(err)
	defer transfer.Close()
	entries, err := transfer.ReadDir("/")
	must(err)
	visible(entries)
	for _, name := range []string{"/.console/private", "/Saves/private", "/Roms/../Saves/private"} {
		_, err := transfer.Open(name)
		check(err != nil, "SFTP disclosed "+name)
	}
	check(transfer.Mkdir("/escaped") != nil, "SFTP wrote to virtual share root")
	check(transfer.Rename("/Roms", "/Moved") != nil, "SFTP renamed a top-level folder")
	must(transfer.Mkdir("/Roms/SFTP"))
	file, err := transfer.Create("/Roms/SFTP/fixture.rom")
	must(err)
	_, err = file.Write(payload)
	must(errors.Join(err, file.Close()))
	must(transfer.Rename("/Roms/SFTP/fixture.rom", "/Roms/SFTP/renamed.rom"))
	file, err = transfer.Open("/Roms/SFTP/renamed.rom")
	must(err)
	data, err := io.ReadAll(file)
	must(errors.Join(err, file.Close()))
	check(bytes.Equal(data, payload), "SFTP download differs")
	file, err = transfer.Create("/BIOS/sftp.bin")
	must(err)
	_, err = file.Write(payload[:20])
	must(errors.Join(err, file.Close()))
	verifyFile("Roms/SFTP/renamed.rom", payload)
	verifyFile("BIOS/sftp.bin", payload[:20])
	return client
}

func main() {
	smb := start("smb", smbAddr, secret)
	sftpService := start("sftp", sftpAddr, secret)
	_, _, err := smbLogin("clove", "wrong", smb21)
	check(err != nil, "SMB accepted a wrong password")
	_, _, err = smbLogin("other", secret, smb21)
	check(err != nil, "SMB accepted a wrong username")
	_, err = sshLogin("clove", "wrong", sftpService.fingerprint)
	check(err != nil, "SFTP accepted a wrong password")
	_, err = sshLogin("other", secret, sftpService.fingerprint)
	check(err != nil, "SFTP accepted a wrong username")
	fmt.Println("LAN_AUTHENTICATION_PASS")
	testSMB()
	testSMB3()
	client := testSFTP(sftpService.fingerprint)
	verifyFile(".console/private", []byte("config\n"))
	verifyFile("Saves/private", []byte("save\n"))
	fmt.Println("LAN_SIGNED_SMB_AND_SFTP_TRANSFER_SCOPING_PASS")
	smb.stop("smb", smbAddr)
	sftpService.stop("sftp", sftpAddr)
	_ = client.Close()
	next := start("sftp", sftpAddr, changed)
	check(next.fingerprint == sftpService.fingerprint, "SFTP host fingerprint changed")
	_, err = sshLogin("clove", secret, next.fingerprint)
	check(err != nil, "SFTP accepted the previous password")
	client, err = sshLogin("clove", changed, next.fingerprint)
	must(err)
	next.stop("sftp", sftpAddr)
	_ = client.Close()
	fmt.Println("LAN_LISTENERS_UNMOUNT_RESTART_AND_STABLE_HOST_KEY_PASS")
}
