#!/usr/bin/env python3
"""Compile actual patched helper functions against a bounds-checking SDL stub.

No GPU or target driver is executed. This catches regressions in window policy,
logical upload/crop rectangles, source pitch validation and portrait mapping.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    first = source.index('static unsigned sdl2_window_flags(')
    last = source.index('static INLINE void sdl_tex_zero(', first)
    helpers = source[first:last]
    directory = Path(__file__).resolve().parent
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixture = args.output_dir / 'moss_sdl2_mock.c'
    fixture.write_text((directory / 'moss_sdl2_mock_prefix.h').read_text() + '\n' + helpers
                       + '\n' + (directory / 'moss_sdl2_mock_main.c').read_text(),
                       encoding='utf-8', newline='\n')
    executable = args.output_dir / 'moss_sdl2_mock.exe'
    command = [str(args.zig.resolve()), 'cc', '-target', 'x86_64-windows-gnu', '-std=c11',
               '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', str(fixture.resolve()),
               '-o', str(executable.resolve())]
    environment = os.environ.copy()
    environment['ZIG_GLOBAL_CACHE_DIR'] = str((args.output_dir / 'zig-global').resolve())
    environment['ZIG_LOCAL_CACHE_DIR'] = str((args.output_dir / 'zig-local').resolve())
    temporary = (args.output_dir / 'compiler-temporary').resolve()
    temporary.mkdir(exist_ok=True)
    environment['TEMP'] = environment['TMP'] = str(temporary)
    subprocess.run(command, check=True, env=environment)
    result = subprocess.run([str(executable.resolve())], check=True, capture_output=True, text=True)
    print(result.stdout, end='')
    report = {'schema': 1, 'kind': 'actual-patched-SDL2-helpers-with-mock-SDL', 'passed': True,
              'source_sha256': hashlib.sha256(args.source.read_bytes()).hexdigest(),
              'helper_source_sha256': hashlib.sha256(helpers.encode()).hexdigest(),
              'gpu_or_physical_board_executed': False, 'assertions_enabled': True,
              'cases': ['Mali SHOWN-only and unchanged other-window policies',
                        'RGB565160x144 in NPOT256x256 uses bounded upload and source crop',
                        'Menu320x240 in backing512x256 and ARGB8888 frames',
                        'Short/overflow pitch, wrong format/access/backing/query failures reject',
                        'Clockwise portrait90 geometry and composed core rotation',
                        'NULL render source and invalid output rejected',
                        'SDL upload error propagates']}
    (args.output_dir / 'MOSS_SDL2_MOCK_TESTS.json').write_text(json.dumps(report, indent=2) + '\n',
                                                             encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
