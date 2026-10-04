#!/usr/bin/env python3
"""
diagnostico.py - Inspección microscópica del error de preprocesador en mem.h y fs.h
"""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MEM_H = ROOT / "src/mem.h"
DOCKER_IMAGE = "myos-toolchain"


def run_docker(cmd: list[str]) -> tuple[int, str]:
    full_cmd = [
        "docker", "run", "--rm", "--platform", "linux/amd64",
        "-v", f"{ROOT}:/myos", "-w", "/myos",
        DOCKER_IMAGE
    ] + cmd
    p = subprocess.run(full_cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main():
    print("\n" + "=" * 60)
    print("DIAGNÓSTICO DEL PREPROCESADOR (mem.h -> fs.h)")
    print("=" * 60 + "\n")

    if not MEM_H.exists():
        print("ERROR: No existe src/mem.h")
        return 1

    original = MEM_H.read_text(encoding="utf-8")
    print(f"[1] Estado actual de src/mem.h ({len(original)} bytes):")
    print("--- INICIO REPR ---")
    print(repr(original[:120]) + " ... " + repr(original[-60:]))
    print("--- FIN REPR ---\n")

    # Prueba A: Compilar el kernel tal como está ahora
    code_a, out_a = run_docker(["gcc", "-m64", "-march=x86-64", "-ffreestanding", "-c", "src/kernel.c", "-o", "build/kernel.o"])
    print(f"[2] Compilación base de kernel.c: {'OK' if code_a == 0 else 'FALLÓ'}")
    if code_a != 0:
        print(out_a)

    # Prueba B: Aplicar el parche que propone la IA
    patched = "/* nota de la IA */\n" + original
    MEM_H.write_text(patched, encoding="utf-8")
    print(f"[3] Aplicando comentario al inicio ({len(patched)} bytes)...")

    # Ejecutar compilación con el parche
    code_b, out_b = run_docker(["gcc", "-m64", "-march=x86-64", "-ffreestanding", "-c", "src/kernel.c", "-o", "build/kernel.o"])
    print(f"[4] Compilación con parche: {'OK' if code_b == 0 else 'FALLÓ'}")
    if code_b != 0:
        print("\n--- ERROR DE GCC ---")
        print(out_b.strip())
        print("---------------------\n")

        # Si falla, obtener salida del preprocesador (-E) alrededor de mem.h y fs.h
        print("[5] Analizando salida de gcc -E:")
        _, cpp_out = run_docker(["gcc", "-m64", "-march=x86-64", "-ffreestanding", "-E", "src/kernel.c"])
        lines = cpp_out.splitlines()
        # Buscar dónde aparece mem.h y fs.h en la salida preprocesada
        hits = [i for i, l in enumerate(lines) if "mem.h" in l or "fs.h" in l]
        if hits:
            start = max(0, hits[0] - 5)
            end = min(len(lines), hits[-1] + 25)
            print("\n".join(lines[start:end]))
        else:
            print("No se encontraron referencias en la salida -E.")

    # Restaurar original
    MEM_H.write_text(original, encoding="utf-8")
    print("\n[6] src/mem.h restaurado a su estado original.")
    return 0


if __name__ == "__main__":
    main()