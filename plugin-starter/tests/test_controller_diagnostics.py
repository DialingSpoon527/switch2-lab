"""Run actual controller dispatch and report functions against deterministic mocks.

The new ABI header is real. Only existing SDK types/services are stubbed; this
does not emulate Horizon scheduling, IPC parsing, or hardware liveness.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from test_controller_io import extract_function

ROOT = Path(__file__).resolve().parents[1]


class ControllerDiagnosticsTests(unittest.TestCase):
    def test_diagnostic_dispatch_checkpoint_errors_and_legacy_api(self):
        source = (ROOT / 'examples/plugin-control/main.c').read_text()
        globals_to_copy = ('static const char* Command ', 'static const char* ResultPath ',
                           'static const char* ResultTemporary ', 'static const char* ResultPrevious ',
                           'static char report[', 'static bool report_truncated;',
                           'static bool report_truncation_noted;', '#define P2_TRY(')
        io_source = [line for line in source.splitlines() if line.startswith(globals_to_copy)]
        io_source += [extract_function(source, name) for name in
                      ('pathMissing', 'badInput', 'deleteIfPresent', 'saveResult', 'readCommand')]
        controller_source = [extract_function(source, name) for name in
                             ('note', 'append', 'checkpoint', 'phaseName', 'stateName', 'diagnostics', 'list')]
        controller_source.append(source[source.index('int main(void)'):].replace(
            'int main(void)', 'static int controllerMain(void)', 1))
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            (folder / 'controller_io_under_test.h').write_text('\n'.join(io_source) + '\n')
            (folder / 'controller_diagnostics_under_test.h').write_text('\n'.join(controller_source) + '\n')
            sdk = folder / 'kaihen'
            sdk.mkdir()
            shutil.copyfile(ROOT / 'tests/controller_protocol_stub.h', sdk / 'protocol.h')
            shutil.copyfile(ROOT.parent / 'hen-updater/libkaihen/include/kaihen/diagnostics.h', sdk / 'diagnostics.h')
            binary = folder / 'controller-diagnostics'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                           ['-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(folder), '-I', str(ROOT / 'examples/plugin-control'),
                            str(ROOT / 'tests/test_controller_diagnostics.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
