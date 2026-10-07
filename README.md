# SOMA 0.2 — AI-Native Multi-Agent Operating System

SOMA (Sistema Operativo Multi-Agente) es un sistema operativo experimental de 64 bits para arquitecturas x86_64 y AArch64 (ARM64) escrito desde cero en C y ensamblador freestanding (sin Linux, sin glibc, sin POSIX y sin librerías externas).

El núcleo proporciona a un ecosistema de Modelos de Lenguaje (LLMs) locales un entorno informático propio en Ring 0 con control directo sobre el hardware (bare-metal), memoria persistente, motor de búsqueda semántica BM25, servidor web y delegación autónoma jerárquica.

---

## Características Principales

1. Sistema Multiagente Jerárquico y Pizarra Compartida:
   - Orquestación y Delegación (delegate <agente> "<misión>").
   - Pizarra Compartida (/agent/blackboard.txt) para comunicación inter-agente.
   - Espacio de Trabajo (/workspace/) y Directorio de Usuario (/mis_archivos/) con visibilidad configurable (ls vs ls -a).

2. Motor de Búsqueda Semántica BM25 en C Bare-Metal:
   - Algoritmo BM25 / TF-IDF (mem_search / bm25) con normalización diacrítica UTF-8 (á, é, í, ó, ú, ñ).

3. Almacenamiento Persistente VFS V2 (SOMAFS02):
   - 64 inodos en virtio-blk con archivos de hasta 16 KiB (16.384 bytes) desacoplados dinámicamente en el KHeap.

4. Servidor Web Bare-Metal HTTPD en Ring 0:
   - Dashboard interactivo HTML5 en el puerto 80 (http://localhost:8090/).
   - Explorador visual de archivos y endpoints REST JSON (/api/telemetry, /api/fs, /api/blackboard).

5. Pila TCP/IP y Multitarea:
   - Paginación x86_64 de 4 niveles (PML4) con protección CR0.WP y bit NX.
   - Demonios de kernel activos (idle, netd, sysmon, httpd).
   - Pila de red completa: Ethernet, ARP, IPv4, ICMP, UDP, DNS WAN y TCP.

---

## Compilación y Arranque

1. Clona el repositorio:
   git clone https://github.com/mlalejandre/soma.git
   cd soma

2. Ejecuta el runner automático:
   python3 ejecutar.py

---

## Licencia

Este proyecto está bajo la Licencia MIT. Consulta el archivo LICENSE para más detalles.\n