"""Pinned Google SDK packages, fetched without installing sdkmanager or Gradle."""
import hashlib
import json
import os
import posixpath
from pathlib import Path
import shutil
import stat
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "https://dl.google.com/android/repository/"
BUILD_TOOLS = "35.0.0"
PLATFORM = "33"
NDK_REVISION = "27.3.13750724"
TOOLCHAINS = ROOT / "build/toolchains"


def properties(path):
    if not path.is_file():
        return {}
    return dict(tuple(part.strip() for part in line.split("=", 1)) for line in
                (line.strip() for line in path.read_text().splitlines())
                if "=" in line and not line.startswith("#"))


def package_info(xml, package):
    tree = ET.fromstring(xml)
    # Namespace versions change; package element names and attributes do not.
    for elem in tree.iter():
        elem.tag = elem.tag.rsplit("}", 1)[-1]
    for elem in tree.findall("remotePackage"):
        if elem.get("path") != package:
            continue
        for archive in elem.findall("archives/archive"):
            if archive.findtext("host-os", "linux") != "linux":
                continue
            complete = archive.find("complete")
            checksum = complete.find("checksum")
            url = urllib.parse.urljoin(REPOSITORY, complete.findtext("url"))
            if not url.startswith(REPOSITORY):
                raise RuntimeError("Unexpected SDK download host")
            return {"url": url, "size": int(complete.findtext("size")),
                    "algorithm": checksum.get("type", "sha1"), "checksum": checksum.text.strip()}
    raise RuntimeError("Google repository has no Linux archive for " + package)


def extract(archive, destination):
    with zipfile.ZipFile(archive) as source:
        links = {entry.filename.rstrip("/") for entry in source.infolist()
                 if stat.S_ISLNK(entry.external_attr >> 16)}
        for entry in source.infolist():
            parts = Path(entry.filename).parts
            mode = entry.external_attr >> 16
            if not parts or Path(entry.filename).is_absolute() or ".." in parts or any(
                    str(parent) in links for parent in Path(entry.filename).parents):
                raise RuntimeError("Unsafe SDK archive entry: " + entry.filename)
            if stat.S_ISLNK(mode):
                continue
            source.extract(entry, destination)
            if mode & 0o111 and not entry.is_dir():
                (destination / entry.filename).chmod(0o755)
        # The NDK contains relative compiler links (clang++ -> clang -> clang-18).
        # Create links only after file extraction, then validate their full chains.
        for name in sorted(links):
            target = source.read(name).decode("utf-8")
            normalized = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
            if target.startswith("/") or normalized == ".." or normalized.startswith("../"):
                raise RuntimeError("Unsafe SDK symlink: " + name)
            path = destination / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(target)
        for name in links:
            try:
                (destination / name).resolve().relative_to(destination.resolve())
            except (ValueError, RuntimeError) as exc:
                raise RuntimeError("Unsafe SDK symlink chain: " + name) from exc


def fetch(package, destination):
    TOOLCHAINS.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(REPOSITORY + "repository2-3.xml", timeout=60) as reply:
        xml = reply.read()
    info = package_info(xml, package)
    cache = TOOLCHAINS / "downloads"
    cache.mkdir(exist_ok=True)
    (cache / "repository2-3.xml").write_bytes(xml)
    archive = cache / Path(urllib.parse.urlparse(info["url"]).path).name

    def valid():
        if not archive.is_file() or archive.stat().st_size != info["size"]:
            return False
        with archive.open("rb") as file:
            return hashlib.file_digest(file, info["algorithm"]).hexdigest() == info["checksum"]

    if not valid():
        print("Downloading", info["url"], flush=True)
        partial = archive.with_suffix(".partial")
        with urllib.request.urlopen(info["url"], timeout=60) as reply, partial.open("wb") as file:
            shutil.copyfileobj(reply, file)
        partial.replace(archive)
        if not valid():
            archive.unlink()
            raise RuntimeError("Google SDK checksum or size mismatch for " + package)
    print("Verified", package, info["algorithm"], info["checksum"], flush=True)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=destination.parent) as directory:
        stage = Path(directory)
        extract(archive, stage)
        roots = list(stage.iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise RuntimeError("Unexpected SDK archive layout")
        if destination.exists():
            raise RuntimeError("Refusing to replace incomplete SDK directory: " + str(destination))
        roots[0].rename(destination)
    (cache / (package.replace(";", "-") + ".json")).write_text(json.dumps(info, indent=2) + "\n")
    return destination


def sdk_matches(sdk):
    return (properties(sdk / "build-tools" / BUILD_TOOLS / "source.properties").get("Pkg.Revision") == BUILD_TOOLS
            and properties(sdk / "platforms" / ("android-" + PLATFORM) / "source.properties").get("AndroidVersion.ApiLevel") == PLATFORM)


def ensure_sdk(requested=None):
    if requested:
        sdk = Path(requested).resolve()
        if not sdk_matches(sdk):
            raise RuntimeError("--sdk needs build-tools 35.0.0 and platform 33: " + str(sdk))
        return sdk
    sdk = TOOLCHAINS / "android-sdk"
    for package, target, key, expected in (
        ("build-tools;" + BUILD_TOOLS, sdk / "build-tools" / BUILD_TOOLS, "Pkg.Revision", BUILD_TOOLS),
        ("platforms;android-" + PLATFORM, sdk / "platforms" / ("android-" + PLATFORM), "AndroidVersion.ApiLevel", PLATFORM),
    ):
        if properties(target / "source.properties").get(key) != expected:
            fetch(package, target)
    return sdk


def ensure_ndk(requested=None):
    if requested:
        ndk = Path(requested).resolve()
        if properties(ndk / "source.properties").get("Pkg.Revision") != NDK_REVISION:
            raise RuntimeError("--ndk must be r27d (" + NDK_REVISION + ")")
        return ndk
    candidates = [TOOLCHAINS / "android-ndk-r27d"]
    for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if os.environ.get(name):
            candidates.append(Path(os.environ[name]))
    for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(name):
            candidates.append(Path(os.environ[name]) / "ndk" / NDK_REVISION)
    for ndk in candidates:
        if properties(ndk / "source.properties").get("Pkg.Revision") == NDK_REVISION:
            return ndk.resolve()
    return fetch("ndk;" + NDK_REVISION, candidates[0])
