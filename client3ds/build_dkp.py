"""Build SpotiBacan with a stock devkitPro install (devkitARM + libctru + citro2d).

    python3 client3ds/build_dkp.py           # .3dsx and .cia in client3ds/dist/

Upstream's build.py targets the author's private clang shim; this one needs only
`dkp-pacman -S 3ds-dev` and makerom. Dependencies are pinned by SHA-256 and
fetched into deps/, which is never executed -- only compiled.
"""
import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DEPS = ROOT / "deps"
BUILD = ROOT / "build" / "dkp"
DIST = ROOT / "dist"

DKP = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
CC = DKP / "devkitARM/bin/arm-none-eabi-gcc"
AR = DKP / "devkitARM/bin/arm-none-eabi-gcc-ar"
TOOL_3DSX = DKP / "tools/bin/3dsxtool"
MAKEROM = Path(os.environ.get("MAKEROM", ROOT.parent / "tools/makerom/makerom"))

VERSION = (1, 8, 4)   # shown in Settings and stamped into the CIA

ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]

STB = "2c980bb59875b0d32144a71867fbdebb2f77cd20"
PINS = {
    "stb_vorbis.c": (f"https://raw.githubusercontent.com/nothings/stb/{STB}/stb_vorbis.c",
                     "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636"),
    "stb_image.h": (f"https://raw.githubusercontent.com/nothings/stb/{STB}/stb_image.h",
                    "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3"),
    "mbedtls-3.6.7.tar.bz2": (
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2",
        "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"),
}
MBED = DEPS / "mbedtls-3.6.7"

APP_DIRS = ["source", "source/ui", "platform3ds"]
APP_EXTRA = ["vendor/stb_vorbis_3ds.c", "vendor/stb_image_impl.c"]


def fetch():
    DEPS.mkdir(exist_ok=True)
    for name, (url, sha) in PINS.items():
        dst = DEPS / name
        if not dst.exists():
            print(f"Downloading {name}...")
            with urllib.request.urlopen(url, timeout=120) as r:
                dst.write_bytes(r.read())
        got = hashlib.sha256(dst.read_bytes()).hexdigest()
        if got != sha:
            raise SystemExit(f"{dst} hashes {got}, expected {sha}")
    if not (MBED / "library/ssl_tls.c").exists():
        with tarfile.open(DEPS / "mbedtls-3.6.7.tar.bz2") as t:
            t.extractall(DEPS, filter="data")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"failed: {cmd[-1]}")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)


def compile_all(pairs, flags, jobs):
    def one(pair):
        src, obj = pair
        if obj.exists() and obj.stat().st_mtime > max(src.stat().st_mtime, NEWEST):
            return
        run([str(CC), *ARCH, *flags, "-c", str(src), "-o", str(obj)])
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        list(ex.map(one, pairs))


def main():
    global NEWEST
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-j", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args()

    for tool in (CC, TOOL_3DSX):
        if not tool.exists():
            raise SystemExit(f"Missing {tool}; install devkitPro: dkp-pacman -S 3ds-dev")
    if args.clean and BUILD.exists():
        shutil.rmtree(BUILD)
    fetch()

    headers = [*ROOT.glob("platform3ds/*.h"), *ROOT.glob("source/**/*.h")]
    NEWEST = max([Path(__file__).stat().st_mtime] + [h.stat().st_mtime for h in headers])

    common = ["-std=gnu11", "-O2", "-g", "-ffunction-sections", "-fdata-sections",
              "-D__3DS__", '-DMBEDTLS_CONFIG_FILE="mbedtls_config_3ds.h"',
              "-I" + str(ROOT / "platform3ds"), "-I" + str(MBED / "include"),
              "-I" + str(DKP / "libctru/include")]

    # mbedTLS: upstream warnings are not ours to fix.
    mdir = BUILD / "mbedtls"
    mdir.mkdir(parents=True, exist_ok=True)
    msrc = sorted((MBED / "library").glob("*.c"))
    mpairs = [(s, mdir / (s.stem + ".o")) for s in msrc]
    compile_all(mpairs, common + ["-I" + str(MBED / "library"), "-w"], args.j)
    lib = BUILD / "libmbedtls.a"
    if not lib.exists() or lib.stat().st_mtime < max(o.stat().st_mtime for _, o in mpairs):
        lib.unlink(missing_ok=True)
        run([str(AR), "rcs", str(lib), *(str(o) for _, o in mpairs)])

    adir = BUILD / "app"
    adir.mkdir(parents=True, exist_ok=True)
    pairs = []
    for d in APP_DIRS:
        for s in sorted((ROOT / d).glob("*.c")):
            pairs.append((s, adir / f"{d.replace('/', '_')}_{s.stem}.o"))
    for rel in APP_EXTRA:
        s = ROOT / rel
        pairs.append((s, adir / f"vendor_{s.stem}.o"))
    app_flags = common + ["-Wall", "-Wextra", "-I" + str(DEPS), "-I" + str(ROOT / "source"),
                          f'-DBUILD_STAMP="{__import__("time").strftime("%H:%M:%S")}"',
                          '-DAPP_VERSION="%d.%d"' % VERSION[:2]]
    compile_all([p for p in pairs if "vendor" not in p[0].parts], app_flags, args.j)
    compile_all([p for p in pairs if "vendor" in p[0].parts],
                [f for f in app_flags if f not in ("-Wall", "-Wextra")] + ["-w"], args.j)

    elf = BUILD / "SpotiBacan.elf"
    run([str(CC), "-specs=3dsx.specs", *ARCH, "-g", "-Wl,--gc-sections",
         f"-Wl,-Map,{BUILD / 'SpotiBacan.map'}", *(str(o) for _, o in pairs), str(lib),
         "-L" + str(DKP / "libctru/lib"), "-lcitro2d", "-lcitro3d", "-lctru", "-lm",
         "-o", str(elf)])

    out = DIST / "3ds/spotibacan/spotibacan.3dsx"
    out.parent.mkdir(parents=True, exist_ok=True)
    run([str(TOOL_3DSX), str(elf), str(out), f"--smdh={ROOT / 'assets/icon.smdh'}"])
    print(f"Built {out}  ({out.stat().st_size:,} bytes)")

    if not MAKEROM.exists():
        print(f"No makerom at {MAKEROM}; skipping the CIA")
        return
    cia = DIST / "SpotiBacan.cia"
    run([str(MAKEROM), "-f", "cia", "-o", str(cia), "-elf", str(elf),
         "-rsf", str(ROOT / "assets/spotibacan.rsf"), "-target", "t", "-exefslogo",
         "-icon", str(ROOT / "assets/icon.smdh"), "-banner", str(ROOT / "assets/banner.bnr"),
         "-major", str(VERSION[0]), "-minor", str(VERSION[1]), "-micro", str(VERSION[2])])
    print(f"Built {cia}  ({cia.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
