#!/usr/bin/env python3
"""
parche.py - Reorganización modular del proyecto SOMA:
1. Crea la carpeta 'tools/' para los scripts de soporte del Host (search_proxy.py, llm_server.py, estructura.py).
2. Deja 'src/' exclusivamente para código C/ASM bare-metal de Ring 0.
3. Actualiza las rutas de importación en ejecutar.py y estructura.py.
4. Elimina archivos residuales (limpiar.sh, archivos temporales).
"""

import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TOOLS_DIR = ROOT / "tools"

def main():
    print("=" * 72)
    print("SOMA OS :: REORGANIZACIÓN MODULAR DEL PROYECTO")
    print("=" * 72)

    if not (ROOT / "src" / "kernel.c").exists():
        print("ERROR: Ejecuta este script en la raíz del proyecto 'soma/'.")
        return 1

    # 1. Crear directorio tools/
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    (TOOLS_DIR / "__init__.py").write_text("# SOMA Host Tools\n", encoding="utf-8")
    print("[OK] Directorio 'tools/' creado.")

    # 2. Mover herramientas del host a tools/
    host_tools = ["search_proxy.py", "llm_server.py", "estructura.py"]
    for tool_name in host_tools:
        src_path = ROOT / tool_name
        dst_path = TOOLS_DIR / tool_name
        if src_path.exists():
            shutil.move(str(src_path), str(dst_path))
            print(f"  -> Movido '{tool_name}' a 'tools/{tool_name}'.")

    # 3. Eliminar archivos temporales o scripts sobrantes en la raíz
    leftovers = ["limpiar.sh", "curl"]
    for item in leftovers:
        p = ROOT / item
        if p.exists():
            p.unlink()
            print(f"  -> Eliminado archivo sobrante '{item}'.")

    # 4. Actualizar tools/estructura.py para que la raíz sea el proyecto completo
    estructura_py = TOOLS_DIR / "estructura.py"
    if estructura_py.exists():
        text = estructura_py.read_text(encoding="utf-8")
        text = text.replace(
            "PROJECT_ROOT = Path(__file__).resolve().parent",
            "PROJECT_ROOT = Path(__file__).resolve().parent.parent"
        )
        estructura_py.write_text(text, encoding="utf-8")
        print("  -> 'tools/estructura.py' ajustado para escanear desde la raíz.")

    # 5. Actualizar ejecutar.py para importar desde tools/
    ejecutar_py = ROOT / "ejecutar.py"
    if ejecutar_py.exists():
        text = ejecutar_py.read_text(encoding="utf-8")
        old_import = "import llm_server\nimport search_proxy"
        new_import = "sys.path.insert(0, str(ROOT / \"tools\"))\nimport llm_server\nimport search_proxy"
        
        if old_import in text and "sys.path.insert(0, str(ROOT / \"tools\"))" not in text:
            text = text.replace(old_import, new_import)
            ejecutar_py.write_text(text, encoding="utf-8")
            print("  -> 'ejecutar.py' actualizado con la ruta de importación 'tools/'.")

    # 6. Actualizar README.md con la nueva estructura
    readme_path = ROOT / "README.md"
    if readme_path.exists():
        text = readme_path.read_text(encoding="utf-8")
        text = text.replace("search_proxy.py", "tools/search_proxy.py")
        text = text.replace("llm_server.py", "tools/llm_server.py")
        readme_path.write_text(text, encoding="utf-8")
        print("  -> 'README.md' actualizado con la estructura modular.")

    print("\n" + "=" * 72)
    print("REORGANIZACIÓN COMPLETADA CON ÉXITO")
    print("=" * 72)
    print("Estructura actual:")
    print("  - soma/src/    -> Código fuente C y ASM bare-metal (Ring 0)")
    print("  - soma/tools/  -> Herramientas Python del Host (proxy, llm, estructura)")
    print("  - soma/tests/  -> Smoke tests e integración")
    print("=" * 72)
    return 0

if __name__ == "__main__":
    sys.exit(main())