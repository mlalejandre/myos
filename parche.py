#!/usr/bin/env python3
"""parche.py (2b) - corrige la alineacion de pila en thread_trampoline_asm."""
from pathlib import Path
import shutil, subprocess, time, sys

ROOT = Path(__file__).resolve().parent
BACKUP = ROOT / ".parche_backup"
IMG = "myos-toolchain"

EDITS = {
"src/switch.s": [(
"""    subq $8, %rsp
    call thread_trampoline_c
    addq $8, %rsp
    call thread_exit""",
"""    /* FIX: %rsp ya es multiplo de 16 aqui; 'call' empuja el retorno.
       Restar 8 dejaba toda la pila del hilo desalineada (#GP con SSE). */
    call thread_trampoline_c
    call thread_exit""")],
}


def main() -> int:
    new = {}
    for rel, edits in EDITS.items():
        text = (ROOT / rel).read_text(encoding="utf-8")
        for old, rep in edits:
            n = text.count(old)
            if n != 1:
                print(f"[ABORTO] {rel}: SEARCH aparece {n} veces: {old[:60]!r}")
                return 1
            text = text.replace(old, rep, 1)
        new[rel] = text

    shutil.rmtree(BACKUP, ignore_errors=True)
    for rel in new:
        dst = BACKUP / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, dst)

    for rel, text in new.items():
        p = ROOT / rel
        p.unlink(missing_ok=True)
        p.write_text(text, encoding="utf-8")
        print(f"[OK] {rel}")
    time.sleep(0.8)

    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64",
                        "-v", f"{ROOT}:/myos", "-w", "/myos", IMG, "make"],
                       capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        print("COMPILACION FALLIDA:\n" + out)
        return 1
    warns = [l for l in out.splitlines() if "warning" in l.lower()]
    print(f"COMPILACION OK ({len(warns)} warnings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())