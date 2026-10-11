#!/usr/bin/env bash
# Build and install branded metadata, then merge upstream translations without
# resolving conflicts or dropping accompanying source changes.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
python3 - "$SCRIPT_DIR/../.." <<'PY'
import configparser
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

repo = Path(sys.argv[1]).resolve()
metadata = (
    "org.kde.konsole.appdata.xml",
    "org.kde.konsole.desktop",
    "konsole.notifyrc",
    "konsolepart.desktop",
    "konsolerun.desktop",
)
appdata = "org.kde.konsole.appdata.xml"
caption = '      <caption xml:lang="ru">Konsole как эмулятор терминала</caption>'
updated_caption = '      <caption xml:lang="ru">Новый Konsole [ &amp; café; @VALUE@ ${VALUE}</caption>'


def run(*args, cwd, ok=True, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if ok and result.returncode:
        raise AssertionError(f"{' '.join(map(str, args))} failed:\n{result.stdout}")
    return result


def git(path, *args, **kwargs):
    return run("git", *args, cwd=path, **kwargs)


def commit(path, message, *paths):
    git(path, "add", *paths)
    git(path, "-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "--quiet", "-m", message)


def desktop(path):
    result = configparser.ConfigParser(interpolation=None)
    result.optionxform = str
    result.read(path, encoding="utf-8")
    return result


with tempfile.TemporaryDirectory(prefix="kmux-metadata-test.") as temporary:
    root = Path(temporary)
    upstream = root / "upstream"
    upstream.mkdir()
    git(upstream, "init", "--quiet", "--initial-branch=master")
    (upstream / "desktop").mkdir()
    for name in metadata:
        shutil.copyfile(repo / "desktop" / name, upstream / "desktop" / name)
    # Use a fixed old translation so this regression survives later KDE edits.
    xml = upstream / "desktop" / appdata
    text = xml.read_text()
    text, count = re.subn(r'      <caption xml:lang="ru">[^\n]*</caption>', caption, text)
    assert count == 1
    xml.write_text(text)
    (upstream / "main.cpp").write_text("upstream source\n")
    commit(upstream, "upstream base", ".")

    fork = root / "fork"
    git(root, "clone", "--quiet", str(upstream), str(fork))
    for name in ("CMakeLists.txt", "kmux-metadata.cmake"):
        shutil.copyfile(repo / "desktop" / name, fork / "desktop" / name)
    shutil.copytree(repo / "data" / "icons", fork / "data" / "icons")
    (fork / "CMakeLists.txt").write_text("""
cmake_minimum_required(VERSION 3.16)
project(MetadataTest NONE)
find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
set(KDE_INSTALL_APPDIR share/applications)
set(KDE_INSTALL_DATADIR share)
set(KDE_INSTALL_METAINFODIR share/metainfo)
set(KDE_INSTALL_KNOTIFYRCDIR share/knotifications6)
set(KDE_INSTALL_ICONDIR share/icons)
add_subdirectory(desktop)
""")
    commit(fork, "generate fork branding", "CMakeLists.txt", "desktop", "data")
    source_before = {name: (fork / "desktop" / name).read_bytes() for name in metadata}
    build = root / "build"
    prefix = root / "prefix"
    run("cmake", "-S", str(fork), "-B", str(build), "-G", "Ninja", f"-DCMAKE_INSTALL_PREFIX={prefix}", cwd=root)
    generated = build / "desktop"
    output = generated / "org.kde.kmux.appdata.xml"
    parsed = ET.parse(output).getroot()
    original = ET.parse(fork / "desktop" / appdata).getroot()
    assert parsed.findtext("id") == "org.kde.kmux.desktop"
    assert parsed.findtext("name") == "kmux"
    assert parsed.findtext("launchable") == "org.kde.kmux.desktop"
    assert parsed.findtext("provides/binary") == "kmux"
    assert [ET.tostring(e) for e in parsed.findall("url")] == [ET.tostring(e) for e in original.findall("url")]
    assert ET.tostring(parsed.find("developer")) == ET.tostring(original.find("developer"))
    assert parsed.findtext("update_contact") == original.findtext("update_contact")
    assert ET.tostring(parsed.find("releases")) == ET.tostring(original.find("releases"))
    assert 'xml:lang="ru">kmux как эмулятор терминала' in output.read_text()
    launcher = desktop(generated / "org.kde.kmux.desktop")
    entry = launcher["Desktop Entry"]
    for key, value in {"Name": "kmux", "TryExec": "kmux", "Exec": "kmux", "Icon": "kmux", "StartupWMClass": "kmux", "X-DocPath": "kmux/index.html"}.items():
        assert entry[key] == value, (key, entry[key])
    assert entry["Keywords"].endswith(";konsole;kmux")
    assert entry["Keywords[de]"] == desktop(fork / "desktop" / "org.kde.konsole.desktop")["Desktop Entry"]["Keywords[de]"]
    assert launcher["Desktop Action NewTab"]["Exec"] == "kmux --new-tab"
    notifications = desktop(generated / "kmux.notifyrc")["Global"]
    assert notifications["DesktopEntry"] == "org.kde.kmux"
    assert notifications["IconName"] == "kmux"
    assert desktop(generated / "kmuxpart.desktop")["Desktop Entry"]["X-KDE-Library"] == "kmuxpart"
    menu = desktop(generated / "kmuxrun.desktop")
    assert menu["Desktop Entry"]["Actions"] == "runInKmux;"
    assert menu["Desktop Action runInKmux"]["Exec"] == "kmux --hold -e %f"
    for name, expected in source_before.items():
        assert (fork / "desktop" / name).read_bytes() == expected
        assert not (fork / "desktop" / name.replace("konsole", "kmux")).exists()

    # Test the real desktop install rules in a disposable staging directory.
    install_env = dict(os.environ, DESTDIR=str(root / "stage"))
    run("cmake", "--install", str(build), cwd=root, env=install_env)
    installed = root / "stage" / prefix.relative_to("/")
    for target, source in {
        "share/applications/org.kde.kmux.desktop": "org.kde.kmux.desktop",
        "share/kglobalaccel/org.kde.kmux.desktop": "org.kde.kmux.desktop",
        "share/metainfo/org.kde.kmux.appdata.xml": "org.kde.kmux.appdata.xml",
        "share/knotifications6/kmux.notifyrc": "kmux.notifyrc",
        "share/kio/servicemenus/kmuxrun.desktop": "kmuxrun.desktop",
    }.items():
        assert (installed / target).read_bytes() == (generated / source).read_bytes(), target
    assert not list(installed.rglob("*konsole*"))
    run("desktop-file-validate", str(installed / "share/applications/org.kde.kmux.desktop"), cwd=root)
    run("appstreamcli", "validate", "--no-net", str(installed / "share/metainfo/org.kde.kmux.appdata.xml"), cwd=root)
    print("PASS: generated and installed metadata preserves kmux identity, translations and upstream links")

    # A second configure with identical input must not rewrite generated files.
    timestamp = output.stat().st_mtime_ns
    run("cmake", "-S", str(fork), "-B", str(build), cwd=root)
    assert output.stat().st_mtime_ns == timestamp

    # Translation changes join the next functional sync with no branded source
    # edits to conflict with. A normal build must regenerate the metadata.
    xml.write_text(xml.read_text().replace(caption, updated_caption))
    (upstream / "main.cpp").write_text("new upstream feature\n")
    commit(upstream, "translation and source update", "desktop", "main.cpp")
    git(fork, "fetch", "--quiet", "origin")
    git(fork, "-c", "user.name=Test", "-c", "user.email=test@example.com", "merge", "--no-edit", "origin/master")
    assert not git(fork, "ls-files", "--unmerged").stdout
    assert (fork / "main.cpp").read_text() == "new upstream feature\n"
    run("cmake", "--build", str(build), cwd=root)
    assert 'Новый kmux [ &amp; café; @VALUE@ ${VALUE}' in output.read_text()
    ET.parse(output)
    assert (fork / "desktop" / appdata).read_bytes() == xml.read_bytes()
    print("PASS: upstream translations merge cleanly and regenerate alongside source changes")

    # Functional conflicts must still be visible to the sync job.
    (fork / "main.cpp").write_text("fork behavior\n")
    commit(fork, "fork source edit", "main.cpp")
    (upstream / "main.cpp").write_text("different upstream behavior\n")
    commit(upstream, "upstream source edit", "main.cpp")
    git(fork, "fetch", "--quiet", "origin")
    result = git(fork, "-c", "user.name=Test", "-c", "user.email=test@example.com", "merge", "--no-edit", "origin/master", ok=False)
    assert result.returncode != 0
    assert git(fork, "diff", "--name-only", "--diff-filter=U").stdout.strip() == "main.cpp"
    print("PASS: genuine source conflicts remain unresolved")
PY
