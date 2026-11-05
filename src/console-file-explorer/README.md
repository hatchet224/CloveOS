# CloveOS File Explorer

Original CloveOS implementation, licensed GPL-3.0-or-later. This module contains
no DinguxCommander implementation, patches, icons or fonts. It uses CloveOS's
existing SDL display, controller input, builtin text renderer and mounted-card
storage adapters.

The managed application receives the existing `cl_file_explorer_config` through
inherited socket descriptor 3. The supervisor launches it at
`/system/bin/console-file-explorer`. It checks the configured card identity and
requires writable FAT storage before enabling operations.

Two panes support folder browsing, file/folder copy and move, rename, recursive
delete, new folders, multiple selection and a read-only text preview. Actions
never overwrite existing destinations. Copy publishes a temporary object only
after completing it; B cancels a transfer. Confirmation defaults to Cancel.
Root system dot-directories are protected. Symlinks, hardlinked files and paths
outside the selected card are rejected. Completed operations are synchronized.
An interruption or sync error can leave partial results; the UI refreshes them.

Controls: D-pad selects entries/panes, shoulders page, A opens, B goes back,
Y marks, X opens actions, Menu/Select opens the quit confirmation. In the name
keyboard, A types, X erases, Start saves, and B cancels.

Build on Linux with matching sibling launcher/sessiond sources and SDL2 headers:

```sh
cmake -S src/console-file-explorer -B build/file-explorer -DFM_BUILD_TESTS=ON
cmake --build build/file-explorer
ctest --test-dir build/file-explorer --output-on-failure
```

Backend-only tests do not require SDL or the device adapters:

```sh
cmake -S src/console-file-explorer -B build/file-tests -DFM_BUILD_APP=OFF -DFM_BUILD_TESTS=ON
cmake --build build/file-tests
ctest --test-dir build/file-tests --output-on-failure
```

The backend regression uses generated temporary files, including cancellation,
media removal, traversal and protected metadata cases. Separate release checks
exercise the production ARM64 application through its FD3 interface and SDL
controller events on a generated FAT32 volume. These checks do not establish
physical handheld verification.
