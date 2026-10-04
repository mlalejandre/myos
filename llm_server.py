#!/usr/bin/env python3
# llm_server.py - Gestion del servidor llama.cpp remoto para MYOS.
#
# Antes de arrancar QEMU:
#   1) Comprueba /health. Si esta activo, lo usa (y ofrece cambiar de modelo).
#   2) Si no, lista los .gguf del servidor por SSH, muestra un menu y lanza
#      llama-server con los mismos flags que el otro proyecto.
#   3) Espera a que cargue el modelo (/health devuelve 503 mientras carga).
# Todo se configura con las mismas variables de entorno que config.py.

from __future__ import annotations

import atexit
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

SERVER_USER = os.getenv("SERVER_USER", "mlalejandre")
SERVER_IP = os.getenv("SERVER_IP", "192.168.1.200")
SERVER_PORT = int(os.getenv("SERVER_PORT_4090", os.getenv("SERVER_PORT", "8087")))

LLAMA_SERVER_BIN = os.getenv(
    "LLAMA_SERVER_BIN",
    "/home/mlalejandre/llama.cpp-qwen38-opt/build/bin/llama-server",
)
MODELS_DIR = os.getenv("MODELS_DIR", "/mnt/IA/models")
DEFAULT_MODEL_PATH = os.getenv(
    "REMOTE_MODEL_PATH",
    "/mnt/IA/models/nail-35b/Nail-Qwen3.6-35B-A3B-UD-Q4_K_S.gguf",
)
DEFAULT_ALIAS = os.getenv("REMOTE_MODEL_ALIAS", "nail-35b")

# PID/log propios de MYOS para no pisar los del otro proyecto.
REMOTE_PID_FILE = os.getenv("MYOS_PID_FILE", "/tmp/myos-llama.pid")
REMOTE_LOG_FILE = os.getenv("MYOS_LOG_FILE", "/tmp/myos-llama.log")

CTX = int(os.getenv("REMOTE_CTX", "32768"))
THREADS = int(os.getenv("REMOTE_THREADS", "8"))
CUDA_DEVICE = os.getenv("CUDA_DEVICE", "0")
EXTRA_ARGS = os.getenv("LLAMA_EXTRA_ARGS", "")
HEALTH_TIMEOUT_S = float(os.getenv("SERVER_HEALTH_TIMEOUT_S", "180"))

SPLIT_RE = re.compile(r"-(\d{5})-of-(\d{5})\.gguf$")

_started_by_us = False
_keep_running = False
_atexit_registered = False


def info(msg: str) -> None:
    print(f"[LLM] {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"[LLM] AVISO: {msg}", flush=True)


@dataclass
class Model:
    path: str
    size: int

    @property
    def rel(self) -> str:
        if self.path.startswith(MODELS_DIR):
            return self.path[len(MODELS_DIR):].lstrip("/")
        return self.path


# ---------------------------------------------------------------- SSH / HTTP

def run_ssh(command: str, timeout: int = 15) -> subprocess.CompletedProcess:
    cmd = [
        "ssh",
        "-o", "ConnectTimeout=4",
        "-o", "BatchMode=yes",
        "-o", "ServerAliveInterval=15",
        "-o", "ServerAliveCountMax=2",
        f"{SERVER_USER}@{SERVER_IP}",
        command,
    ]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)


def health_status() -> int | None:
    # 200 = listo, 503 = cargando modelo, None = sin respuesta.
    url = f"http://{SERVER_IP}:{SERVER_PORT}/health"
    try:
        with urllib.request.urlopen(url, timeout=2.0) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except Exception:
        return None


def loaded_model() -> str:
    try:
        url = f"http://{SERVER_IP}:{SERVER_PORT}/v1/models"
        with urllib.request.urlopen(url, timeout=3.0) as r:
            return json.load(r)["data"][0]["id"]
    except Exception:
        return "desconocido"


def log_tail(lines: int = 30) -> str:
    try:
        r = run_ssh(f"tail -n {lines} {shlex.quote(REMOTE_LOG_FILE)}", timeout=8)
        return r.stdout.strip()
    except Exception:
        return ""


def process_alive() -> bool:
    try:
        pid = shlex.quote(REMOTE_PID_FILE)
        r = run_ssh(f"kill -0 $(cat {pid}) 2>/dev/null && echo up || echo down", timeout=8)
        return "up" in r.stdout
    except Exception:
        return True  # ante la duda, seguir esperando


