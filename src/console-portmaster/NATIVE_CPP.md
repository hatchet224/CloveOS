# Native port C++ runtime

The official PortMaster FRT 3.3.4 ARM64 runtime requires `GLIBCXX_3.4.26`.
The vendor C++ library in R50, R51 and R56 only exports through
`GLIBCXX_3.4.24`, so the loader rejects that runtime before Godot starts.

Download the original Debian package pinned in `native-cpp.lock.json`, then run:

```
python3 prepare_native_cpp.py --package libstdc++6_10.2.1-6_arm64.deb --overlay overlay
```

Merge the generated two-file overlay when building the rootfs. The existing
Ports wrapper already places `/system/portmaster/lib` first in its library
search path. Keep this runtime isolated to that path; do not replace
`/usr/lib/libstdc++.so.6`, change emulator search paths, or modify port data.

The lock records the original binary package and library checksums, matching
upstream and Debian source archives, and the retained GCC/Debian copyright
notice. Include the corresponding source and notices under the release policy.
This addition is a dependency; it does not relicense GCC or vendor components.

Qualification: actual ARM64 FRT 3.3.4 plus the official Bananaguy Classic data
was executed under QEMU with the shipped Clove libraries. The original loader
failure was reproduced; with this isolated library Godot starts and reaches
video initialization. QEMU has no handheld display or controller, so physical
launch, rendering, sound and controls remain unverified. The shell launcher
probe also hit its virtual hardware-discovery time limit; that result is not
proof of a physical hardware-discovery failure.
