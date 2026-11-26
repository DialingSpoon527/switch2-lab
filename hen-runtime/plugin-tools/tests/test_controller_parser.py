"""Compile and run the actual SDK-independent C parser used by the controller."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ControllerParserTests(unittest.TestCase):
    def test_actual_parser_rejects_ambiguous_commands(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'controller-parser'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/test_controller_parser.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