# ---------------------------------------------------------------- modelos

def list_models() -> list[Model]:
    cmd = (
        f"find {shlex.quote(MODELS_DIR)} -type f -name '*.gguf' "
        "-printf '%s\\t%p\\n' 2>/dev/null"
    )
    try:
        out = run_ssh(cmd, timeout=40).stdout
    except Exception as e:
        warn(f"no se pudo listar modelos: {e}")
        return []

    groups: dict[str, dict] = {}
    for line in out.splitlines():
        size_s, _, path = line.partition("\t")
        if not size_s.isdigit() or not path:
            continue
        if "mmproj" in os.path.basename(path).lower():
            continue  # proyectores de vision: no son modelos autonomos
        m = SPLIT_RE.search(path)
        key = SPLIT_RE.sub(".gguf", path)
        g = groups.setdefault(key, {"size": 0, "first": None})
        g["size"] += int(size_s)
        if m is None or m.group(1) == "00001":
            g["first"] = path  # llama.cpp carga el resto de partes solo

    models = [Model(g["first"], g["size"]) for g in groups.values() if g["first"]]
    models.sort(key=lambda m: m.path.lower())
    return models


def alias_for(path: str) -> str:
    if path == DEFAULT_MODEL_PATH:
        return DEFAULT_ALIAS
    name = os.path.basename(path)
    name = SPLIT_RE.sub("", name)
    name = re.sub(r"\.gguf$", "", name, flags=re.I)
    return re.sub(r"[^a-z0-9._-]+", "-", name.lower()).strip("-")[:48] or "model"


def choose_model(models: list[Model]) -> Model | None:
    default_idx = next((i for i, m in enumerate(models) if m.path == DEFAULT_MODEL_PATH), 0)

    if not sys.stdin.isatty():
        return models[default_idx]

    print("\nModelos disponibles en el servidor:")
    for i, m in enumerate(models, 1):
        mark = "   <- por defecto" if i - 1 == default_idx else ""
        print(f"  {i:2d}) {m.rel}  [{m.size / 2**30:.1f} GiB]{mark}")

    while True:
        try:
            raw = input(f"\nElige modelo [1-{len(models)}, Enter={default_idx + 1}, q=sin LLM]: ")
        except EOFError:
            return models[default_idx]
        raw = raw.strip().lower()
        if raw == "":
            return models[default_idx]
        if raw == "q":
            return None
        if raw.isdigit() and 1 <= int(raw) <= len(models):
            return models[int(raw) - 1]
        print("Opcion no valida.")


# ---------------------------------------------------------------- ciclo de vida

def launch(model_path: str, alias: str) -> bool:
    global _started_by_us

    args = [
        LLAMA_SERVER_BIN,
        "-m", model_path,
        "--alias", alias,
        "--host", "0.0.0.0",          # imprescindible: si no, la LAN recibe RST
        "--port", str(SERVER_PORT),
        "-ngl", "all",
        "--split-mode", "none",
        "--flash-attn", "on",
        "-c", str(CTX),
        "-n", "8192",
        "-np", "1",
        "-ctk", "q8_0", "-ctv", "q8_0",
        "--threads", str(THREADS),
    ]
    if EXTRA_ARGS:
        args += shlex.split(EXTRA_ARGS)

    q = shlex.quote
    cmd = (
        f"kill -9 $(lsof -iTCP:{SERVER_PORT} -sTCP:LISTEN -t 2>/dev/null) 2>/dev/null || true; "
        f"rm -f {q(REMOTE_PID_FILE)}; "
        f"CUDA_VISIBLE_DEVICES={q(CUDA_DEVICE)} nohup {shlex.join(args)} "
        f"> {q(REMOTE_LOG_FILE)} 2>&1 < /dev/null & "
        f"echo $! > {q(REMOTE_PID_FILE)}"
    )

    info(f"Lanzando llama-server con '{alias}' ({os.path.basename(model_path)}) ...")
    try:
        res = run_ssh(cmd, timeout=20)
    except Exception as e:
        warn(f"fallo al lanzar por SSH: {e}")
        return False
    if res.returncode != 0:
        warn(f"fallo al lanzar llama-server: {res.stderr.strip()}")
        return False

    _started_by_us = True
    return True


