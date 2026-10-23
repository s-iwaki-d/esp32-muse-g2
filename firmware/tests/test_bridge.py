"""Compile real bounded reply code, including malformed/cancelled/multibyte cases."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ReplyCore(unittest.TestCase):
    def test_reply_boundaries_and_correlation(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / 'test-reply')
            subprocess.run([
                *shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-I', str(ROOT / 'src'),
                str(ROOT / 'src/bridge_reply.c'), str(ROOT / 'tests/test_reply.c'), '-o', binary,
            ], check=True)
            subprocess.run([binary], check=True)

if __name__ == '__main__':
    unittest.main()
