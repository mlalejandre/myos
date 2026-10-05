#!/usr/bin/env python3
"""parche.py (3) - cerrojos VFS, pilas 8 pag, general-regs-only, timeouts curl, marco inicial."""
from pathlib import Path
import shutil, subprocess, time, sys

ROOT = Path(__file__).resolve().parent
BACKUP = ROOT / ".parche_backup"
IMG = "myos-toolchain"

EDITS = {
"src/fs.c": [
("int vfs_sync(void)\n{\n    struct fs_superblock sb;",
 "static int vfs_sync_unlocked(void)\n{\n    struct fs_superblock sb;"),
("void vfs_format(void)\n{",
 "static void vfs_format_unlocked(void)\n{"),
("int vfs_create(const char *name, const char *initial_data)\n{",
 "static int vfs_create_unlocked(const char *name, const char *initial_data)\n{"),
("int vfs_read(const char *name, char *buf_out, uint32_t max_len)\n{",
 "static int vfs_read_unlocked(const char *name, char *buf_out, uint32_t max_len)\n{"),
("int vfs_delete(const char *name)\n{",
 "static int vfs_delete_unlocked(const char *name)\n{"),
("void vfs_list(void)\n{",
 "static void vfs_list_unlocked(void)\n{"),
("int vfs_format_list(char *out_buf, uint32_t max)\n{",
 "static int vfs_format_list_unlocked(char *out_buf, uint32_t max)\n{"),
("    out_buf[pos] = '\\0';\n    return (int)pos;\n}",
"""    out_buf[pos] = '\\0';
    return (int)pos;
}


/* ---- Envoltorios con cerrojo (mutex recursivo) ---- */
int vfs_sync(void)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_sync_unlocked();
    kmutex_unlock(&vfs_mutex);
    return r;
}

void vfs_format(void)
{
    kmutex_lock(&vfs_mutex);
    vfs_format_unlocked();
    kmutex_unlock(&vfs_mutex);
}

int vfs_create(const char *name, const char *initial_data)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_create_unlocked(name, initial_data);
    kmutex_unlock(&vfs_mutex);
    return r;
}

int vfs_read(const char *name, char *buf_out, uint32_t max_len)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_read_unlocked(name, buf_out, max_len);
    kmutex_unlock(&vfs_mutex);
    return r;
}

int vfs_delete(const char *name)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_delete_unlocked(name);
    kmutex_unlock(&vfs_mutex);
    return r;
}

void vfs_list(void)
{
    kmutex_lock(&vfs_mutex);
    vfs_list_unlocked();
    kmutex_unlock(&vfs_mutex);
}

int vfs_format_list(char *out_buf, uint32_t max)
{
    kmutex_lock(&vfs_mutex);
    int r = vfs_format_list_unlocked(out_buf, max);
    kmutex_unlock(&vfs_mutex);
    return r;
}"""),
],

"src/thread.c": [
("#define THREAD_STACK_PAGES 3", "#define THREAD_STACK_PAGES 8"),
("""    *(--sp) = (uint64_t)thread_trampoline_asm;
    *(--sp) = 0x202ULL;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
""",
"""    /* Orden inverso a switch_context: rip, rbp, rbx, r12-r15, rflags (el ultimo se pop primero) */
    *(--sp) = (uint64_t)thread_trampoline_asm;
    *(--sp) = 0;    /* rbp */
    *(--sp) = 0;    /* rbx */
    *(--sp) = 0;    /* r12 */
    *(--sp) = 0;    /* r13 */
    *(--sp) = 0;    /* r14 */
    *(--sp) = 0;    /* r15 */
    *(--sp) = 0x202ULL;   /* rflags (IF=1) */
"""),
],

"Makefile": [
("-mno-red-zone\n\nOBJS",
 "-mno-red-zone \\\n\t-mgeneral-regs-only\n\nOBJS"),
],

"src/http.c": [
("uint32_t connect_timeout = timeout_ms > 8000 ? 8000 : timeout_ms;",
 "uint32_t connect_timeout = timeout_ms > 30000 ? 30000 : timeout_ms;"),
],

"src/kernel.c": [
("http_get_host(tip, port, host_hdr, path, http_buf, sizeof(http_buf), &resp, 10000);",
 "http_get_host(tip, port, host_hdr, path, http_buf, sizeof(http_buf), &resp, 40000);"),
],
}


def main() -> int:
    new = {}
    for rel, edits in EDITS.items():
        text = (ROOT / rel).read_text(encoding="utf-8")
        for old, rep in edits:
            n = text.count(old)
            if n != 1:
                print(f"[ABORTO] {rel}: SEARCH aparece {n} veces: {old[:70]!r}")
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
        p.unlink(missing_ok=True)          # invalida cache VirtioFS
        p.write_text(text, encoding="utf-8")
        print(f"[OK] {rel}")
    time.sleep(0.8)

    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64",
                        "-v", f"{ROOT}:/myos", "-w", "/myos", IMG, "make"],
                       capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        print("COMPILACION FALLIDA:\n" + out)
        print("Restaurar: cp -R .parche_backup/src/. src/ && cp .parche_backup/Makefile Makefile")
        return 1
    warns = [l for l in out.splitlines() if "warning" in l.lower()]
    print(f"COMPILACION OK ({len(warns)} warnings)")
    for w in warns:
        print("  ", w)
    return 0


if __name__ == "__main__":
    sys.exit(main())