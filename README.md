# 🧠 SOMA v0.2 — AI-Native Multi-Agent Operating System

![Architecture](https://img.shields.io/badge/Architecture-x86__64%20%7C%20AArch64-blue?style=for-the-badge)
![Language](https://img.shields.io/badge/Language-Freestanding%20C%20%7C%20ASM-green?style=for-the-badge)
![Storage](https://img.shields.io/badge/Storage-SOMAFS02%20(16KiB)-orange?style=for-the-badge)
![AI](https://img.shields.io/badge/AI-Hierarchical%20ReAct%20Agents-purple?style=for-the-badge)
![License](https://img.shields.io/badge/License-MIT-brightgreen?style=for-the-badge)

**SOMA** (Sistema Operativo Multi-Agente) es un sistema operativo experimental de 64 bits escrito **desde cero en C y Ensamblador *freestanding*** (sin Linux, sin glibc, sin POSIX y sin dependencias externas).

A diferencia de los asistentes virtuales tradicionales que corren como aplicaciones sobre sistemas operativos anfitriones, SOMA proporciona a los Modelos de Lenguaje (LLMs) **un entorno informático nativo y aislado en Ring 0**, con control absoluto sobre el hardware, la red y la memoria.

---

## ✨ Características Principales

### 🤖 Inteligencia Artificial y Orquestación Multi-Agente
- **Kernel ReAct:** Bucle autónomo integrado en el núcleo que permite a la IA razonar, ejecutar comandos binarios y evaluar la telemetría del sistema en tiempo real.
- **Delegación Jerárquica:** El orquestador (`soma`) puede subcontratar tareas de investigación compleja a agentes especialistas (ej. `buscador`).
- **Pizarra Compartida (IPC):** Sincronización de contexto inter-agente mediante un *Blackboard* en RamFS (`/agent/blackboard.txt`).

### 🔍 Motor Semántico Bare-Metal
- **Algoritmo BM25 / TF-IDF:** Motor de búsqueda nativo en C (`mem_search`) que evalúa la relevancia y densidad de términos en el disco duro.
- **Soporte UTF-8:** Ponderación con normalización diacrítica (identifica `computación` y `computacion` por igual).

### 🌐 Pila de Red TCP/IP y Web (Ring 0)
- **Networking desde cero:** Controladores VirtIO-NET, Ethernet, caché ARP, IPv4, ICMP, UDP y TCP (RFC 793).
- **Servidor Web HTTPD:** Dashboard HTML5 interactivo servido directamente desde la memoria física en el puerto 80.
- **API REST Integrada:** Endpoints JSON en vivo (`/api/telemetry`, `/api/fs`) para monitorización externa.

### 💾 Almacenamiento Persistente y Memoria
- **SOMAFS02:** Sistema de archivos de alta capacidad (16 KiB por archivo, 64 inodos dinámicos) sobre VirtIO-BLK.
- **Jerarquía de Espacios:** Rutas con políticas de aislamiento (`/workspace/` para la IA, `/mis_archivos/` para el usuario).
- **KHeap Dinámico:** Asignador elástico de memoria virtual sobre VMM (Paginación de 4 niveles) libre de fragmentación.

---

## 🏗️ Arquitectura del Sistema

```text
       +-----------------------------------------------------------+
       |                  SOMA BARE-METAL KERNEL                   |
       |                                                           |
       |   +-------------------+       +-----------------------+   |
       |   |  Interactive CLI  | <---> |   Autonomous ReAct    |   |
       |   | (UTF-8 / COM1/VGA)|       |  Orchestrator (soma)  |   |
       |   +-------------------+       +-----------------------+   |
       |             |                             | (delegate)    |
       |   +-------------------+       +-----------------------+   |
       |   | RamFS V2 (SOMAFS) |       | Research Agent        |   |
       |   | /workspace, /mem  | <---> | (buscador) / BB IPC   |   |
       |   +-------------------+       +-----------------------+   |
       |             |                             |               |
       |   +---------------------------------------------------+   |
       |   |  BM25 Semantic Search & KHeap Allocator (64 MiB)  |   |
       |   +---------------------------------------------------+   |
       |             |                             |               |
       |   +-------------------+       +-----------------------+   |
       |   | Ring 0 Web Server |       |   Bare-Metal TCP/IP   |   |
       |   | (httpd Dashboard) |       |  TCP, UDP, IPv4, ARP  |   |
       |   +-------------------+       +-----------------------+   |
       |                             |                             |
       |             +-------------------------------+             |
       |             | VirtIO-NET & VirtIO-BLK (Disk)|             |
       |             +-------------------------------+             |
       +-----------------------------|-----------------------------+
                                     | (VirtIO Queues)
       +-----------------------------------------------------------+
       |                     QEMU Hypervisor                       |
       |              (Host Web Forwarding :8090)                  |
       +-----------------------------|-----------------------------+
                                     | SLIRP NAT / LAN
       +-----------------------------------------------------------+
       |               Local LAN Server (llama.cpp)                |
       |          192.168.1.200:8087 (Model: nail-35b)             |
       +-----------------------------------------------------------+
🚀 Instalación y Arranque (Quickstart)
El proyecto utiliza un entorno reproducible basado en Docker para la compilación cruzada x86_64 / AArch64 y QEMU para la emulación.

Clona el repositorio:

code
Bash
git clone https://github.com/mlalejandre/soma.git
cd soma
Ejecuta el orquestador automático:

code
Bash
python3 ejecutar.py
Este script construirá el toolchain, compilará el kernel en C/ASM, generará la ISO de arranque y lanzará el sistema en QEMU.

Accede al Dashboard Web:
Abre tu navegador en el host y dirígete a http://localhost:8090/.

💻 El Intérprete de Comandos (soma> )
Una vez iniciado el sistema, dispones de una shell interactiva tipo Unix:

Operaciones Multi-Agente
soma <mision> : Despierta al orquestador central y le asigna un objetivo.

delegate <agente> "<misión>" : Comando interno para delegación entre especialistas.

bm25 <termino> : Ejecuta una búsqueda semántica en la memoria persistente.

Gestión de Sistema y Red
somafetch : Muestra la telemetría del sistema en arte ASCII.

httpd : Muestra el estado y estadísticas del servidor web de Ring 0.

ps / threads : Tabla en tiempo real de los KThreads y demonios en ejecución.

ping <ip> / curl <url> : Pruebas de la pila TCP/IP nativa.

Sistema de Archivos
ls -a /workspace : Lista el contenido del espacio de trabajo.

cat <archivo> : Muestra el contenido de texto (soporta tuberías |).

nano <archivo> : Abre el editor de texto visual a pantalla completa (80x25).

🤝 Ejemplo Práctico: Coordinación Autónoma
Si le pides al kernel:

code
Text
soma> soma investiga los principios de la computación cuántica y guarda tus notas
soma analiza que carece de información y ejecuta delegate buscador "principios cuánticos...".

buscador utiliza search y curl para recuperar datos de la web real, sintetiza un resumen y lo escribe en la Pizarra Compartida.

soma retoma el control, lee la pizarra y ejecuta la herramienta write para persistir el informe técnico en /workspace/notas_quantica.txt.

El usuario puede ver el resultado desde el Dashboard Web en el host.

📄 Licencia y Contribución
Este proyecto es experimental y de código abierto bajo la Licencia MIT. Consulta el archivo LICENSE para más detalles.
