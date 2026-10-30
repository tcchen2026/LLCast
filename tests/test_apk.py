"""Offline checks for APK packaging and root command construction; no ADB."""
import io
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import android_sdk
import build_dav1d
from device_session import session_script


class ApkTests(unittest.TestCase):
    def test_dav1d_rejects_corrupt_cached_source_before_extraction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / "build/toolchains/downloads"
            cache.mkdir(parents=True)
            (cache / f"dav1d-{build_dav1d.VERSION}.tar.xz").write_bytes(b"incomplete archive")
            with patch.object(build_dav1d, "ROOT", root), self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                build_dav1d.source()
            self.assertFalse((root / "build/toolchains" / f"dav1d-{build_dav1d.VERSION}").exists())

    def test_repository_checksum_selection(self):
        xml = b'''<r:repository xmlns:r="urn:test"><remotePackage path="build-tools;35.0.0">
          <archives><archive><host-os>windows</host-os><complete><url>wrong.zip</url></complete></archive>
          <archive><host-os>linux</host-os><complete><url>build-tools_r35_linux.zip</url><size>42</size>
          <checksum type="sha1">abc</checksum></complete></archive></archives>
        </remotePackage></r:repository>'''
        info = android_sdk.package_info(xml, "build-tools;35.0.0")
        self.assertEqual(info["url"], android_sdk.REPOSITORY + "build-tools_r35_linux.zip")
        self.assertEqual((info["algorithm"], info["checksum"], info["size"]), ("sha1", "abc", 42))
        with self.assertRaises(RuntimeError):
            android_sdk.package_info(xml, "missing")

    def test_zip_cannot_write_outside_staging(self):
        for path in ("../escape", "/absolute"):
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as file:
                file.writestr(path, "no")
            data.seek(0)
            with tempfile.TemporaryDirectory() as directory, self.assertRaises(RuntimeError):
                android_sdk.extract(data, Path(directory))

    def test_ndk_compiler_symlinks_and_escape(self):
        for target in ("clang-18", "../../outside"):
            data = io.BytesIO()
            with zipfile.ZipFile(data, "w") as file:
                file.writestr("bin/clang-18", "compiler")
                link = zipfile.ZipInfo("bin/clang")
                link.create_system = 3
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                file.writestr(link, target)
            data.seek(0)
            with tempfile.TemporaryDirectory() as directory:
                if target == "clang-18":
                    android_sdk.extract(data, Path(directory))
                    self.assertTrue((Path(directory) / "bin/clang").is_symlink())
                    self.assertEqual((Path(directory) / "bin/clang").read_text(), "compiler")
                else:
                    with self.assertRaises(RuntimeError):
                        android_sdk.extract(data, Path(directory))

    def test_embedded_guard_is_unlimited_and_executes_installed_paths(self):
        script = session_script(None, [], 0, display=True, apk=True)
        subprocess.run(["sh", "-n"], input=script, text=True, check=True)
        self.assertIn('"$4" --watch-back "$$"', script)
        self.assertIn('"$2" --display --drm-card "$3"', script)
        self.assertIn("--stop-hint", script)
        self.assertNotIn("--csv", script)
        self.assertNotIn("--seconds", script)
        self.assertNotIn("watchdog ", script.split("lls_child=$!", 1)[1])
        self.assertIn("setprop ctl.restart zygote", script)
        self.assertLess(script.index('wait "$lls_child"'), script.index("start surfaceflinger"))
        self.assertLess(script.index("echo READY >&5"), script.index("|| stop surfaceflinger"))
        self.assertLess(script.index("then pause_framework || exit"), script.index("|| stop surfaceflinger"))
        self.assertLess(script.index("start surfaceflinger"), script.index("      resume_framework"))

    @unittest.skipUnless(shutil.which("javac") and shutil.which("java"), "JDK required")
    def test_java_protocol_and_command_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["javac", "-d", directory,
                str(ROOT / "android/src/org/llcast/CheckProtocol.java"),
                str(ROOT / "android/src/org/llcast/EncoderSize.java"),
                str(ROOT / "tests/CheckProtocolTest.java")], check=True)
            subprocess.run(["java", "-cp", directory, "org.llcast.CheckProtocolTest"], check=True)


if __name__ == "__main__":
    unittest.main()
