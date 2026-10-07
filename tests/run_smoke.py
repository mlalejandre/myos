#!/usr/bin/env python3
"""
tests/run_smoke.py - Runner interactivo de Smoke Tests en SOMA (QEMU).
Ejecuta secuencialmente los comandos de tests/smoke.json comunicándose sobre COM1.
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SMOKE_JSON = ROOT / "tests" / "smoke.json"
ISO = ROOT / "build" / "soma.iso"
HDD = ROOT / "hdd.img"

def reader_loop(pipe, out_q):
    while True:
        try:
            chunk = pipe.read(1)
            if not chunk:
                break
            out_q.put(chunk)
        except Exception:
            break

def main():
    if not SMOKE_JSON.exists():
        print(f"ERROR: No se encontro {SMOKE_JSON}")
        return 1

    if not ISO.exists():
        print("ERROR: Compila primero la imagen ISO con 'docker run ... make'.")
        return 1

    with open(SMOKE_JSON, "r", encoding="utf-8") as f:
        tests = json.load(f)

    print("=" * 72)
    print("SOMA SMOKE TEST RUNNER - SUITE DE REGRESION BARE-METAL")
    print(f"Total de aserciones a validar: {len(tests)}")
    print("=" * 72)

    qemu_cmd = [
        "qemu-system-x86_64",
        "-machine", "pc",
        "-m", "256M",
        "-no-reboot",
        "-cdrom", str(ISO),
        "-netdev", "user,id=net0",
        "-device", "virtio-net-pci,netdev=net0",
        "-drive", f"file={HDD},format=raw,if=virtio",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-serial", "stdio",
        "-display", "none",
    ]

    proc = subprocess.Popen(
        qemu_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=ROOT,
        bufsize=0
    )

    out_q = queue.Queue()
    t = threading.Thread(target=reader_loop, args=(proc.stdout, out_q), daemon=True)
    t.start()

    # 1. Esperar arranque hasta prompt
    buf = bytearray()
    t_start = time.time()
    booted = False
    print("Iniciando QEMU y esperando banner de SOMA... ", end="", flush=True)
    while time.time() - t_start < 25:
        try:
            ch = out_q.get(timeout=0.1)
            buf += ch
            if b"soma>" in buf:
                booted = True
                break
        except queue.Empty:
            pass

    if not booted:
        print("FALLO (Timeout esperando soma>)")
        proc.kill()
        return 1
    print("OK (Listo)\n")

    passed = 0
    failed = 0

    for idx, test in enumerate(tests, 1):
        cmd = test["cmd"]
        expect = test["expect"]
        print(f"[{idx:2d}/{len(tests)}] Test '{cmd}' -> esperando '{expect}' ... ", end="", flush=True)

        # Vaciar cola previa
        while not out_q.empty():
            out_q.get_nowait()

        proc.stdin.write(cmd.encode("utf-8") + b"\n")
        proc.stdin.flush()

        resp = bytearray()
        t_cmd = time.time()
        ok = False
        while time.time() - t_cmd < 6:
            try:
                ch = out_q.get(timeout=0.1)
                resp += ch
                if expect.encode("utf-8") in resp:
                    ok = True
                if b"soma>" in resp and ok:
                    break
            except queue.Empty:
                pass

        if ok:
            print("OK")
            passed += 1
        else:
            print("FALLO")
            failed += 1

    try:
        proc.stdin.write(b"poweroff\n")
        proc.stdin.flush()
        proc.wait(timeout=3)
    except Exception:
        proc.kill()

    print("=" * 72)
    print(f"RESUMEN: {passed} PASADOS, {failed} FALLIDOS (Total: {len(tests)})")
    print("=" * 72)

    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
