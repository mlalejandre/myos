#!/usr/bin/env python3
# MYOS DEVELOPMENT RUNNER - THE SINGULARITY LOOP (v2)
#
# Protocolo del buzon (LBA 1..16, 8 KiB):
#   [0..7]  "PATCHv02"
#   [8..11] longitud u32 little-endian
#   [12..]  texto: uno o mas bloques
#             FILE: src/archivo.c
#             <<<<<<< SEARCH
#             texto exacto (debe aparecer UNA vez)
#             =======
#             texto nuevo
#             >>>>>>> REPLACE
# Resultado para la IA: LBA 20..23 (2 KiB, texto ASCII). Leer con 'sector_read 20'.

from pathlib import Path
import argparse
import difflib
import re
import shutil
import subprocess
import time
import sys

import llm_server

ROOT = Path(__file__).resolve().parent
BUILD_DIR = ROOT / "build"
SRC_DIR = ROOT / "src"
BACKUP_DIR = ROOT / "build_backup_src"
HDD = ROOT / "hdd.img"
DOCKER_IMAGE = "myos-toolchain"
QEMU = "qemu-system-x86_64"

SECTOR = 512
MAILBOX_LBA = 1
MAILBOX_SIZE = 8192
MAILBOX_HDR = 12
MAGIC_V2 = b"PATCHv02"
MAGIC_V1 = b"PATCHv01"
RESULT_LBA = 20
RESULT_SIZE = 2048
BOOT_GATE_LBA = 24
QEMU_EXIT_GATE_OK = (0x20 << 1) | 1    # 0x41 = 65
QEMU_EXIT_GATE_FAIL = (0x22 << 1) | 1  # 0x45 = 69
QEMU_EXIT_PATCH = (0x10 << 1) | 1   # qemu_exit(0x10) -> codigo de proceso 0x21

ALLOWED_EXT = {".c", ".h", ".s"}
FILE_RE = re.compile(
    r"^FILE:[ \t]*(\S+)[ \t]*\n(.*?)(?=^FILE:[ \t]*\S+[ \t]*\n|\Z)",
    re.S | re.M,
)
BLOCK_RE = re.compile(
    r"<<<<<<< SEARCH\n(.*?)\n=======\n(.*?)\n?>>>>>>> REPLACE",
    re.S,
)


# ---------------------------------------------------------------- utilidades

def run(command: list[str], title: str) -> int:
    print(f"\n{'=' * 72}\n{title}\n{'=' * 72}\n\n$ {' '.join(command)}\n")
    return subprocess.run(command, cwd=ROOT).returncode


def get_host_uid_gid() -> tuple[str, str]:
    uid = subprocess.check_output(["id", "-u"], text=True).strip()
    gid = subprocess.check_output(["id", "-g"], text=True).strip()
    return uid, gid


