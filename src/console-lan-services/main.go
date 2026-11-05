//go:build linux

package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/subtle"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"path"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/pkg/sftp"
	"github.com/softwarity/smbserver"
	"golang.org/x/crypto/ssh"
	"golang.org/x/sys/unix"
)

const user = "clove"

type filesystem struct{ root *os.Root }

func userPath(input string, writable bool) (string, error) {
	name := strings.TrimPrefix(input, "/")
	if name == "" || name == "." {
		if writable {
			return "", os.ErrPermission
		}
		return ".", nil
	}
	if strings.ContainsAny(name, "\\\x00") || path.Clean(name) != name {
		return "", os.ErrPermission
	}
	parent, _, nested := strings.Cut(name, "/")
	if (parent != "Roms" && parent != "BIOS") || (writable && !nested) {
		return "", os.ErrPermission
	}
	return name, nil
}

func (f filesystem) Fileread(r *sftp.Request) (io.ReaderAt, error) {
	name, err := userPath(r.Filepath, false)
	if err != nil {
		return nil, err
	}
	return f.root.Open(name)
}

func (f filesystem) Filewrite(r *sftp.Request) (io.WriterAt, error) { return f.OpenFile(r) }

func (f filesystem) OpenFile(r *sftp.Request) (sftp.WriterAtReaderAt, error) {
	name, err := userPath(r.Filepath, true)
	if err != nil {
		return nil, err
	}
	p := r.Pflags()
	flags := os.O_WRONLY
	if p.Read {
		flags = os.O_RDWR
	}
	if p.Creat {
		flags |= os.O_CREATE
	}
	if p.Trunc {
		flags |= os.O_TRUNC
	}
	if p.Excl {
		flags |= os.O_EXCL
	}
	if p.Append {
		return nil, sftp.ErrSSHFxOpUnsupported
	}
	return f.root.OpenFile(name, flags, 0600)
}

func (f filesystem) Filecmd(r *sftp.Request) error {
	name, err := userPath(r.Filepath, true)
	if err != nil {
		return err
	}
	switch r.Method {
	case "Rename", "PosixRename":
		to, err := userPath(r.Target, true)
		if err != nil {
			return err
		}
		if r.Method == "Rename" {
			if _, err := f.root.Lstat(to); !errors.Is(err, os.ErrNotExist) {
				if err != nil {
					return err
				}
				return os.ErrExist
			}
		}
		return f.root.Rename(name, to)
	case "Mkdir":
		return f.root.Mkdir(name, 0700)
	case "Remove", "Rmdir":
		info, err := f.root.Lstat(name)
		if err != nil {
			return err
		}
		if info.IsDir() != (r.Method == "Rmdir") {
			return os.ErrPermission
		}
		return f.root.Remove(name)
	case "Setstat":
		flags, attrs := r.AttrFlags(), r.Attributes()
		if flags.UidGid || flags.Permissions {
			return sftp.ErrSSHFxOpUnsupported
		}
		if flags.Size {
			if attrs.Size > 0xffffffff {
				return os.ErrInvalid
			}
			file, err := f.root.OpenFile(name, os.O_WRONLY, 0)
			if err != nil {
				return err
			}
			err = errors.Join(file.Truncate(int64(attrs.Size)), file.Close())
			if err != nil {
				return err
			}
		}
		if flags.Acmodtime {
			return f.root.Chtimes(name, time.Unix(int64(attrs.Atime), 0), time.Unix(int64(attrs.Mtime), 0))
		}
		return nil
	default:
		return sftp.ErrSSHFxOpUnsupported
	}
}

type entries []os.FileInfo

func (e entries) ListAt(out []os.FileInfo, offset int64) (int, error) {
	if offset < 0 || offset >= int64(len(e)) {
		return 0, io.EOF
	}
	n := copy(out, e[offset:])
	if n < len(out) {
		return n, io.EOF
	}
	return n, nil
}

func (f filesystem) Filelist(r *sftp.Request) (sftp.ListerAt, error) {
	name, err := userPath(r.Filepath, false)
	if err != nil {
		return nil, err
	}
	if r.Method == "List" {
		dir, err := f.root.Open(name)
		if err != nil {
			return nil, err
		}
		defer dir.Close()
		list, err := dir.Readdir(0)
		return entries(list), err
	}
	if r.Method != "Stat" && r.Method != "Lstat" {
		return nil, sftp.ErrSSHFxOpUnsupported
	}
	var info os.FileInfo
	if r.Method == "Lstat" {
		info, err = f.root.Lstat(name)
	} else {
		info, err = f.root.Stat(name)
	}
	return entries{info}, err
}

