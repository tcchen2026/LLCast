#!/usr/bin/env bash
# Cross-build check; no device/PC needed. Optional argument is a fresh build dir.
set -euo pipefail
lls_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
lls_build=${1:-"$lls_root/build/windows-check"}
cmake -S "$lls_root" -B "$lls_build" \
    -DCMAKE_TOOLCHAIN_FILE="$lls_root/cmake/mingw-w64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$lls_build" --target llstream_win llstream_send test_color_math test_hlsl \
    test_stage_log test_sender_history test_frame_map test_stats -j4
python3 - "$lls_build" <<'PY'
from pathlib import Path
import re
import subprocess
import sys

directory = Path(sys.argv[1])
for name in ('llstream_win.exe', 'llstream_send.exe', 'test_color_math.exe', 'test_hlsl.exe',
             'test_stage_log.exe', 'test_sender_history.exe', 'test_frame_map.exe', 'test_stats.exe'):
    output = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(directory/name)], text=True)
    imports = re.findall(r'DLL Name:\s*(\S+)', output)
    print(name + ': ' + ', '.join(imports))
    assert imports, name
    assert not any(re.search(r'lib(?:gcc|stdc\+\+|winpthread)', dll, re.I) for dll in imports), imports
    assert not any('nvencodeapi' in dll.lower() for dll in imports), 'NVENC must load at runtime'
PY