def docker_image_exists() -> bool:
    res = subprocess.run(["docker", "image", "inspect", DOCKER_IMAGE],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return res.returncode == 0


def git(*args: str) -> None:
    if not (ROOT / ".git").exists() or not shutil.which("git"):
        return
    subprocess.run(["git", *args], cwd=ROOT,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def git_snapshot(message: str) -> None:
    git("add", "src")
    git("commit", "-m", message)


def build(uid: str, gid: str) -> tuple[int, str]:
    cmd = ["docker", "run", "--rm", "--platform", "linux/amd64",
           "-u", f"{uid}:{gid}", "-v", f"{ROOT}:/myos", DOCKER_IMAGE, "make"]
    print(f"\n{'=' * 72}\nCOMPILANDO MYOS\n{'=' * 72}\n\n$ {' '.join(cmd)}\n")
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    print(out)
    return p.returncode, out


def summarize_errors(out: str, limit: int = 6) -> list[str]:
    hits = [l.strip() for l in out.splitlines()
            if re.search(r"error|undefined reference|\*\*\*", l, re.I)]
    return hits[:limit] or out.strip().splitlines()[-limit:]


# ---------------------------------------------------------------- buzon

def read_mailbox() -> tuple[str | None, str]:
    # Devuelve (estado, texto). estado: None (sin peticion), "v1", "bad", "ok".
    if not HDD.exists():
        return None, ""
    with open(HDD, "rb") as f:
        f.seek(MAILBOX_LBA * SECTOR)
        raw = f.read(MAILBOX_SIZE)
    magic = raw[:8]
    if magic == MAGIC_V1:
        return "v1", ""
    if magic != MAGIC_V2:
        return None, ""
    n = int.from_bytes(raw[8:12], "little")
    if n == 0 or n > MAILBOX_SIZE - MAILBOX_HDR:
        return "bad", ""
    return "ok", raw[MAILBOX_HDR:MAILBOX_HDR + n].decode("utf-8", errors="replace")


def clear_mailbox() -> None:
    with open(HDD, "rb+") as f:
        f.seek(MAILBOX_LBA * SECTOR)
        f.write(b"\x00" * MAILBOX_SIZE)


def write_result(status: str, detail: str = "") -> None:
    text = f"[PARCHE-RESULTADO] {status}"
    if detail:
        text += " | " + " | ".join(str(detail).splitlines())
    data = text.encode("ascii", "replace")[:RESULT_SIZE - 1]
    data += b"\x00" * (RESULT_SIZE - len(data))
    with open(HDD, "rb+") as f:
        f.seek(RESULT_LBA * SECTOR)
        f.write(data)
    print(f"[RESULTADO -> LBA {RESULT_LBA}] {text[:300]}")


# ---------------------------------------------------------------- parches

def arm_boot_gate() -> None:
    with open(HDD, "rb+") as f:
        f.seek(BOOT_GATE_LBA * SECTOR)
        data = b"GATE_TEST_REQ\x00" + b"\x00" * (SECTOR - 14)
        f.write(data)


def disarm_boot_gate() -> None:
    with open(HDD, "rb+") as f:
        f.seek(BOOT_GATE_LBA * SECTOR)
        f.write(b"\x00" * SECTOR)


PROTECTED_FILES = {
    'boot.s', 'idt.c', 'idt.h', 'io.h', 'srcfs.c', 'srcfs.h',
    'virtio_blk.c', 'virtio_blk.h', 'boot_gate.c', 'boot_gate.h'
}


def is_target_protected(path: Path) -> bool:
    if path.name in PROTECTED_FILES:
        return True
    prot_txt = ROOT / 'protected.txt'
    if prot_txt.exists():
        for line in prot_txt.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if line and not line.startswith('#') and Path(line).name == path.name:
                return True
    return False


def safe_target(name: str) -> Path | None:
    if "\x00" in name:
        return None
    p = (ROOT / name).resolve()          # resuelve '..' y symlinks
    src = SRC_DIR.resolve()
    if p.is_relative_to(src) and p.suffix.lower() in ALLOWED_EXT and p.is_file():
        if is_target_protected(p):
            print(f"[SEGURIDAD HOST] Modificacion denegada: '{p.name}' es un archivo protegido.")
            return None
        return p
    return None


def compute_patch(text: str):
    # Devuelve ({Path: (viejo, nuevo)}, None) o (None, mensaje_de_error).
    text = text.replace("\r\n", "\n")
    sections = FILE_RE.findall(text)
    if not sections:
        return None, "No hay secciones 'FILE: src/...'"

    contents: dict[Path, list[str]] = {}
    for name, body in sections:
        target = safe_target(name)
        if target is None:
            return None, f"FILE invalido '{name}' (solo src/*.c|h|s existentes)"
        if target not in contents:
            old = target.read_text(encoding="utf-8")
            contents[target] = [old, old]
        blocks = BLOCK_RE.findall(body)
        if not blocks:
            return None, f"Sin bloques SEARCH/REPLACE para {name}"
        for search, repl in blocks:
            cur = contents[target][1]
            n = cur.count(search) if search else 0
            if n != 1:
                return None, (f"SEARCH aparece {n} veces (debe ser 1) en {name}: "
                              f"{search[:60]!r}")
            contents[target][1] = cur.replace(search, repl, 1)

    changes = {p: (o, n) for p, (o, n) in contents.items() if o != n}
    if not changes:
        return None, "El parche no cambia nada"
    return changes, None


def show_diff(changes) -> None:
    for p, (old, new) in changes.items():
        rel = p.relative_to(ROOT)
        diff = difflib.unified_diff(old.splitlines(), new.splitlines(),
                                    f"a/{rel}", f"b/{rel}", lineterm="")
        print("\n".join(diff))


def apply_changes(changes) -> None:
    shutil.rmtree(BACKUP_DIR, ignore_errors=True)
    shutil.copytree(SRC_DIR, BACKUP_DIR)
    for p, (_, new) in changes.items():
        # Invalidar inodo en macOS VirtioFS borrando antes de escribir
        p.unlink(missing_ok=True)
        p.write_text(new, encoding="utf-8")
    # Pausa de sincronizacion para que la cache de Docker en macOS actualice st_size
    time.sleep(0.6)


def restore_src() -> None:
    shutil.copytree(BACKUP_DIR, SRC_DIR, dirs_exist_ok=True)
    time.sleep(0.4)


def handle_mailbox(approve: bool) -> bool | None:
    # None = no hubo peticion; True = parche aplicado; False = rechazado.
    state, text = read_mailbox()
    if state is None:
        return None
    clear_mailbox()

    print("\n" + "=" * 72)
    print("!!! PUENTE HOST-BRIDGE: LA IA PROPONE MODIFICAR EL CODIGO !!!")
    print("=" * 72)

    if state == "v1":
        write_result("REJECTED", "formato PATCHv01 obsoleto; usa FILE/SEARCH/REPLACE")
        return False
    if state == "bad":
        write_result("REJECTED", "longitud de parche invalida")
        return False

    changes, err = compute_patch(text)
    if err:
        print(f"[RECHAZADO] {err}")
        write_result("REJECTED", err)
        return False

    show_diff(changes)

    if approve:
        ans = input("\nAplicar este parche? [y/N] ").strip().lower()
        if ans != "y":
            write_result("REJECTED", "rechazado por el humano")
            return False

    git_snapshot("MYOS: snapshot antes de parche de la IA")
    apply_changes(changes)
    names = ", ".join(str(p.relative_to(ROOT)) for p in changes)
    print(f"\n[OK] Parche aplicado a: {names}")
    return True


# ---------------------------------------------------------------- main

# ---------------------------------------------------------------- SRCFS
# Empaqueta src/*.c|h|s en hdd.img para que la IA los lea con src_ls/src_cat/src_grep.
# Indice en LBA 256..263 (entradas de 64 bytes), datos desde LBA 264, todo < LBA 1024.

SRCFS_LBA = 256
SRCFS_INDEX_SECTORS = 8
SRCFS_ENTRY = 64
SRCFS_LIMIT_LBA = 1024


def pack_sources() -> None:
    files = sorted(p for p in SRC_DIR.iterdir()
                   if p.is_file() and p.suffix.lower() in ALLOWED_EXT)
    max_entries = SRCFS_INDEX_SECTORS * SECTOR // SRCFS_ENTRY - 1
    if len(files) > max_entries:
        print(f"[SRCFS] AVISO: {len(files)} archivos; solo caben {max_entries}")
        files = files[:max_entries]

    index = bytearray(SRCFS_INDEX_SECTORS * SECTOR)
    index[0:8] = b"SRCFSv01"
    index[8:12] = len(files).to_bytes(4, "little")

    lba = SRCFS_LBA + SRCFS_INDEX_SECTORS
    blobs = []
    for i, p in enumerate(files, 1):
        data = p.read_bytes()
        name = p.name.encode("utf-8")[:47]
        off = i * SRCFS_ENTRY
        index[off:off + len(name)] = name
        index[off + 48:off + 52] = lba.to_bytes(4, "little")
        index[off + 52:off + 56] = len(data).to_bytes(4, "little")
        blobs.append((lba, data))
        lba += (len(data) + SECTOR - 1) // SECTOR

    if lba > SRCFS_LIMIT_LBA:
        print(f"[SRCFS] ERROR: las fuentes ocupan hasta LBA {lba} (limite {SRCFS_LIMIT_LBA}); no se empaquetan")
        return

    with open(HDD, "rb+") as f:
        f.seek(SRCFS_LBA * SECTOR)
        f.write(index)
        for blba, data in blobs:
            f.seek(blba * SECTOR)
            f.write(data + b"\x00" * (-len(data) % SECTOR))
    print(f"[SRCFS] {len(files)} fuentes empaquetadas en LBA {SRCFS_LBA}..{lba - 1}")


def main() -> int:
    ap = argparse.ArgumentParser(description="MYOS runner (Singularity Loop v2)")
    ap.add_argument("--aprobar", action="store_true",
                    help="mostrar el diff y pedir confirmacion antes de aplicar")
    ap.add_argument("--max-ciclos", type=int, default=10,
                    help="maximo de ciclos compilar/arrancar (por defecto 10)")
    llm_server.add_arguments(ap)
    args = ap.parse_args()

    print("\n" + "=" * 72 + "\nMYOS DEVELOPMENT RUNNER - THE SINGULARITY LOOP v2\n" + "=" * 72 + "\n")
    if not shutil.which("docker") or not shutil.which(QEMU):
        print("ERROR: Faltan dependencias (Docker o QEMU).")
        return 1

    if not docker_image_exists():
        if run(["docker", "build", "--platform", "linux/amd64", "-t", DOCKER_IMAGE, "."],
               "CONSTRUYENDO TOOLCHAIN") != 0:
            return 1
    uid, gid = get_host_uid_gid()

    if not HDD.exists():
        print("Preparando disco fisico persistente (hdd.img de 16 MiB) fuera de build/ ...")
        with open(HDD, "wb") as f:
            f.write(b"\x00" * (16 * 1024 * 1024))

    llm_server.ensure_server(args)

    patched = False
    cycles = 0

    while True:
        cycles += 1
        if cycles > args.max_ciclos:
            print(f"Limite de {args.max_ciclos} ciclos alcanzado. Fin.")
            break

        if BUILD_DIR.exists():
            shutil.rmtree(BUILD_DIR)
        BUILD_DIR.mkdir(exist_ok=True)

        code, out = build(uid, gid)

        if code != 0:
            if patched:
                errs = " ; ".join(summarize_errors(out))
                print("\n[ROLLBACK] El parche de la IA no compila. Restaurando src/ ...")
                restore_src()
                write_result("COMPILE_FAILED (rollback aplicado)", errs)
                patched = False
                continue
            print("ERROR de compilacion. Rompiendo bucle.")
            return code

        if patched:
            print("\n" + "=" * 72)
            print("PUERTA DE ARRANQUE: PROBANDO KERNEL EN MODO CANARY")
            print("=" * 72)
            pack_sources()
            arm_boot_gate()

            canary_cmd = [
                QEMU, "-machine", "pc", "-m", "256M", "-no-reboot",
                "-cdrom", "build/myos.iso", "-netdev", "user,id=net0",
                "-device", "virtio-net-pci,netdev=net0",
                "-drive", "file=hdd.img,format=raw,if=virtio",
                "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                "-serial", "stdio", "-display", "none",
            ]
            print("$ " + " ".join(canary_cmd) + "\n")
            try:
                canary_rc = subprocess.run(canary_cmd, cwd=ROOT, timeout=45).returncode
            except subprocess.TimeoutExpired:
                print("\n[BOOT-GATE TIMEOUT] El kernel se colgo o tardo demasiado (>45s).")
                canary_rc = -1

            disarm_boot_gate()

            if canary_rc == QEMU_EXIT_GATE_OK:
                print("\n[BOOT-GATE OK] El nuevo kernel paso el health-check. Parche validado.")
                write_result("BOOT_OK", "kernel nuevo verificado y operativo")
                git_snapshot("MYOS: parche verificado por puerta de arranque")
                patched = False
            else:
                print(f"\n[BOOT-GATE FAIL] El kernel fallo en el arranque (rc={canary_rc}).")
                print("[ROLLBACK] Restaurando src/ a la version sana anterior...")
                restore_src()
                write_result("BOOT_FAILED (rollback aplicado)", f"canary_rc={canary_rc}")
                patched = False
                continue

        pack_sources()

        qemu_command = [
            QEMU, "-machine", "pc", "-m", "256M", "-no-reboot", "-d", "int", "-D", "build/qemu.log",
            "-cdrom", "build/myos.iso", "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
            "-drive", "file=hdd.img,format=raw,if=virtio",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-object", "filter-dump,id=f1,netdev=net0,file=build/net.pcap",
            "-serial", "stdio",
        ]

        print("\n" + "=" * 72 + "\nARRANCANDO MYOS BARE-METAL\n" + "=" * 72 + "\n")
        try:
            rc = subprocess.run(qemu_command, cwd=ROOT).returncode
        except KeyboardInterrupt:
            print("\nQEMU detenido por el usuario.")
            break

        outcome = handle_mailbox(args.aprobar)

        if outcome is None:
            if rc == QEMU_EXIT_PATCH:
                print("AVISO: el kernel pidio un parche (exit 0x21) pero el buzon esta vacio o corrupto.")
            print(">>> QEMU cerrado sin peticion de auto-modificacion. Fin del script.")
            break

        print(">>> Reiniciando el ciclo de compilacion...")
        patched = outcome

    return 0


if __name__ == "__main__":
    sys.exit(main())
