"""Exercise actual controller FS code with failures; not a Horizon emulation."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

def extract_function(source, name):
    # These exact leaf functions contain no brace-valued string literals.
    marker = 'static '
    start = source.index(marker, source.rfind('\n', 0, source.index(name + '(')) + 1)
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        if source[cursor] == '{': depth += 1
        elif source[cursor] == '}': depth -= 1
        cursor += 1
    return source[start:cursor]

class ControllerIOTests(unittest.TestCase):
    def test_real_read_and_save_functions_handle_io_failures(self):
        source = (ROOT / 'examples/plugin-control/main.c').read_text()
        globals_to_copy = ('static const char* Command ', 'static const char* ResultPath ',
                           'static const char* ResultTemporary ', 'static const char* ResultPrevious ',
                           'static char report[', 'static bool report_truncated;', 'static bool report_truncation_noted;', '#define P2_TRY(')
        selected = [line for line in source.splitlines() if line.startswith(globals_to_copy)]
        selected += [extract_function(source, name) for name in ('pathMissing', 'badInput', 'deleteIfPresent', 'saveResult', 'readCommand')]
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / 'controller_io_under_test.h').write_text('\n'.join(selected) + '\n')
            binary = folder / 'controller-io'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-I', str(folder), '-I', str(ROOT / 'examples/plugin-control'),
                           str(ROOT / 'tests/test_controller_io.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
