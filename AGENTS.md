# CloveOS project requirements

- Keep this repository and every future CloveOS update source-first. Publish the
  matching original source, patches, build instructions, dependency pins, and
  notices with each release. Never publish a binary-only CloveOS update.
- Original CloveOS code is GPL-3.0-or-later. Preserve third-party notices and
  licenses; do not claim vendor firmware, games, BIOS, themes, or other projects
  have been relicensed by this repository's LICENSE.
- Follow RELEASE_POLICY.md before uploading or publishing any release. Unresolved
  source, license, asset, or vendor redistribution questions block binary
  publication. A public download URL is not a redistribution license.
- Keep ROMs, BIOS, saves, personal box art, credentials, private keys, personal
  device logs, and local user paths out of Git and release attachments.
- Run `python3 tools/check-source.py` and the relevant existing source tests.
  Record what was tested; distinguish fixtures, QEMU, and physical handheld tests.
- Changes under development must not be presented as verified release behavior.
  Never flash a physical card without explicit user authorization and fresh
  device identification. Preserve the user's content and saves when authorized.
- Keep open-source publishing work separate from the active handheld workspace.
  Do not overwrite newer in-progress changes with an older source snapshot.
