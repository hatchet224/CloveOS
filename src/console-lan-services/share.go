//go:build linux

package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"golang.org/x/sys/unix"
)

// The vendor kernel has no mount namespaces. Use one private, root-only tmpfs
// per service and bind only the two approved directories into it.
func prepareShare(share, card string) (func(), error) {
	cardFD, err := unix.Open(card, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	defer unix.Close(cardFD)
	var statfs unix.Statfs_t
	var cardStat unix.Stat_t
	if err := unix.Fstatfs(cardFD, &statfs); err != nil {
		return nil, err
	}
	if statfs.Type != unix.MSDOS_SUPER_MAGIC {
		return nil, errors.New("data partition is not FAT32")
	}
	if err := unix.Fstat(cardFD, &cardStat); err != nil {
		return nil, err
	}
	if err := os.Mkdir(share, 0700); err != nil && !errors.Is(err, os.ErrExist) {
		return nil, err
	}
	var point unix.Stat_t
	if err := unix.Lstat(share, &point); err != nil {
		return nil, err
	}
	if point.Mode&unix.S_IFMT != unix.S_IFDIR || point.Mode&0077 != 0 || point.Uid != uint32(os.Geteuid()) {
		return nil, errors.New("invalid LAN share mountpoint")
	}
	var runDir unix.Stat_t
	if err := unix.Stat(filepath.Dir(share), &runDir); err != nil {
		return nil, err
	}
	if point.Dev != runDir.Dev {
		return nil, errors.New("LAN share already mounted")
	}
	const restrictions = unix.MS_NODEV | unix.MS_NOSUID | unix.MS_NOEXEC
	if err := unix.Mount("tmpfs", share, "tmpfs", restrictions, "mode=0700,size=128k"); err != nil {
		return nil, err
	}
	cleanup := func() {
		_ = unix.Unmount(share, unix.MNT_DETACH)
		_ = os.Remove(share)
	}
	for _, name := range []string{"Roms", "BIOS"} {
		if err := bindDirectory(cardFD, cardStat.Dev, name, filepath.Join(share, name), restrictions); err != nil {
			cleanup()
			return nil, fmt.Errorf("prepare %s share: %w", name, err)
		}
	}
	if err := unix.Mount("", share, "", unix.MS_REMOUNT|unix.MS_RDONLY|restrictions, ""); err != nil {
		cleanup()
		return nil, err
	}
	return cleanup, nil
}

func bindDirectory(cardFD int, device uint64, name, target string, restrictions uintptr) error {
	if err := unix.Mkdirat(cardFD, name, 0700); err != nil && !errors.Is(err, os.ErrExist) {
		return err
	}
	fd, err := unix.Openat(cardFD, name, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return err
	}
	defer unix.Close(fd)
	var info unix.Stat_t
	if err := unix.Fstat(fd, &info); err != nil {
		return err
	}
	if info.Dev != device {
		return errors.New("folder is on another filesystem")
	}
	if err := os.Mkdir(target, 0700); err != nil {
		return err
	}
	if err := unix.Mount(fmt.Sprintf("/proc/self/fd/%d", fd), target, "", unix.MS_BIND, ""); err != nil {
		return err
	}
	return unix.Mount("", target, "", unix.MS_BIND|unix.MS_REMOUNT|restrictions, "")
}
