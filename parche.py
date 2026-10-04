#!/usr/bin/env python3
"""
parche.py - Extracción y blindaje de la Puerta de Arranque (boot_gate.c)
1. Crea src/boot_gate.h y src/boot_gate.c desacoplando el LLM de los tests de hardware.
2. Integra boot_gate.o en Makefile.
3. Limpia src/kernel.c delegando en boot_gate.
4. Instrumenta ejecutar.py para proteger src/boot_gate.* contra adulteraciones de la IA.
"""

from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
SRC_DIR = ROOT / "src"
KERNEL_C = SRC_DIR / "kernel.c"
BOOT_GATE_H = SRC_DIR / "boot_gate.h"
BOOT_GATE_C = SRC_DIR / "boot_gate.c"
MAKEFILE = ROOT / "Makefile"
EJECUTAR = ROOT / "ejecutar.py"
DOCKER_IMAGE = "myos-toolchain"


def run_docker_check() -> bool:
    if not shutil.which("docker"):
        print("[AVISO] Docker no encontrado; omitiendo verificación de compilación.")
        return True

    cmd = [
        "docker", "run", "--rm", "--platform", "linux/amd64",
        "-v", f"{ROOT}:/myos", "-w", "/myos",
        DOCKER_IMAGE, "make"
    ]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("\n--- ERROR DE COMPILACIÓN GCC TRAS EL PARCHE ---")
        print(p.stderr or p.stdout)
        return False
    return True


BOOT_GATE_H_CONTENT = """#ifndef MYOS_BOOT_GATE_H
#define MYOS_BOOT_GATE_H

#include <stdint.h>

/* Ejecuta la suite de pruebas bare-metal (0 = OK, -1 = fallo critico) */
int boot_gate_run_test_suite(char *out_buf, uint32_t max_out);

/* Verificacion del modo canary en el arranque */
void boot_gate_check(void);

#endif
"""

