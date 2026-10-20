"""Build a 3DS app with the toolchain set up for the Melee port.

    python3 client3ds/build.py              # the audio bridge client
    python3 client3ds/build.py --app probe_tls

That shim is clang + lld + the pinned libctru, with host tools named .exe so
the Melee scripts find them on macOS. Nothing here needs devkitARM's gcc.

Apps that speak TLS also need libmbedtls, which deps.py builds separately.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain

ROOT = Path(__file__).resolve().parent

# name -> (source directories, extra defines, mbedTLS flavour or None,
#          extra single sources relative to client3ds/)
APPS = {
    # The native source brings TLS and the whole Spotify protocol with it.
    "audio3ds":    (["source", "source/ui", "platform3ds"], [], "release",
                    ["deps/stb_vorbis.c", "deps/stb_image_impl.c"]),
    "probe_tls":   (["probe_tls", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_pair":  (["probe_pair", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_ap":    (["probe_ap", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_meta":  (["probe_meta", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    # No TLS: this one only reads a file off the card and decodes it.
    "probe_audio": (["probe_audio"], [], None, ["deps/stb_vorbis.c"]),
    # The whole chain: Spotify over TLS, plus the Vorbis decoder.
    "probe_play":  (["probe_play", "platform3ds"], ["-DPROBE_BUILD"], "probe",
                    ["deps/stb_vorbis.c"]),
}


def layout(sdk: Path) -> str:
    """libctru's 3dsx linker script, adjusted for lld.

    GNU ld infers the TLS layout; lld needs an explicit PT_TLS segment.
    3dsxtool only consumes the three PT_LOAD segments either way.
    """
    text = (sdk / "devkitARM/arm-none-eabi/lib/3dsx.ld").read_text()
    text = text.replace("data   PT_LOAD FLAGS(6)",
                        "tls    PT_TLS FLAGS(4);\n\tdata   PT_LOAD FLAGS(6)")
    for start, end in (("\t.tdata :", "\t.tbss :"), ("\t.tbss :", "\t/*")):
        a = text.index(start)
        b = text.index(end, a + len(start))
        text = text[:a] + text[a:b].replace(": data", ": data : tls") + text[b:]
    text = text.replace(".bss ALIGN(4)", ".bss ALIGN(8)")
    # lld otherwise orphans .got past __bss_start__, where the CRT clears it.
    return text.replace("*(.data.*)", "*(.data.*)\n\t\t*(.got .got.*)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", choices=sorted(APPS), default="audio3ds")
    ap.add_argument("--toolchain", type=Path, default=toolchain.DEFAULT_TOOLCHAIN)
    ap.add_argument("--debug", action="store_true", help="-O0 with assertions")
    args = ap.parse_args()

    app = args.app
    srcdirs, defines, mbed, extra_sources = APPS[app]
    tc = toolchain.find(args.toolchain)
    sdk, arm = tc.sdk, tc.arm

    out = ROOT / "build" / app
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%H:%M:%S")
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-D__3DS__",
             f'-DBUILD_STAMP="{stamp}"',
             '-DAPP_VERSION="1.2"',
             "-O0" if args.debug else "-O2", "-g",
             "-ffunction-sections", "-fdata-sections", "-fshort-enums", *defines]
    includes = list(tc.includes)
    # deps/ holds the vendored single-header libraries (stb_vorbis, stb_image).
    includes.append("-I" + str(ROOT / "deps"))
    if mbed:
        tree = ROOT / "deps/mbedtls-3.6.7"
        if not (tree / "include/mbedtls/ssl.h").exists():
            raise SystemExit("mbedTLS is missing; run: python3 client3ds/deps.py --probe")
        flags.append('-DMBEDTLS_CONFIG_FILE="mbedtls_config_3ds.h"')
        includes = ["-I" + str(ROOT / "platform3ds"),
                    "-I" + str(tree / "include")] + includes

    sources = [(d, src) for d in srcdirs for src in sorted((ROOT / d).glob("*.c"))]
    for rel in extra_sources:
        path = ROOT / rel
        if not path.exists():
            raise SystemExit(f"{path} is missing; run: python3 client3ds/deps.py")
        sources.append((Path(rel).parent.name, path))

    objects = []
    for d, src in sources:
        # Subdirectories (source/ui) would put a slash in the object name.
        obj = out / f"{str(d).replace('/', '_')}_{src.stem}.o"
        cmd = [str(tc.cc), *toolchain.ARCH, *flags, *includes,
               "-c", str(src), "-o", str(obj)]
        # Warnings in pinned upstream sources are not ours to fix.
        if "deps" in src.parts:
            cmd = [c for c in cmd if c not in ("-Wall", "-Wextra")] + ["-w"]
        subprocess.run(cmd, check=True)
        objects.append(obj)
    if not objects:
        raise SystemExit(f"No sources under {', '.join(srcdirs)}")

    libs = arm / "arm-none-eabi/lib/armv6k/fpu"
    gcc = sorted((arm / "lib/gcc/arm-none-eabi").iterdir())[-1] / "armv6k/fpu"
    script = out / "3dsx.ld"
    script.write_text(layout(sdk))

    extra = []
    if mbed:
        lib = ROOT / f"build/libmbedtls{'-probe' if mbed == 'probe' else ''}.a"
        if not lib.exists():
            raise SystemExit(f"{lib} is missing; run: python3 client3ds/deps.py"
                             f"{' --probe' if mbed == 'probe' else ''}")
        extra.append(str(lib))

    elf = out / f"{app}.elf"
    subprocess.run([str(tc.lld), "-T", str(script), "--gc-sections",
                    "-Map=" + str(out / f"{app}.map"),
                    str(libs / "3dsx_crt0.o"), str(gcc / "crti.o"),
                    str(gcc / "crtbegin.o"), *map(str, objects), *extra,
                    "-L" + str(sdk / "libctru/lib"), "-L" + str(libs), "-L" + str(gcc),
                    "--start-group", "-lcitro2d", "-lcitro3d", "-lctru", "-lm", "-lc",
                    "-lsysbase", "-lgcc", "--end-group",
                    str(gcc / "crtend.o"), str(gcc / "crtn.o"), "-o", str(elf)],
                   check=True)

    # Homebrew Launcher lists SD:/3ds/<name>/<name>.3dsx, so the folder and the
    # executable must share a name. The SMDH gives the entry its HOME-menu
    # icon and title; it is optional so probes stay brandless.
    dest = ROOT / f"dist/3ds/{app}/{app}.3dsx"
    dest.parent.mkdir(parents=True, exist_ok=True)
    pack = [str(tc.packer), str(elf), str(dest)]
    smdh = ROOT / "assets/icon.smdh"
    if app == "audio3ds" and smdh.exists():
        pack.append(f"--smdh={smdh}")
    subprocess.run(pack, check=True)
    print(f"\nBuilt {dest}  ({dest.stat().st_size:,} bytes)")
    if app == "audio3ds":
        print(f"Copy dist/3ds/ to the SD root, then edit "
              f"SD:/3ds/{app}/server.txt with your bridge's host:port")
        build_cia(ROOT, elf)


def build_cia(root: Path, elf: Path):
    """Assemble the FBI-installable CIA when the home-menu tools are around.
    Skipped silently otherwise: the .3dsx remains the primary artifact."""
    hm = Path("/Users/emiliorobles/melee-3ds/native-3ds/.toolchain/home-menu")
    makerom = hm / "makerom-bin/makerom"
    bannertool = hm / "bannertool-bin/mac-x86_64/bannertool"
    rsf = root / "assets/spotibacan.rsf"
    if not (makerom.exists() and bannertool.exists() and rsf.exists()):
        return
    subprocess.run([str(bannertool), "makebanner", "-i",
                    str(root / "assets/banner.png"), "-a",
                    str(root / "assets/silent.wav"), "-o",
                    str(root / "build/banner.bnr")], check=True)
    cia = root / "dist/SpotiBacan.cia"
    subprocess.run([str(makerom), "-f", "cia", "-o", str(cia), "-elf",
                    str(elf), "-rsf", str(rsf), "-target", "t", "-exefslogo",
                    "-icon", str(root / "assets/icon.smdh"), "-banner",
                    str(root / "build/banner.bnr"),
                    "-major", "1", "-minor", "2", "-micro", "0"], check=True)
    print(f"Built {cia}  ({cia.stat().st_size:,} bytes)  - install with FBI")


if __name__ == "__main__":
    main()
