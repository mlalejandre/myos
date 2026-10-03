#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"

DOCKER_IMAGE = "myos-toolchain"
QEMU = "qemu-system-x86_64"


def run(command: list[str], title: str) -> int:
    print()
    print("=" * 72)
    print(title)
    print("=" * 72)
    print()
    print("$", " ".join(command))
    print()

    result = subprocess.run(
        command,
        cwd=ROOT,
    )

    return result.returncode


def get_host_uid_gid() -> tuple[str, str]:
    uid = subprocess.check_output(
        ["id", "-u"],
        text=True,
    ).strip()

    gid = subprocess.check_output(
        ["id", "-g"],
        text=True,
    ).strip()

    return uid, gid


def docker_image_exists() -> bool:
    result = subprocess.run(
        [
            "docker",
            "image",
            "inspect",
            DOCKER_IMAGE,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    return result.returncode == 0


def main() -> int:

    print()
    print("=" * 72)
    print("MYOS DEVELOPMENT RUNNER")
    print("=" * 72)
    print()

    # --------------------------------------------------------
    # Comprobar entorno
    # --------------------------------------------------------

    required_files = [
        ROOT / "Dockerfile",
        ROOT / "Makefile",
        ROOT / "linker.ld",
    ]

    for file in required_files:

        if not file.exists():

            print(
                f"ERROR: falta {file.name}"
            )

            return 1

    if not shutil.which("docker"):

        print(
            "ERROR: Docker no está instalado "
            "o no está en PATH."
        )

        return 1

    if not shutil.which(QEMU):

        print(
            "ERROR: QEMU no está instalado "
            "o no está en PATH."
        )

        return 1

    # --------------------------------------------------------
    # Limpiar build
    # --------------------------------------------------------

    print("Limpiando compilación anterior...")

    if BUILD_DIR.exists():

        shutil.rmtree(BUILD_DIR)

        print(
            "[OK] build/ eliminado"
        )

    else:

        print(
            "[OK] build/ no existía"
        )

    # --------------------------------------------------------
    # Toolchain
    # --------------------------------------------------------

    if not docker_image_exists():

        code = run(
            [
                "docker",
                "build",
                "--platform",
                "linux/amd64",
                "-t",
                DOCKER_IMAGE,
                ".",
            ],
            "CONSTRUYENDO TOOLCHAIN",
        )

        if code != 0:

            print()
            print(
                "ERROR: fallo construyendo "
                "myos-toolchain."
            )

            return code

    else:

        print()
        print(
            "[OK] Docker image "
            "myos-toolchain disponible"
        )

    # --------------------------------------------------------
    # UID/GID del usuario del Mac
    # --------------------------------------------------------

    uid, gid = get_host_uid_gid()

    # --------------------------------------------------------
    # Compilar
    # --------------------------------------------------------

    code = run(
        [
            "docker",
            "run",
            "--rm",
            "--platform",
            "linux/amd64",
            "-u",
            f"{uid}:{gid}",
            "-v",
            f"{ROOT}:/myos",
            DOCKER_IMAGE,
            "make",
        ],
        "COMPILANDO MYOS",
    )

    if code != 0:

        print()
        print("=" * 72)
        print("COMPILACIÓN FALLIDA")
        print("=" * 72)
        print()
        print(
            "QEMU no se ejecutará."
        )
        print()

        return code

    # --------------------------------------------------------
    # Verificar resultados
    # --------------------------------------------------------

    hdd = BUILD_DIR / "hdd.img"
    if not hdd.exists():
        print()
        print("Creando disco persistente (hdd.img de 16 MiB)...")
        with open(hdd, "wb") as f:
            f.write(b"\x00" * (16 * 1024 * 1024))
        print("[OK] hdd.img creado")

    elf = BUILD_DIR / "myos.elf"
    iso = BUILD_DIR / "myos.iso"

    if not elf.exists():

        print()
        print(
            "ERROR: falta build/myos.elf"
        )

        return 1

    if not iso.exists():

        print()
        print(
            "ERROR: falta build/myos.iso"
        )

        return 1

    print()
    print("=" * 72)
    print("COMPILACIÓN CORRECTA")
    print("=" * 72)
    print()
    print(
        f"[OK] {elf}"
    )
    print(
        f"[OK] {iso}"
    )

    # --------------------------------------------------------
    # Arrancar QEMU
    # --------------------------------------------------------

    qemu_command = [
        QEMU,
        "-machine",
        "pc",
        "-m",
        "256M",
        "-no-reboot",
        "-d",
        "int",
        "-D",
        "build/qemu.log",
        "-cdrom",
        "build/myos.iso",
        "-netdev",
        "user,id=net0",
        "-device",
        "virtio-net-pci,netdev=net0",
        "-drive",
        "file=build/hdd.img,format=raw,if=virtio",
        "-object",
        "filter-dump,id=dump0,netdev=net0,file=build/net.pcap",
        "-serial",
        "stdio",
    ]

    print()
    print("=" * 72)
    print("ARRANCANDO MYOS")
    print("=" * 72)
    print()
    print(
        "Ctrl+C = detener QEMU"
    )
    print()

    try:

        code = subprocess.run(
            qemu_command,
            cwd=ROOT,
        ).returncode

    except KeyboardInterrupt:

        print()
        print(
            "QEMU detenido por el usuario."
        )

        return 0

    if code == 0:

        print()
        print(
            "[OK] QEMU terminó."
        )

    else:

        print()
        print(
            f"[AVISO] QEMU terminó "
            f"con código {code}."
        )

    return code


if __name__ == "__main__":
    sys.exit(main())