BOOT_GATE_C_CONTENT = """#include <stdint.h>

#include "boot_gate.h"
#include "console.h"
#include "io.h"
#include "mem.h"
#include "net.h"
#include "ip.h"
#include "fs.h"
#include "virtio_blk.h"
#include "llm.h"
#include "idt.h"

#define BOOT_GATE_LBA 24

static void bg_puts(char *out, uint32_t max, uint32_t *pos, const char *s)
{
    if (!out || max == 0) return;
    while (*s && *pos + 1 < max) {
        out[(*pos)++] = *s++;
    }
    out[*pos] = '\\0';
}

/* ---- Suite de Auto-Test de No Regresion (Bare-Metal) ------------- */
int boot_gate_run_test_suite(char *out_buf, uint32_t max_out)
{
    uint32_t pos = 0;
    int failed = 0;

    kprint("\\n============================================================\\n");
    kprint("MYOS BARE-METAL TEST SUITE (Verificacion de No Regresion)\\n");
    kprint("============================================================\\n");
    bg_puts(out_buf, max_out, &pos, "[SUITE DE AUTO-TEST MYOS]:\\n");

    /* 1. Test de Heap y Alineacion 16-bytes (kmalloc/kfree) */
    kprint("[TEST 1/5] Heap Allocator & Alineacion 16-bytes... ");
    void *p1 = kmalloc(64);
    void *p2 = kmalloc(256);
    void *p3 = kmalloc(1024);
    if (!p1 || !p2 || !p3 ||
        ((uintptr_t)p1 & 0x0F) != 0 ||
        ((uintptr_t)p2 & 0x0F) != 0 ||
        ((uintptr_t)p3 & 0x0F) != 0) {
        kprint("FALLO (alloc o desalineacion)\\n");
        bg_puts(out_buf, max_out, &pos, "- Heap kmalloc: FALLO\\n");
        failed++;
    } else {
        memset(p1, 0xAA, 64);
        memset(p2, 0x55, 256);
        memset(p3, 0x33, 1024);
        uint8_t *b1 = (uint8_t *)p1;
        uint8_t *b2 = (uint8_t *)p2;
        int canaries_ok = (b1[0] == 0xAA && b1[63] == 0xAA && b2[0] == 0x55 && b2[255] == 0x55);
        kfree(p2);
        kfree(p1);
        kfree(p3);
        if (canaries_ok) {
            kprint("OK (16-byte align & canaries OK)\\n");
            bg_puts(out_buf, max_out, &pos, "- Heap kmalloc: OK\\n");
        } else {
            kprint("FALLO (corrupcion de canario)\\n");
            bg_puts(out_buf, max_out, &pos, "- Heap kmalloc: FALLO CANARIO\\n");
            failed++;
        }
    }

    /* 2. Test de Disco VirtIO-BLK (Lectura/Escritura LBA 2048) */
    kprint("[TEST 2/5] Disco VirtIO-BLK (Lectura/Escritura LBA 2048)... ");
    static uint8_t sec_w[512] __attribute__((aligned(16)));
    static uint8_t sec_r[512] __attribute__((aligned(16)));
    for (int i = 0; i < 512; ++i) sec_w[i] = (uint8_t)(i ^ 0x5A);
    memcpy(sec_w, "MYOS_SELFTEST_INTEGRITY_SECTOR_2048", 35);
    if (virtio_blk_write(2048, sec_w) != 0 ||
        virtio_blk_read(2048, sec_r) != 0 ||
        memcmp(sec_w, sec_r, 512) != 0) {
        kprint("FALLO de I/O en virtio-blk\\n");
        bg_puts(out_buf, max_out, &pos, "- VirtIO-BLK: FALLO\\n");
        failed++;
    } else {
        kprint("OK (LBA 2048 verificado)\\n");
        bg_puts(out_buf, max_out, &pos, "- VirtIO-BLK: OK\\n");
    }

    /* 3. Test de Sistema de Archivos Persistente (RamFS / MYOSFS01) */
    kprint("[TEST 3/5] Sistema de Archivos RamFS (Ciclo CRUD y sync)... ");
    const char *tfile = "/.test_canary.tmp";
    const char *tdata = "MYOS_FS_CANARY_VALIDATION_STRING";
    char rdata[64];
    memset(rdata, 0, sizeof(rdata));
    int w_res = vfs_write(tfile, tdata, 32);
    int r_res = vfs_read(tfile, rdata, sizeof(rdata));
    int d_res = vfs_delete(tfile);
    int r2_res = vfs_read(tfile, rdata, sizeof(rdata));
    if (w_res > 0 && r_res > 0 && memcmp(rdata, tdata, 32) == 0 && d_res == 0 && r2_res < 0) {
        kprint("OK (crear, leer, borrar, sync OK)\\n");
        bg_puts(out_buf, max_out, &pos, "- RamFS MYOSFS01: OK\\n");
    } else {
        kprint("FALLO en operaciones VFS\\n");
        bg_puts(out_buf, max_out, &pos, "- RamFS MYOSFS01: FALLO\\n");
        failed++;
    }

    /* 4. Test de Red Bare-Metal (ARP + Ping Gateway 10.0.2.2) */
    kprint("[TEST 4/5] Pila de Red (ARP + ICMP Ping Gateway 10.0.2.2)... ");
    int p_res = icmp_ping(net_gateway, 777, 1200);
    if (p_res == 1) {
        kprint("OK (Echo Reply recibido)\\n");
        bg_puts(out_buf, max_out, &pos, "- Red (ICMP Gateway): OK\\n");
    } else {
        kprint("FALLO (Gateway no responde)\\n");
        bg_puts(out_buf, max_out, &pos, "- Red (ICMP Gateway): FALLO\\n");
        failed++;
    }

    /* 5. Test de Enlace HTTP LLM Local (/health) - No bloqueante */
    kprint("[TEST 5/5] Enlace HTTP llama-server (/health)... ");
    if (llm_health()) {
        kprint("OK (HTTP 200 OK)\\n");
        bg_puts(out_buf, max_out, &pos, "- LLM Server: OK\\n");
    } else {
        kprint("AVISO (Servidor LLM inactivo o sin respuesta, no critico)\\n");
        bg_puts(out_buf, max_out, &pos, "- LLM Server: AVISO (offline o ocupado)\\n");
    }

    kprint("============================================================\\n");
    if (failed == 0) {
        kprint("RESUMEN: COMPONENTES HARDWARE OPERATIVOS (4/4 CRITICOS OK)\\n\\n");
        bg_puts(out_buf, max_out, &pos, "Resultado: Pruebas de hardware superadas. Kernel robusto.");
        return 0;
    } else {
        kprint("RESUMEN: DETECTADOS FALLOS CRITICOS EN EL KERNEL.\\n\\n");
        bg_puts(out_buf, max_out, &pos, "Resultado: FALLOS DETECTADOS en la suite de auto-test.");
        return -1;
    }
}

/* ---- Puerta de Arranque (Canary Boot) ----------------------------- */
void boot_gate_check(void)
{
    static char gate_sec[512];
    if (virtio_blk_read(BOOT_GATE_LBA, gate_sec) != 0) {
        return;
    }

    if (memcmp(gate_sec, "GATE_TEST_REQ", 13) != 0) {
        return;
    }

    boot_gate_active = 1;
    kprint("\\n============================================================\\n");
    kprint("[PUERTA DE ARRANQUE]: Verificando salud del nuevo kernel\\n");
    kprint("============================================================\\n");

    /* Limpiar sector para no quedar en bucle */
    memset(gate_sec, 0, sizeof(gate_sec));
    virtio_blk_write(BOOT_GATE_LBA, gate_sec);

    /* Ejecutar Suite de Hardware (Hard fails) */
    static char gate_report[512];
    if (boot_gate_run_test_suite(gate_report, sizeof(gate_report)) != 0) {
        kprint("[PUERTA DE ARRANQUE] ERROR: La suite de auto-test fallo. ABORTANDO KERNEL.\\n");
        qemu_exit(0x22); /* Dispara rollback del host */
        return;
    }

    /* Verificacion rapida y no bloqueante del LLM si el servicio esta levantado */
    if (llm_health()) {
        static char canary_reply[128];
        if (llm_chat("Responde: OK", canary_reply, sizeof(canary_reply), 10000)) {
            kprint("[PUERTA DE ARRANQUE] Enlace de inferencia LLM comprobado.\\n");
        } else {
            kprint("[PUERTA DE ARRANQUE] AVISO: Inferencia ocupada; kernel aprobado por hardware.\\n");
        }
    } else {
        kprint("[PUERTA DE ARRANQUE] AVISO: LLM offline; kernel aprobado por hardware.\\n");
    }

    kprint("[PUERTA DE ARRANQUE] EXITO: Kernel verificado y operativo.\\n");
    kprint("Notificando al host (aprobacion de parche)...\\n");
    boot_gate_active = 0;
    qemu_exit(0x20); /* Codigo de salida 65 (QEMU_EXIT_GATE_OK) */
}
"""


