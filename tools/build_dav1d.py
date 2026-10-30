"""Pinned, static 8-bit dav1d for the APK. Sources/build products stay in build/."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

from android_sdk import ROOT

VERSION = "1.5.4"
SHA256 = "686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5"
URL = f"https://downloads.videolan.org/pub/videolan/dav1d/{VERSION}/dav1d-{VERSION}.tar.xz"


def source():
    cache = ROOT / "build/toolchains/downloads"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"dav1d-{VERSION}.tar.xz"
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        with urllib.request.urlopen(URL, timeout=60) as reply, partial.open("wb") as output:
            shutil.copyfileobj(reply, output)
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError("dav1d source archive SHA-256 mismatch: " + str(archive))
    directory = ROOT / "build/toolchains" / f"dav1d-{VERSION}"
    if not directory.exists():
        with tempfile.TemporaryDirectory(dir=directory.parent) as temporary:
            with tarfile.open(archive) as package:
                # The pinned release needs only regular files/directories.
                for entry in package.getmembers():
                    path = Path(entry.name)
                    if path.is_absolute() or ".." in path.parts or not (entry.isfile() or entry.isdir()):
                        raise RuntimeError("Unsafe dav1d archive entry")
                package.extractall(temporary, filter="data")
            (Path(temporary) / f"dav1d-{VERSION}").rename(directory)
    return directory


def build(ndk, abi, meson):
    src = source()
    output = ROOT / "build/apk" / ("dav1d-" + abi)
    output.mkdir(parents=True, exist_ok=True)
    llvm = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    arm64 = abi == "arm64-v8a"
    cc = llvm / ("aarch64-linux-android28-clang" if arm64 else "armv7a-linux-androideabi28-clang")
    cross = output / "android.meson"
    cross.write_text(
        f"[binaries]\nc = '{cc}'\nar = '{llvm / 'llvm-ar'}'\nstrip = '{llvm / 'llvm-strip'}'\n"
        f"[host_machine]\nsystem = 'android'\ncpu_family = '{'aarch64' if arm64 else 'arm'}'\n"
        f"cpu = '{'armv8-a' if arm64 else 'armv7-a'}'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n")
    command = [meson, "setup", str(output), str(src), "--cross-file", str(cross),
               "--buildtype=release", "--default-library=static", "-Db_staticpic=true",
               "-Db_lto=true", "-Db_lto_mode=thin", "-Dbitdepths=8", "-Denable_tools=false",
               "-Denable_tests=false"]
    if arm64:
        # Tune scheduling for the tested CPU without requiring its newer ISA.
        command += ["-Dc_args=-mtune=cortex-a76"]
    if (output / "meson-private/coredata.dat").exists():
        command += ["--reconfigure"]
    subprocess.run(command, check=True)
    subprocess.run(["ninja", "-C", str(output), "-j", "4"], check=True)
    return src, output
