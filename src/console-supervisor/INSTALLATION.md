# Image integration and startup order

These are installation requirements for the future qualified Tina image, not
commands executed on the Windows workspace.

Place bootstrap files on the original root filesystem so they remain available
before mounting or after unmounting either TF card:

```text
/usr/magicx/bin/console-rc          <- init/rc.init, executable
/usr/magicx/bin/console-ctl         <- compiled bootstrap control helper
/usr/magicx/bin/console-poweroff    <- guarded normal shutdown wrapper
/etc/console/shutdown-hook.sh      <- early pre-kill/pre-unmount barrier
/etc/console/cards.conf            <- root owned, mode 0400 or 0600
/etc/console/supervisor.json
/etc/console/base-profile.json
/etc/console/ui.json
/etc/console/bridge-launch.json
/etc/console/catalog/*.json
/etc/console/profiles/*.json
/etc/console/retroarch.cfg
```

System-TF contents at the configured read-only mount:

```text
/opt/console/bin/console-supervisor
/opt/console/bin/console-sessiond
/opt/console/bin/console-launcher
/opt/console/bin/retroarch
/opt/console/cores/*_libretro.so
/opt/console/bios/
/opt/console/lib/                    matching vendor/SDK libraries only
```

Retain the existing procd/init setup and stock rc.local mixer initialization.
The supplied Moss patch replaces only its runmagicx.sh application entry with
console-rc start. It targets the inspected Moss revision
021bb12bf53c6ccc7c5f7f57952e3f0e04ecb492. Do not run the original application
owner in parallel; its updater/poweroff fallbacks are not this supervisor's
lifecycle. No systemd unit or assumed /etc/rc.common API is required.

The BSP's actual shutdown sequence must call shutdown-hook.sh **before** any
kill-all, forced service timeout, or filesystem teardown. Its nonzero result must
prevent that phase from proceeding. The public poweroff entry must call
console-poweroff, which waits for the barrier before invoking normal
/sbin/poweroff. The supervisor's configured poweroff must be the normal init
request, never poweroff -f, reboot(RB_POWER_OFF) directly, or a recursive wrapper.

An arbitrary late procd stop hook cannot veto a shutdown that PID 1 has already
begun; ignoring the hook result or allowing a service manager's kill timeout to
terminate checkpoint writers defeats the barrier. The complete vendor shutdown
scripts are not present in Moss, so no invented universal procd patch is supplied.
Wire the provided early hook into the real BSP and qualify that ordering. Direct
uncoordinated /sbin/poweroff, kill -9, PMIC forced-off and abrupt battery removal
remain outside the orderly-shutdown guarantee.

Populate cards.conf with measured device paths, UUIDs, filesystem types and mount
options. No mmc index or partition number is inferred. The script requires the
qualified root-image mount/umount/mountpoint/stat/readlink/ln tools and util-linux
blkid supporting -s UUID -o value; do not assume a different BusyBox blkid accepts
those options. If the BSP does not create /dev/disk/by-uuid aliases, use qualified
explicit block-device paths; the script creates an alias only after UUID checks.
An existing inconsistent alias/mount is refused, not replaced. There is no format,
fsck, repair, raw flash write or automatic unmount of an unexpected device.

Games options should include nosuid,nodev,noexec. Use the actual supported ext4
driver or qualified FAT32 vfat setup. FAT uid/gid/masks must satisfy daemon
ownership/protected-mode checks, and qualified_fat_syncfs must be true in all
matching profiles only after qualification. The system mount must be read-only.
Bootstrap binaries must use the stock root ABI; vendor SDL/GPU libraries on the
system TF must match its kernel and Tina runtime.

Base profile all-zero identities are an idle storage-bootstrap context, never
an executable game. Register each real game through catalog.example.json and a
frozen profile containing the endpoint's eight actual measured identities. The
existing RetroArch enrollment helper documents that record. All per-game profiles
must agree with base card path/UUID, bridge socket/UID, power input identity/code,
state bounds and FAT policy. No user-controlled ROM name becomes an executable or
shell command. Registry/profile/config files must be trusted, root owned and not
group/other writable. Empty catalog is supported.

Merge the delivered retroarch-managed.cfg into the one frozen RetroArch
configuration. Preserve its BIOS directory, thread/data policy, disabled
competing autosaves/config overrides and bridge-enabled build. Runtime config/BIOS
identities are rechecked at HELLO rather than copied from requests. Disc/subsystem
and independent-writer cores remain unqualified.

ui.json uses the reference button indices only as a template. Replace GUID and
confirm actual physical labels and hats on the board. Value 65535 disables an
optional button/hat; rotation 999 means auto. Choose the measured portrait 90/270
direction and qualify any software fallback. No GPIO or module name is guessed;
INPUT_MODULES may be empty for built-in drivers. Menu evdev name/code and SDL
button indices are separate namespaces. Missing/ambiguous MENU discovery disables
boot auto-resume; the daemon independently requires its real power input.

Operational controls are local, root-only:

```text
console-ctl --socket /run/console/control.sock status
console-ctl --socket /run/console/control.sock menu
console-ctl --socket /run/console/control.sock retry
console-ctl --socket /run/console/control.sock stop
console-ctl check-stopped
```

Retry acknowledges only queueing a retry. Stop has no deadline-to-success; it
waits for a completed barrier. If a flush fails after the daemon has exited,
explicit retry can re-run the final volume barrier. The process remains alive
while it is blocked; there is no crash/respawn loop or forced kill. A disconnected
stop client does not cancel an already accepted shutdown request.

The new combined daemon patch is relative to the earlier production source ZIP
and includes the earlier read-only API additions. It supersedes the launcher
read-only patch for that baseline. All changes are already present in the current
source directories: do not apply either patch again to them. The RetroArch module
and existing upstream checkpoint patch are unchanged by this phase.