def wait_ready() -> bool:
    deadline = time.time() + HEALTH_TIMEOUT_S
    info(f"Esperando a http://{SERVER_IP}:{SERVER_PORT}/health (max {int(HEALTH_TIMEOUT_S)}s) ...")
    last_print = 0.0
    last_alive = time.time()

    while time.time() < deadline:
        if health_status() == 200:
            info("Servidor listo.")
            return True

        now = time.time()
        if now - last_print >= 5.0:
            info(f"   cargando modelo... ({int(deadline - now)}s restantes)")
            last_print = now

        if _started_by_us and now - last_alive >= 10.0:
            last_alive = now
            if not process_alive():
                warn("el proceso llama-server ha terminado. Ultimas lineas del log:")
                print(log_tail())
                return False

        time.sleep(2)

    warn("tiempo agotado. Ultimas lineas del log:")
    print(log_tail())
    return False


def stop_server() -> None:
    global _started_by_us
    if not _started_by_us or _keep_running:
        return
    info("Deteniendo llama-server (liberando VRAM) ...")
    pid = shlex.quote(REMOTE_PID_FILE)
    cmd = (
        f"if [ -f {pid} ]; then P=$(cat {pid} 2>/dev/null); "
        f'[ -n "$P" ] && kill "$P" 2>/dev/null; sleep 2; '
        f'[ -n "$P" ] && kill -9 "$P" 2>/dev/null; rm -f {pid}; fi; true'
    )
    try:
        run_ssh(cmd, timeout=15)
    except Exception as e:
        warn(f"no se pudo detener limpiamente: {e}")
    _started_by_us = False


def add_arguments(ap) -> None:
    ap.add_argument("--modelo", action="store_true",
                    help="mostrar el menu de modelos del servidor aunque ya haya uno activo")
    ap.add_argument("--sin-servidor", action="store_true",
                    help="no gestionar llama-server (MYOS arranca sin comprobar el LLM)")
    ap.add_argument("--dejar-servidor", action="store_true",
                    help="no detener llama-server al salir (por defecto se detiene si lo lanzo yo)")


def ensure_server(args) -> None:
    global _keep_running, _atexit_registered

    _keep_running = bool(getattr(args, "dejar_servidor", False))
    if not _atexit_registered:
        atexit.register(stop_server)
        _atexit_registered = True

    print("\n" + "=" * 72 + "\nSERVIDOR LLM (llama.cpp)\n" + "=" * 72)

    if getattr(args, "sin_servidor", False):
        info("Gestion del servidor omitida (--sin-servidor).")
        return

    status = health_status()
    force_menu = bool(getattr(args, "modelo", False))

    if status == 200 and not force_menu:
        info(f"llama-server activo en {SERVER_IP}:{SERVER_PORT}, modelo: {loaded_model()}")
        if not sys.stdin.isatty():
            return
        try:
            ans = input("Cambiar de modelo? [y/N] ").strip().lower()
        except EOFError:
            return
        if ans != "y":
            return
        force_menu = True

    if status == 503 and not force_menu:
        info("llama-server esta cargando un modelo.")
        wait_ready()
        return

    # Hay que lanzar (o relanzar) el servidor: hace falta SSH.
    if shutil.which("ssh") is None:
        warn("no hay cliente ssh; MYOS arrancara sin LLM.")
        return
    try:
        r = run_ssh("exit", timeout=8)
    except Exception as e:
        warn(f"SSH no responde ({e}); MYOS arrancara sin LLM.")
        return
    if r.returncode != 0:
        warn(f"SSH con {SERVER_USER}@{SERVER_IP} fallo: {r.stderr.strip()}. MYOS arrancara sin LLM.")
        return

    models = list_models()
    if not models:
        warn(f"no encontre .gguf en {MODELS_DIR}; uso el modelo por defecto.")
        models = [Model(DEFAULT_MODEL_PATH, 0)]

    choice = choose_model(models)
    if choice is None:
        info("Sin LLM: MYOS arrancara sin servidor.")
        return

    if launch(choice.path, alias_for(choice.path)):
        if not wait_ready():
            warn("el servidor no llego a estar listo; MYOS arrancara y el test LLM fallara.")
