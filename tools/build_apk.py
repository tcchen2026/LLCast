#!/usr/bin/env python3
"""Build and sign LLCast.apk using the Android command-line tools, without Gradle."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import zipfile

from android_sdk import BUILD_TOOLS, PLATFORM, ROOT, ensure_ndk, ensure_sdk, sdk_matches
from generate_apk_sources import guard_header
import build_dav1d

APPLICATION_ID = "io.github.tcchen2026.llcast"  # the Mac bundle ID; Java code stays in org.llcast
# One APK: Extreme mode is arm64 only; Normal mode and the sender also run on 32-bit ARM.
PACKAGED_LIBRARIES = ("lib/arm64-v8a/libllcast_rx.so", "lib/arm64-v8a/libllcast_mpp.so", "lib/arm64-v8a/libllcast_session.so",
                      "lib/arm64-v8a/libllcast.so", "lib/armeabi-v7a/libllcast.so")


def run(command, **kwargs):
    print("+ " + shlex.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def version():
    name = subprocess.check_output(["sh", ROOT / "tools/current-tree-version.sh"], text=True).strip().removeprefix("v")
    code = 1
    if name != "0.0.0-dev":
        timestamp = subprocess.check_output(["git", "log", "-1", "--format=%ct"], cwd=ROOT, text=True)
        code = int(timestamp) // 60
        if not 0 < code < 2**31:
            raise RuntimeError("Commit time is outside the Android versionCode range")
    return name, code


def link_interfaces(compiler, output):
    """Only satisfy the static linker. The APK uses /vendor/lib64 at runtime.

    CI has no box/proprietary binaries. These trap-only symbol providers are
    never packaged, installed or placed on the runtime library search path.
    Header types still check every call. Missing new imports fail the build.
    """
    output.mkdir(parents=True, exist_ok=True)
    for library in ("mpp", "drm"):
        names = (ROOT / "android/native" / (library + "-symbols.txt")).read_text().splitlines()
        assembly = [".text"]
        for name in names:
            if name.startswith("#") or not name:
                continue
            if not re.fullmatch(r"[a-zA-Z_][a-zA-Z_0-9]*", name):
                raise RuntimeError("Invalid interface symbol: " + name)
            assembly += [".global " + name, ".type " + name + ", %function", name + ":", "  brk #0"]
        source = output / (library + ".S")
        source.write_text("\n".join(assembly) + "\n")
        run([compiler, "-shared", "-nostdlib", "-Wl,-soname,lib" + library + ".so",
             source, "-o", output / ("lib" + library + ".so")])


def signing(build):
    env = dict(os.environ)
    supplied = bool(env.get("LLCAST_KEYSTORE") or env.get("LLCAST_KEYSTORE_BASE64"))
    if env.get("LLCAST_KEYSTORE") and env.get("LLCAST_KEYSTORE_BASE64"):
        raise RuntimeError("Set only LLCAST_KEYSTORE or LLCAST_KEYSTORE_BASE64")
    if env.get("LLCAST_KEYSTORE_BASE64"):
        directory = build / "signing"
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        key = directory / "release.p12"
        # Restrict permissions at creation, not after writing private bytes.
        fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as file:
            file.write(base64.b64decode(env["LLCAST_KEYSTORE_BASE64"], validate=True))
    elif env.get("LLCAST_KEYSTORE"):
        key = Path(env["LLCAST_KEYSTORE"]).resolve()
    else:
        directory = ROOT / "build/signing"
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        key = directory / "llcast-development.p12"
        env["LLCAST_STORE_PASSWORD"] = "android"
        env["LLCAST_KEY_PASSWORD"] = "android"
        env["LLCAST_KEY_ALIAS"] = "llcast-development"
        if not key.exists():
            run(["keytool", "-genkeypair", "-keystore", key, "-storetype", "PKCS12",
                 "-alias", env["LLCAST_KEY_ALIAS"], "-keyalg", "RSA", "-keysize", "3072",
                 "-validity", "10000", "-dname", "CN=LLCast Development",
                 "-storepass:env", "LLCAST_STORE_PASSWORD", "-keypass:env", "LLCAST_KEY_PASSWORD"], env=env)
            key.chmod(0o600)
    for name in ("LLCAST_STORE_PASSWORD", "LLCAST_KEY_ALIAS"):
        if not env.get(name):
            raise RuntimeError("Release signing needs " + name)
    if not env.get("LLCAST_KEY_PASSWORD"):
        env["LLCAST_KEY_PASSWORD"] = env["LLCAST_STORE_PASSWORD"]
    if not key.is_file():
        raise RuntimeError("Signing keystore not found: " + str(key))
    return key, env, "release" if supplied else "development"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, help="SDK containing build-tools 35.0.0 and platform 33")
    parser.add_argument("--ndk", type=Path, help="NDK r27d (27.3.13750724)")
    parser.add_argument("--output", type=Path, default=ROOT / "build/apk/LLCast.apk")
    parser.add_argument("--ci", action="store_true", help="Use the runner SDK only when pinned versions are present")
    parser.add_argument("--meson", default="meson", help="Meson executable for the bundled dav1d decoder")
    args = parser.parse_args(argv)
    for executable in ("cmake", "javac", "java", "keytool", "zip", "ninja", args.meson):
        if not shutil.which(executable):
            parser.error("Missing required tool: " + executable)
    if sys.platform != "linux":
        parser.error("This builder currently uses the Linux x86_64 SDK/NDK tools")
    requested = args.sdk
    if args.ci and not requested:
        for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
            candidate = os.environ.get(variable)
            if candidate and sdk_matches(Path(candidate)):
                requested = Path(candidate)
                break
    sdk, ndk = ensure_sdk(requested), ensure_ndk(args.ndk)
    version_name, version_code = version()
    build = ROOT / "build/apk"
    build.mkdir(parents=True, exist_ok=True)
    tools = sdk / "build-tools" / BUILD_TOOLS
    platform = sdk / "platforms" / ("android-" + PLATFORM) / "android.jar"
    llvm = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    compiler = llvm / "aarch64-linux-android28-clang"
    interfaces = build / "link-interfaces"
    link_interfaces(compiler, interfaces)
    native = build / "native"
    dav1d_source, dav1d64 = build_dav1d.build(ndk, "arm64-v8a", args.meson)
    run(["cmake", "-S", ROOT, "-B", native,
         "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"),
         "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", "-DCMAKE_BUILD_TYPE=Release",
         "-DMPP_LIBRARY=" + str(interfaces / "libmpp.so"), "-DDRM_LIBRARY=" + str(interfaces / "libdrm.so"),
         "-DDAV1D_SOURCE=" + str(dav1d_source), "-DDAV1D_BUILD=" + str(dav1d64)])
    run(["cmake", "--build", native, "--target", "llstream_rx", "llcast_mpp", "llcast", "-j", "4"])
    # Normal mode and the sender also run on 32-bit userlands (many TV sticks); same APK.
    native32 = build / "native-armeabi-v7a"
    _, dav1d32 = build_dav1d.build(ndk, "armeabi-v7a", args.meson)
    run(["cmake", "-S", ROOT, "-B", native32,
         "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"),
         "-DANDROID_ABI=armeabi-v7a", "-DANDROID_PLATFORM=android-28", "-DCMAKE_BUILD_TYPE=Release",
         "-DDAV1D_SOURCE=" + str(dav1d_source), "-DDAV1D_BUILD=" + str(dav1d32)])
    run(["cmake", "--build", native32, "--target", "llcast", "-j", "4"])
    # Recreate packaging inputs to avoid retaining classes/files deleted in source.
    stage = build / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    generated = stage / "generated"
    generated.mkdir()
    (generated / "session_script.h").write_text(guard_header())
    libs = stage / "lib/arm64-v8a"
    libs.mkdir(parents=True)
    run([llvm / "llvm-strip", "--strip-unneeded", "-o", libs / "libllcast_rx.so", native / "llstream_rx"])
    run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wformat=2",
         "-fPIE", "-pie", "-Wl,-z,max-page-size=16384", "-I", generated, ROOT / "android/native/llcast_session.c",
         "-o", libs / "libllcast_session.so"])
    run([llvm / "llvm-strip", "--strip-unneeded", libs / "libllcast_session.so"])
    run([llvm / "llvm-strip", "--strip-unneeded", "-o", libs / "libllcast.so", native / "libllcast.so"])
    run([llvm / "llvm-strip", "--strip-unneeded", "-o", libs / "libllcast_mpp.so", native / "libllcast_mpp.so"])
    libs32 = stage / "lib/armeabi-v7a"
    libs32.mkdir(parents=True)
    run([llvm / "llvm-strip", "--strip-unneeded", "-o", libs32 / "libllcast.so", native32 / "libllcast.so"])
    for library in [*libs.iterdir(), *libs32.iterdir()]:
        library.chmod(0o755)
    resources = stage / "resources.zip"
    run([tools / "aapt2", "compile", "--dir", ROOT / "android/res", "-o", resources])
    unsigned = stage / "unsigned.apk"
    run([tools / "aapt2", "link", "-o", unsigned, "--manifest", ROOT / "android/AndroidManifest.xml",
         "--rename-manifest-package", APPLICATION_ID,
         "--version-code", str(version_code), "--version-name", version_name,
         "-I", platform, "--java", generated, "--auto-add-overlay", resources])
    classes = stage / "classes"
    classes.mkdir()
    java = sorted((ROOT / "android/src").rglob("*.java")) + sorted(generated.rglob("*.java"))
    run(["javac", "-encoding", "UTF-8", "--release", "8", "-Xlint:-options", "-classpath", platform, "-d", classes, *java])
    dex = stage / "dex"
    dex.mkdir()
    run([tools / "d8", "--release", "--min-api", "28", "--lib", platform, "--output", dex,
         *sorted(classes.rglob("*.class"))])
    for file in dex.glob("*.dex"):
        shutil.copy2(file, stage / file.name)
    run(["zip", "-q", unsigned, *[file.name for file in dex.glob("*.dex")], *PACKAGED_LIBRARIES], cwd=stage)
    notices = stage / "assets"
    notices.mkdir()
    shutil.copy2(dav1d_source / "COPYING", notices / "dav1d-COPYING.txt")
    run(["zip", "-q", unsigned, "assets/dav1d-COPYING.txt"], cwd=stage)
    aligned = stage / "aligned.apk"
    run([tools / "zipalign", "-f", "-p", "4", unsigned, aligned])
    key, env, kind = signing(build)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    run([tools / "apksigner", "sign", "--ks", key, "--ks-key-alias", env["LLCAST_KEY_ALIAS"],
         "--ks-pass", "env:LLCAST_STORE_PASSWORD", "--key-pass", "env:LLCAST_KEY_PASSWORD",
         "--out", output, aligned], env=env)
    run([tools / "apksigner", "verify", "--verbose", output])
    run([tools / "zipalign", "-c", "4", output])
    with zipfile.ZipFile(output) as apk:
        packaged = {name for name in apk.namelist() if name.startswith("lib/")}
        if packaged != set(PACKAGED_LIBRARIES):
            raise RuntimeError("Unexpected native libraries in APK")
    metadata = {"apk": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                "version_name": version_name, "version_code": version_code,
                "signing": kind, "sdk": str(sdk), "ndk": str(ndk), "min_sdk": 28, "target_sdk": 33,
                "vendor_libraries": "Runtime box libraries only; link-only interfaces are not packaged",
                "dav1d": {"version": build_dav1d.VERSION, "source_sha256": build_dav1d.SHA256, "bitdepth": 8},
                "guard_sha256": hashlib.sha256(guard_header().encode()).hexdigest()}
    (build / "build.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Built", output, "SHA256", metadata["sha256"], flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("ERROR:", exc, file=sys.stderr)
        sys.exit(1)