def main() -> int:
    print("-> Creando src/boot_gate.h y src/boot_gate.c...")
    BOOT_GATE_H.write_text(BOOT_GATE_H_CONTENT, encoding="utf-8")
    BOOT_GATE_C.write_text(BOOT_GATE_C_CONTENT, encoding="utf-8")

    # 1. Modificar Makefile para compilar boot_gate.o
    mk_text = MAKEFILE.read_text(encoding="utf-8")
    if "$(BUILD)/boot_gate.o" not in mk_text:
        old_mk = "$(BUILD)/idt.o"
        new_mk = "$(BUILD)/idt.o \\\n\t$(BUILD)/boot_gate.o"
        if old_mk in mk_text:
            mk_text = mk_text.replace(old_mk, new_mk, 1)
            MAKEFILE.write_text(mk_text, encoding="utf-8")
            print("[OK] Makefile actualizado con $(BUILD)/boot_gate.o.")
        else:
            print("ERROR: No se encontró $(BUILD)/idt.o en Makefile")
            return 1

    # 2. Modificar src/kernel.c: incluir boot_gate.h y remover bloques antiguos
    kc_text = KERNEL_C.read_text(encoding="utf-8")

    # Incluir boot_gate.h
    if '#include "boot_gate.h"' not in kc_text:
        kc_text = kc_text.replace('#include "idt.h"', '#include "idt.h"\n#include "boot_gate.h"', 1)

    # Reemplazar llamada en dispatch_command y remover declaración estática
    old_proto = "static uint32_t max_fb_dummy = 2048;\nstatic int kernel_run_test_suite(char *out_buf, uint32_t max_out);"
    new_proto = "static uint32_t max_fb_dummy = 2048;"
    if old_proto in kc_text:
        kc_text = kc_text.replace(old_proto, new_proto, 1)

    kc_text = kc_text.replace(
        "kernel_run_test_suite(out_buf, max_out);",
        "boot_gate_run_test_suite(out_buf, max_out);",
        1
    )

    # Remover el cuerpo de kernel_run_test_suite y boot_gate_check
    marker_start = "/* ---- Suite de Auto-Test de No Regresion (Bare-Metal) ------------- */"
    marker_end = "void kernel_main(void)"

    if marker_start in kc_text and marker_end in kc_text:
        start_idx = kc_text.find(marker_start)
        end_idx = kc_text.find(marker_end)
        kc_text = kc_text[:start_idx] + kc_text[end_idx:]
        print("[OK] Funciones de boot_gate removidas de src/kernel.c.")
    else:
        print("ERROR: No se encontraron los marcadores de la suite en src/kernel.c")
        return 1

    KERNEL_C.write_text(kc_text, encoding="utf-8")

    # 3. Blindaje en ejecutar.py
    ej_text = EJECUTAR.read_text(encoding="utf-8")
    old_target = (
        "def safe_target(name: str) -> Path | None:\n"
        "    if \"\\x00\" in name:\n"
        "        return None\n"
        "    p = (ROOT / name).resolve()          # resuelve '..' y symlinks\n"
        "    src = SRC_DIR.resolve()\n"
        "    if p.is_relative_to(src) and p.suffix.lower() in ALLOWED_EXT and p.is_file():\n"
        "        return p\n"
        "    return None"
    )

    new_target = (
        "PROTECTED_FILES = {\n"
        "    'boot.s', 'idt.c', 'idt.h', 'io.h', 'srcfs.c', 'srcfs.h',\n"
        "    'virtio_blk.c', 'virtio_blk.h', 'boot_gate.c', 'boot_gate.h'\n"
        "}\n\n\n"
        "def is_target_protected(path: Path) -> bool:\n"
        "    if path.name in PROTECTED_FILES:\n"
        "        return True\n"
        "    prot_txt = ROOT / 'protected.txt'\n"
        "    if prot_txt.exists():\n"
        "        for line in prot_txt.read_text(encoding='utf-8').splitlines():\n"
        "            line = line.strip()\n"
        "            if line and not line.startswith('#') and Path(line).name == path.name:\n"
        "                return True\n"
        "    return False\n\n\n"
        "def safe_target(name: str) -> Path | None:\n"
        "    if \"\\x00\" in name:\n"
        "        return None\n"
        "    p = (ROOT / name).resolve()          # resuelve '..' y symlinks\n"
        "    src = SRC_DIR.resolve()\n"
        "    if p.is_relative_to(src) and p.suffix.lower() in ALLOWED_EXT and p.is_file():\n"
        "        if is_target_protected(p):\n"
        "            print(f\"[SEGURIDAD HOST] Modificacion denegada: '{p.name}' es un archivo protegido.\")\n"
        "            return None\n"
        "        return p\n"
        "    return None"
    )

    if old_target in ej_text:
        ej_text = ej_text.replace(old_target, new_target, 1)
        EJECUTAR.write_text(ej_text, encoding="utf-8")
        print("[OK] ejecutar.py blindado: la IA ya no puede modificar archivos protegidos.")

    # 4. Verificación de compilación en Docker
    print("-> Verificando compilación completa en contenedor...")
    if not run_docker_check():
        print("[ERROR] Falló la compilación del nuevo sistema.")
        return 1

    print("\n[ÉXITO] Arquitectura blindada completada:")
    print("        1. Submódulo src/boot_gate.c creado y desacoplado.")
    print("        2. Canary Boot insensible a cortes o latencias del LLM.")
    print("        3. Archivos del sistema y suite de tests protegidos contra la IA en ejecutar.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main())