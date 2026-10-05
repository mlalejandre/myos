#!/usr/bin/env python3
"""
parches.py - Aplicador de parches para SOMA (lote 1: arreglos pequenos).

Uso (desde la raiz del proyecto, donde esta ejecutar.py):

    python3 ejecutar.py --snapshot pre_lote1     # recomendado ANTES
    python3 parches.py --dry-run                 # ver que haria, sin tocar nada
    python3 parches.py                           # aplicar todo el lote 1
    python3 parches.py --solo P3                 # aplicar un unico parche
    python3 parches.py --tests                   # solo mostrar las pruebas
    python3 parches.py --revertir                # restaurar el ultimo backup

Cada parche:
  - SEARCH debe aparecer EXACTAMENTE una vez (si no, se salta y se avisa).
  - Si ya esta aplicado, se detecta y se salta (idempotente).
  - Se hace backup en .parches_backup/<fecha>/ antes de escribir.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BACKUP_ROOT = ROOT / ".parches_backup"

# ----------------------------------------------------------------------------
# DEFINICION DE PARCHES
#   file    : ruta relativa a la raiz
#   search  : texto exacto (una sola aparicion)
#   replace : texto nuevo
#   append  : (alternativa) lista de lineas a anadir al final si faltan
#   test    : que debes comprobar tras arrancar con ejecutar.py
# ----------------------------------------------------------------------------

PATCHES = [
    {
        "id": "P1",
        "titulo": "net_big_lock: inicializar y registrar el mutex de red",
        "file": "src/net.c",
        "search": "void net_lock(void)   { kmutex_lock(&net_big_lock); }",
        "replace": (
            "static int net_lock_ready;\n"
            "\n"
            "void net_lock(void)\n"
            "{\n"
            "    if (!net_lock_ready) {\n"
            "        kmutex_init(&net_big_lock, \"net_lock\");\n"
            "        net_lock_ready = 1;\n"
            "    }\n"
            "    kmutex_lock(&net_big_lock);\n"
            "}"
        ),
        "test": [
            "Arranca SOMA y espera al prompt.",
            "soma> spawn curl example.com        (anota el TID, p.ej. 4)",
            "soma> kill <TID>                    (mientras el curl sigue en marcha)",
            "  -> debe aparecer '[MUTEX RECOVERY] Mutex 'net_lock' liberado...'",
            "soma> ping 10.0.2.2                 -> debe responder 'Reply from'",
            "  (Antes del parche: el ping se quedaba colgado para siempre.)",
            "NOTA: si matas un curl a mitad, una conexion TCP puede quedar",
            "      'used' (fuga conocida; la arreglamos en un lote posterior).",
            "      No repitas el kill mas de 1 vez en esta prueba.",
        ],
    },
    {
        "id": "P2",
        "titulo": "Panic handler: desmutear la consola antes de imprimir",
        "file": "src/idt.c",
        "search": "    uint64_t cr2 = read_cr2();\n    uint64_t cr0 = read_cr0();",
        "replace": (
            "    console_muted = 0; /* un panic dentro de una redireccion '>' debe ser visible */\n"
            "    uint64_t cr2 = read_cr2();\n"
            "    uint64_t cr0 = read_cr0();"
        ),
        "test": [
            "soma> panic div      -> debe imprimir el KERNEL PANIC completo como siempre",
            "  (Es un parche defensivo: no hay forma facil de provocar un panic",
            "   DENTRO de una redireccion. Solo comprobamos que no hay regresion",
            "   y que compila. Cierra QEMU con Ctrl+C despues.)",
        ],
    },
    {
        "id": "P3a",
        "titulo": "SECTOR_USER_MIN: 2048 -> 2056 (2048..2055 = scratch del sistema)",
        "file": "src/srcfs.h",
        "search": "#define SECTOR_USER_MIN 2048",
        "replace": (
            "/* LBA 2048..2055: scratch del sistema (test_suite y canary). */\n"
            "#define SECTOR_USER_MIN 2056"
        ),
        "test": [
            "soma> sector_write 2048 hola   -> debe dar 'sector protegido'",
            "soma> sector_write 2056 hola   -> debe escribir con exito",
            "soma> sector_read 2056         -> debe mostrar 'hola'",
            "soma> test_suite               -> debe seguir en 'INTEGRIDAD TOTAL'",
        ],
    },
    {
        "id": "P3b",
        "titulo": "sector_write: proteger tambien el checkpoint (LBA 4095..4400)",
        "file": "src/kernel.c",
        "search": "        if (sec < SECTOR_USER_MIN) {",
        "replace": "        if (sec < SECTOR_USER_MIN || (sec >= 4095 && sec <= 4400)) {",
        "test": [
            "soma> sector_write 4100 x      -> debe dar 'sector protegido'",
            "soma> sector_write 5000 x      -> debe escribir con exito",
            "soma> checkpoint               -> debe guardar OK",
            "soma> rollback                 -> debe restaurar OK",
        ],
    },
    {
        "id": "P3c",
        "titulo": "Mensaje de error de sector_write acorde a los nuevos rangos",
        "file": "src/kernel.c",
        "search": '"Error: sector protegido (LBA < 2048 reservado). Usa LBA >= 2048."',
        "replace": '"Error: sector protegido (LBA < 2056 o 4095-4400 reservados). Usa LBA >= 2056."',
        "test": ["(Se comprueba junto con P3a/P3b: lee el texto del error.)"],
    },
    {
        "id": "P3d",
        "titulo": "srcfs.h: unificar los dos comentarios contradictorios del mapa de disco",
        "file": "src/srcfs.h",
        "search": (
            " *   0 .. 1023   reservado al sistema (buzon LBA 1-16, resultado LBA 20-23,\n"
            " *               indice y fuentes SRCFS en LBA 256-1023)\n"
            " *   >= 1024     libre para el usuario / la IA (sector_write)\n"
            " */\n"
            "/*\n"
            " * Mapa del disco (sectores de 512 bytes):\n"
        ),
        "replace": (
            " *   (ver el mapa unificado a continuacion)\n"
            " */\n"
            "/*\n"
            " * Mapa del disco (sectores de 512 bytes):\n"
        ),
        "test": ["Solo comentarios: basta con que compile."],
    },
    {
        "id": "P3e",
        "titulo": "srcfs.h: documentar scratch y checkpoint en el mapa",
        "file": "src/srcfs.h",
        "search": " *   >= 2048     libre para el usuario / la IA (sector_write)",
        "replace": (
            " *   2048..2055  scratch del sistema (test_suite / canary)\n"
            " *   2056..4094  libre para el usuario / la IA (sector_write)\n"
            " *   4095..4400  checkpoint del RamFS (protegido)\n"
            " *   >= 4401     libre para el usuario / la IA (sector_write)"
        ),
        "test": ["Solo comentarios: basta con que compile."],
    },
    {
        "id": "P4",
        "titulo": ".gitignore: snapshots, checkpoints y backups de parches",
        "file": ".gitignore",
        "append": [
            "",
            "# Snapshots y checkpoints de disco (16 MiB cada uno)",
            "snapshots/",
            "hdd.img.checkpoint*",
            "*.checkpoint_*",
            ".parches_backup/",
        ],
        "test": [
            "git status      -> 'snapshots/' y 'hdd.img.checkpoint_bootgate'",
            "                   ya no deben aparecer como sin seguimiento.",
            "(Si hdd.img.checkpoint_bootgate ya estaba commiteado:",
            " git rm --cached hdd.img.checkpoint_bootgate)",
        ],
    },
    {
        "id": "P5",
        "titulo": "ejecutar.py: QEMU sin '-d int' (evita GB de qemu.log)",
        "file": "ejecutar.py",
        "search": '"-m", "256M", "-no-reboot", "-d", "int", "-D", "build/qemu.log",',
        "replace": '"-m", "256M", "-no-reboot", "-d", "guest_errors,cpu_reset", "-D", "build/qemu.log",',
        "test": [
            "python3 ejecutar.py, deja correr ~1 minuto, sal y mira:",
            "  ls -lh build/qemu.log     -> debe ser pequeno (KB, no MB)",
            "Si algun dia necesitas depurar excepciones, vuelve a poner '-d int'",
            "temporalmente.",
        ],
    },
    {
        "id": "P6",
        "titulo": "console.h: quitar el '/* hpatch ok */' duplicado",
        "file": "src/console.h",
        "search": "#define MYOS_CONSOLE_H\n/* hpatch ok */\n/* hpatch ok */\n",
        "replace": "#define MYOS_CONSOLE_H\n",
        "test": [
            "Solo limpieza: basta con que compile.",
            "(Ojo: 'hpatch ok' vuelve a anadir un comentario cuando lo uses.)",
        ],
    },
]


# ----------------------------------------------------------------------------
# UTILIDADES
# ----------------------------------------------------------------------------

def read_text(path: Path) -> tuple[str, str]:
    """Devuelve (texto normalizado a \\n, fin de linea original)."""
    raw = path.read_bytes().decode("utf-8")
    eol = "\r\n" if "\r\n" in raw else "\n"
    return raw.replace("\r\n", "\n"), eol


def write_text(path: Path, text: str, eol: str) -> None:
    if eol == "\r\n":
        text = text.replace("\n", "\r\n")
    path.write_bytes(text.encode("utf-8"))


def plan_patch(p: dict, text: str) -> tuple[str, str | None]:
    """Devuelve (estado, nuevo_texto). estado: OK | YA | ERROR:<motivo>."""
    if "append" in p:
        existing = set(l.strip() for l in text.splitlines())
        missing = [l for l in p["append"] if l.strip() and l.strip() not in existing]
        if not missing:
            return "YA", None
        new = text
        if not new.endswith("\n"):
            new += "\n"
        # conserva las lineas vacias/comentarios solo si hay algo nuevo que anadir
        new += "\n".join(p["append"]) + "\n"
        return "OK", new

    search, replace = p["search"], p["replace"]
    if replace.strip() and replace in text:
        return "YA", None
    n = text.count(search)
    if n == 1:
        return "OK", text.replace(search, replace, 1)
    if n == 0:
        if replace in text:
            return "YA", None
        return "ERROR:SEARCH no encontrado (el archivo difiere de lo esperado)", None
    return f"ERROR:SEARCH aparece {n} veces (debe ser 1)", None


def print_tests(patches) -> None:
    print("\n" + "=" * 72)
    print("PRUEBAS A REALIZAR (arranca con: python3 ejecutar.py)")
    print("=" * 72)
    for p in patches:
        print(f"\n[{p['id']}] {p['titulo']}")
        for line in p["test"]:
            print(f"    {line}")
    print()


def latest_backup() -> Path | None:
    if not BACKUP_ROOT.exists():
        return None
    dirs = sorted(d for d in BACKUP_ROOT.iterdir() if d.is_dir())
    return dirs[-1] if dirs else None


def revert() -> int:
    b = latest_backup()
    if not b:
        print("No hay backups en .parches_backup/")
        return 1
    count = 0
    for f in b.rglob("*"):
        if f.is_file():
            rel = f.relative_to(b)
            dest = ROOT / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)
            print(f"  restaurado {rel}")
            count += 1
    print(f"\n[OK] {count} archivo(s) restaurados desde {b.name}")
    return 0


# ----------------------------------------------------------------------------
# MAIN
# ----------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="Aplicador de parches SOMA (lote 1)")
    ap.add_argument("--dry-run", action="store_true", help="no escribe nada")
    ap.add_argument("--solo", metavar="ID", help="aplica solo un parche (ej: P3 aplica P3a..P3e)")
    ap.add_argument("--tests", action="store_true", help="solo muestra las pruebas")
    ap.add_argument("--revertir", action="store_true", help="restaura el ultimo backup")
    args = ap.parse_args()

    if not (ROOT / "ejecutar.py").exists() or not (ROOT / "src").is_dir():
        print("ERROR: ejecuta parches.py desde la raiz del proyecto (donde esta ejecutar.py).")
        return 1

    if args.revertir:
        return revert()

    selected = PATCHES
    if args.solo:
        selected = [p for p in PATCHES if p["id"] == args.solo or p["id"].startswith(args.solo)]
        if not selected:
            print(f"No existe el parche '{args.solo}'. Disponibles: "
                  + ", ".join(p["id"] for p in PATCHES))
            return 1

    if args.tests:
        print_tests(selected)
        return 0

    stamp = time.strftime("%Y%m%d_%H%M%S")
    backup_dir = BACKUP_ROOT / stamp

    # Trabajamos en memoria por archivo para que varios parches sobre el mismo
    # archivo se acumulen correctamente.
    buffers: dict[Path, tuple[str, str]] = {}   # path -> (texto_actual, eol)
    original: dict[Path, bytes] = {}
    results: list[tuple[str, str, str]] = []    # (id, estado, detalle)
    applied_patches = []

    print("\n" + "=" * 72)
    print("PARCHES SOMA - LOTE 1" + ("  [DRY-RUN]" if args.dry_run else ""))
    print("=" * 72)

    for p in selected:
        path = ROOT / p["file"]
        if path not in buffers:
            if not path.exists():
                results.append((p["id"], "ERROR", f"{p['file']} no existe"))
                print(f"[{p['id']}] ERROR  {p['file']} no existe")
                continue
            original[path] = path.read_bytes()
            buffers[path] = read_text(path)

        text, eol = buffers[path]
        status, new_text = plan_patch(p, text)

        if status == "OK":
            buffers[path] = (new_text, eol)
            results.append((p["id"], "APLICADO", p["file"]))
            applied_patches.append(p)
            print(f"[{p['id']}] APLICADO     {p['file']}  - {p['titulo']}")
        elif status == "YA":
            results.append((p["id"], "YA APLICADO", p["file"]))
            print(f"[{p['id']}] ya aplicado  {p['file']}  - {p['titulo']}")
        else:
            motivo = status.split(":", 1)[1]
            results.append((p["id"], "ERROR", motivo))
            print(f"[{p['id']}] ERROR        {p['file']}: {motivo}")

    if not args.dry_run and applied_patches:
        for path, (text, eol) in buffers.items():
            if text != read_text(path)[0]:
                rel = path.relative_to(ROOT)
                (backup_dir / rel).parent.mkdir(parents=True, exist_ok=True)
                (backup_dir / rel).write_bytes(original[path])
                write_text(path, text, eol)
        print(f"\n[OK] Backup en {backup_dir.relative_to(ROOT)}/  (revertir: python3 parches.py --revertir)")

    n_ok = sum(1 for _, s, _ in results if s == "APLICADO")
    n_ya = sum(1 for _, s, _ in results if s == "YA APLICADO")
    n_err = sum(1 for _, s, _ in results if s == "ERROR")
    print(f"\nResumen: {n_ok} aplicados, {n_ya} ya aplicados, {n_err} con error.")

    if n_err:
        print("\nHay parches con error: NO se tocaron esos puntos. Pegame la salida de este")
        print("script (y si hace falta el trozo de archivo afectado) y lo ajusto.")

    if applied_patches and not args.dry_run:
        print_tests(applied_patches)
        print("Siguiente paso: python3 ejecutar.py  (compila y arranca), haz las pruebas")
        print("y pegame el resultado (o lo que haya fallado).")
    elif args.dry_run:
        print("\n(dry-run: no se ha escrito nada)")

    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())