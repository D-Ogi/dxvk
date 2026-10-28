"""amdgpu-wddm: build and run the host test of the machine profile file (config_profile_test.cpp).

    python src/util/config/config_profile_test.py --clang-cl <clang-cl.exe> [--work <empty work dir>]

Builds the test with the configuration sources of this tree, once as the release flavour and once as the lab
flavour (AMDGPU_WDDM_LAB_FLAVOUR), and runs both.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("--clang-cl", required=True)
ap.add_argument("--work")
a = ap.parse_args()

util = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
root = os.path.abspath(os.path.join(util, "..", ".."))
work = os.path.abspath(a.work) if a.work else tempfile.mkdtemp(prefix="dxvk-profile-")
os.makedirs(work, exist_ok=True)
sources = [os.path.join(util, p) for p in (
    r"config\config_profile_test.cpp", r"config\config.cpp", r"config\config_profile.cpp", r"log\log.cpp",
    r"log\log_debug.cpp", r"util_env.cpp", r"util_string.cpp", r"thread.cpp", r"sha1\sha1_util.cpp")]
csources = [os.path.join(util, r"sha1\sha1.c")]
flags = ["/nologo", "/MT", "/EHsc", "/O1", "/Z7", "-DNOMINMAX", "-D_CRT_SECURE_NO_WARNINGS", "-I" + util,
         "-I" + os.path.join(root, "include"), "-I" + os.path.join(root, "src"), "-Wno-unused-parameter"]

failed = False
for flavour, defines in (("release", []), ("lab", ["-DAMDGPU_WDDM_LAB_FLAVOUR=1"])):
    out = os.path.join(work, flavour)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    objs = []
    for src in sources + csources:
        obj = os.path.join(out, os.path.splitext(os.path.basename(src))[0] + ".obj")
        std = ["/std:c++17"] if src.endswith(".cpp") else []
        r = subprocess.run([a.clang_cl, "/c"] + flags + std + defines + [src, "/Fo" + obj], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout[-6000:], r.stderr[-3000:])
            sys.exit(f"{flavour}: {os.path.basename(src)} does not compile")
        objs.append(obj)
    exe = os.path.join(out, "config_profile_test.exe")
    r = subprocess.run([a.clang_cl, "/nologo", "/MT"] + objs + ["/Fe" + exe, "/link", "advapi32.lib", "user32.lib"],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-6000:], r.stderr[-3000:])
        sys.exit(f"{flavour}: the test does not link")
    data = os.path.join(out, "profiles")
    r = subprocess.run([exe, data], capture_output=True, text=True, cwd=out)
    print(f"== {flavour}")
    print(r.stdout.rstrip())
    print("-- log lines")
    print(r.stderr.rstrip())
    failed |= r.returncode != 0
sys.exit(1 if failed else 0)