func sshSession(connection net.Conn, cfg *ssh.ServerConfig, handlers sftp.Handlers) {
	_ = connection.SetDeadline(time.Now().Add(20 * time.Second))
	_, channels, requests, err := ssh.NewServerConn(connection, cfg)
	if err != nil {
		return
	}
	_ = connection.SetDeadline(time.Time{})
	go ssh.DiscardRequests(requests)
	for channel := range channels {
		if channel.ChannelType() != "session" {
			_ = channel.Reject(ssh.UnknownChannelType, "SFTP session required")
			continue
		}
		stream, requests, err := channel.Accept()
		if err != nil {
			return
		}
		defer stream.Close()
		for request := range requests {
			var subsystem struct{ Name string }
			if request.Type == "subsystem" && ssh.Unmarshal(request.Payload, &subsystem) == nil && subsystem.Name == "sftp" {
				_ = request.Reply(true, nil)
				go ssh.DiscardRequests(requests)
				server := sftp.NewRequestServer(stream, handlers)
				defer server.Close()
				err := server.Serve()
				if errors.Is(err, io.EOF) || err == nil {
					_, _ = stream.SendRequest("exit-status", false, ssh.Marshal(struct{ Status uint32 }{0}))
				}
				return
			}
			_ = request.Reply(false, nil)
		}
		return
	}
}

func serveSFTP(ctx context.Context, listener net.Listener, root *os.Root, cfg *ssh.ServerConfig) error {
	ctx, cancel := context.WithCancel(ctx)
	fs := filesystem{root}
	handlers := sftp.Handlers{FileGet: fs, FilePut: fs, FileCmd: fs, FileList: fs}
	var workers sync.WaitGroup
	defer func() {
		cancel()
		workers.Wait()
	}()
	go func() { <-ctx.Done(); _ = listener.Close() }()
	limit := make(chan struct{}, 4)
	for {
		connection, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		select {
		case limit <- struct{}{}:
		default:
			_ = connection.Close()
			continue
		}
		workers.Add(1)
		go func() {
			defer workers.Done()
			defer func() { <-limit }()
			defer connection.Close()
			stop := context.AfterFunc(ctx, func() { _ = connection.Close() })
			defer stop()
			sshSession(connection, cfg, handlers)
		}()
	}
}

func sshSigner(card string) (ssh.Signer, error) {
	root, err := os.OpenRoot(card)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	const key = ".console/lan-ssh.seed"
	seed, err := root.ReadFile(key)
	if errors.Is(err, os.ErrNotExist) {
		seed = make([]byte, ed25519.SeedSize)
		if _, err := rand.Read(seed); err != nil {
			return nil, err
		}
		var suffix [8]byte
		if _, err := rand.Read(suffix[:]); err != nil {
			return nil, err
		}
		pending := fmt.Sprintf(".console/.lan-key-%x.tmp", suffix)
		file, err := root.OpenFile(pending, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			return nil, err
		}
		defer root.Remove(pending)
		_, err = file.Write(seed)
		if err := errors.Join(err, file.Sync(), file.Close()); err != nil {
			return nil, err
		}
		if err := root.Rename(pending, key); err != nil {
			return nil, err
		}
		dir, err := root.Open(".console")
		if err != nil {
			return nil, err
		}
		if err := errors.Join(unix.Syncfs(int(dir.Fd())), dir.Close()); err != nil {
			return nil, err
		}
	} else if err != nil {
		return nil, err
	}
	if len(seed) != ed25519.SeedSize {
		return nil, errors.New("invalid SFTP host key")
	}
	return ssh.NewSignerFromKey(ed25519.NewKeyFromSeed(seed))
}

func run() error {
	if len(os.Args) != 4 || (os.Args[1] != "smb" && os.Args[1] != "sftp") {
		return errors.New("expected protocol, listen address, and card directory")
	}
	protocol := os.Args[1]
	control := os.NewFile(3, "supervisor control")
	defer control.Close()
	var packet [12]byte
	if n, err := control.Read(packet[:]); err != nil || n != len(packet) {
		return errors.New("missing supervisor credentials")
	}
	secret := string(packet[:12])
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	go func() { _, _ = control.Read(make([]byte, 1)); cancel() }()
	shareRoot := "/run/console/lan-" + protocol
	cleanup, err := prepareShare(shareRoot, os.Args[3])
	if err != nil {
		return err
	}
	defer cleanup()
	root, err := os.OpenRoot(shareRoot)
	if err != nil {
		return err
	}
	defer root.Close()
	roms, err := root.Open("Roms")
	if err != nil {
		return err
	}
	defer roms.Close()
	listener, err := net.Listen("tcp4", os.Args[2])
	if err != nil {
		return err
	}
	defer listener.Close()
	if protocol == "smb" {
		if _, err := control.Write([]byte("R")); err != nil {
			return err
		}
		err = smbserver.Serve(ctx, listener, smbserver.Config{
			Root: shareRoot, Share: "CloveOS", User: user, NTHash: smbserver.NTHash(secret),
		})
	} else {
		signer, err := sshSigner(os.Args[3])
		if err != nil {
			return err
		}
		cfg := &ssh.ServerConfig{MaxAuthTries: 3, PasswordCallback: func(metadata ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
			if metadata.User() != user || subtle.ConstantTimeCompare(password, []byte(secret)) != 1 {
				return nil, errors.New("authentication failed")
			}
			return nil, nil
		}}
		cfg.AddHostKey(signer)
		if _, err := control.Write([]byte("R" + ssh.FingerprintSHA256(signer.PublicKey()))); err != nil {
			return err
		}
		return errors.Join(serveSFTP(ctx, listener, root, cfg), unix.Syncfs(int(roms.Fd())))
	}
	return errors.Join(err, unix.Syncfs(int(roms.Fd())))
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "LAN transfer:", err)
		os.Exit(1)
	}
}
