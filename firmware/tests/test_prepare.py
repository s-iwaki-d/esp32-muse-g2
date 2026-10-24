"""A generated SDK inside a Git checkout must receive, not skip, its patches."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare_firmware", ROOT / "scripts/prepare_firmware.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class PatchPreparation(unittest.TestCase):
    def test_patch_applied_inside_parent_git_repository(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            subprocess.run(["git", "init", "-q", str(parent)], check=True)
            target = parent / ".build/sdk"
            source = target / "esp32/component.c"
            source.parent.mkdir(parents=True)
            source.write_text("before\n")
            patch = parent / "overlay.patch"
            patch.write_text("""diff --git a/esp32/component.c b/esp32/component.c
--- a/esp32/component.c
+++ b/esp32/component.c
@@ -1 +1 @@
-before
+after
""")
            prepare.apply_patch(patch, target)
            self.assertEqual(source.read_text(), "after\n")
            self.assertFalse((parent / "esp32/component.c").exists())


if __name__ == "__main__":
    unittest.main()
