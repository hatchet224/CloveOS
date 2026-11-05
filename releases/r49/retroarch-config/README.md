# Recorded target RetroArch configuration

`config.h` records the target feature selection. `config.mk.in` is the recorded
make configuration with the local developer prefix replaced by
`$(CLOVE_BUILD_ROOT)`. That variable names the project build layout containing the
qualified AArch64 compiler wrapper, SDL headers and target libraries. It is not
the RetroArch checkout directory itself.

After applying the source installer to the exact upstream pin, provide matching
target dependencies, copy these files into the RetroArch checkout as `config.h`
and `config.mk`, and build with:

```sh
make HAVE_MAGICX_BRIDGE=1 CLOVE_BUILD_ROOT=/path/to/project
```

These are Linux/target settings, not host
autodetection results. Read the bridge's Moss/SDL integration notes first.

No target shared library, proprietary GPU component, SDK, kernel or bootloader is
provided by this configuration snapshot. Those dependencies keep their own
licenses and source/distribution requirements.
