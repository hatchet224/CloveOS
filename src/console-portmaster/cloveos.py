# SPDX-License-Identifier: GPL-3.0-or-later
"""CloveOS platform adapter for the pinned MIT PortMaster GUI.

Ports remain under CLOVE_DATA/Roms/ports. The native CloveOS Ports browser
rescans that directory when PortMaster returns. No duplicate port data is kept.
"""
from pathlib import Path
import shutil
from .base import PlatformBase


class PlatformCloveOS(PlatformBase):
    ES_NAME = None
    WANT_XBOX_FIX = False
    WANT_SWAP_BUTTONS = False

    def portmaster_install(self, bash_files):
        super().portmaster_install(bash_files)
        # Explicit upstream updates must retain the device's platform glue.
        target = self.hm.tools_dir / 'PortMaster'
        template = Path('/system/portmaster/template')
        for relative in ('control.txt', 'pylibs/harbourmaster/platform/cloveos.py'):
            source = template / relative
            if source.is_file():
                shutil.copyfile(source, target / relative)

    def port_install(self, port_name, port_info, port_files, fix_perm_files):
        super().port_install(port_name, port_info, port_files, fix_perm_files)
        # Installed scripts are executed through Bash on our private executable
        # Ports bind mount; chmod on FAT32 is neither needed nor meaningful.

