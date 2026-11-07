#!/usr/bin/env python3
"""Compile the native panel and run its stage tests without board-only services."""
from pathlib import Path
import shutil
import subprocess
import sys

if not sys.platform.startswith('linux'):
    raise SystemExit('Run on Linux with CMake, SDL2 and SDL2_ttf development packages.')
compiler = shutil.which('clang')
if compiler is None:
    raise SystemExit('Clang is required, matching the qualified device compiler family.')
root = Path(__file__).resolve().parents[1]
fixture = root / 'build/netplay-panel-source'
build = root / 'build/netplay-panel-build'
fixture.mkdir(parents=True, exist_ok=True)
(fixture / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.16)
project(netplay_panel_source_checks LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
get_filename_component(CLOVE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_subdirectory("${CLOVE_SOURCE}/src/console-launcher" launcher EXCLUDE_FROM_ALL)
add_executable(console-netplay-status "${CLOVE_SOURCE}/src/console-supervisor/src/netplay_status.c")
target_include_directories(console-netplay-status PRIVATE "${CLOVE_SOURCE}/src/console-supervisor/include")
target_link_libraries(console-netplay-status PRIVATE console_launcher m)
add_executable(netplay_status_tests "${CLOVE_SOURCE}/src/console-supervisor/tests/test_netplay_status.c")
target_include_directories(netplay_status_tests PRIVATE "${CLOVE_SOURCE}/src/console-supervisor/include")
target_compile_options(netplay_status_tests PRIVATE -UNDEBUG)
target_link_libraries(netplay_status_tests PRIVATE console_launcher_history)
''', encoding='utf-8')
subprocess.run(['cmake', '-S', str(fixture), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER='+compiler], check=True)
subprocess.run(['cmake', '--build', str(build), '--target', 'console-netplay-status', 'netplay_status_tests', '-j2'], check=True)
subprocess.run([str(build / 'netplay_status_tests')], check=True)
