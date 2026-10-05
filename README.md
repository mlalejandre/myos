# MYOS 0.1 — Autonomous AI-Native x86_64 Operating System

![Architecture](https://img.shields.io/badge/Architecture-x86__64%20(Long%20Mode)-blue)
![Language](https://img.shields.io/badge/Language-Freestanding%20C%20%7C%20ASM-green)
![Networking](https://img.shields.io/badge/Network-Custom%20TCP%2FIP%20Stack-orange)
![AI](https://img.shields.io/badge/AI-Autonomous%20ReAct%20Agent-purple)
![License](https://img.shields.io/badge/License-MIT-brightgreen)

**MYOS** es un sistema operativo experimental de 64 bits para arquitectura x86_64 escrito **desde cero en C y ensamblador *freestanding*** (sin Linux, sin glibc, sin POSIX y sin librerías externas).

El objetivo del proyecto es proporcionar a un Modelo de Lenguaje (LLM) local un entorno informático propio, aislado y de control directo sobre el hardware (*bare-metal*), permitiéndole inspeccionar el sistema, operar herramientas de diagnóstico, manipular el sistema de archivos y avanzar hacia un ciclo autónomo de modificación y auto-evolución.

---

## 🏛️ Arquitectura del Sistema

```text
       +-----------------------------------------------------------+
       |                  MYOS BARE-METAL KERNEL                   |
       |                                                           |
       |   +-------------------+       +-----------------------+   |
       |   |  Interactive CLI  | <---> |   Autonomous ReAct    |   |
       |   |   (Serial COM1)   |       |      Agent Loop       |   |
       |   +-------------------+       +-----------------------+   |
       |             |                             |               |
       |   +-------------------+       +-----------------------+   |
       |   | RamFS (In-Memory) |       |  Hardware Tool-Call   |   |
       |   |  /etc, /src, ...  |       | stats, ping, mem, ... |   |
       |   +-------------------+       +-----------------------+   |
       |             |                             |               |
       |   +---------------------------------------------------+   |
       |   |      Heap Allocator (kmalloc / kfree - 12 MiB)    |   |
       |   +---------------------------------------------------+   |
       |             |                             |               |
       |   +-------------------+       +-----------------------+   |
       |   |  HTTP/1.0 Client  |       |   Bare-Metal TCP/IP   |   |
       |   |  & JSON Extractor |       |  TCP, UDP, IPv4, ARP  |   |
       |   +-------------------+       +-----------------------+   |
       |                             |                             |
       |             +-------------------------------+             |
       |             | VirtIO-NET PCI Legacy Driver  |             |
       |             |   (Descriptor & Ring Queues)  |             |
       |             +-------------------------------+             |
       +-----------------------------|-----------------------------+
                                     | (virtqueue TX/RX)
       +-----------------------------------------------------------+
       |                     QEMU Hypervisor                       |
       |                      (-netdev user)                       |
       +-----------------------------|-----------------------------+
                                     | SLIRP NAT / Wi-Fi
       +-----------------------------------------------------------+
       |               Local LAN Server (llama.cpp)                |
       |          192.168.1.200:8087 (Model: nail-35b)             |
       +-----------------------------------------------------------+
```

---

## ⚡ Características Principales

### 1. Núcleo y Arquitectura de CPU
- **Bootstrap Multiboot 1 (`boot.s`)**: Conmutación de modo protegido de 32 bits a **Long Mode de 64 bits**.
- **Paginación propia de 4 niveles (`PML4`)**: Mapeo directo (*identity-mapped*) del primer 1 GiB de memoria física en páginas de 2 MiB.
- **Soporte SSE/SSE2**: Activación a nivel de registro de control `CR4.OSFXSR` y `CR4.PAE`.
- **E/S Básica**: Consola por puerto serie COM1 (`0x3F8`) con soporte de edición de línea y búfer VGA (`0xB8000`).

### 2. Controladores de Hardware
- **Escaneo de Bus PCI**: Mecanismo de configuración por puertos I/O `0xCF8` y `0xCFC`.
- **VirtIO-NET Legacy**: Negociación de características, configuración de `BAR0` I/O y gestión de *virtqueues* en memoria física fija (`0x00200000`).

### 3. Pila de Red Completa desde Cero
- **Ethernet**: Formateo de tramas y resolución MAC.
- **ARP**: Tabla de resolución y caché activa de 8 entradas.
- **IPv4**: Encabezado IPv4 con cálculo de checksum RFC 1071.
- **ICMP**: Soporte para ping bare-metal (*echo request/reply*).
- **UDP & Cliente DNS**: Resolución de nombres mediante consultas a `10.0.2.3:53`.
- **Máquina de Estados TCP**: Handshake de tres vías (`SYN`, `SYN-ACK`, `ACK`), control de flujo por ventana deslizante de **32 KiB**, retransmisiones por RTO y cierre ordenado `FIN/ACK`.

### 4. Capa de Aplicación e Integración de IA
- **Cliente HTTP/1.0**: Peticiones `GET` y `POST` con soporte de `Content-Length` y cabeceras personalizadas.
- **Parser JSON Bare-Metal**: Navegador de árboles de texto sin dependencias para extraer `choices[0].message.content`.
- **Conexión Local**: Enlace con `llama-server` (modelo `nail-35b` de 35.000 millones de parámetros) cruzando la red física del host.

### 5. Memoria Dinámica y Sistema de Archivos
- **Heap Allocator (`kmalloc` / `kfree`)**: Asignador de bloques dinámicos alineados a 16 bytes con un pool de **12 MiB** ubicado en `0x00400000`.
- **RamFS (Sistema de Archivos en Memoria)**: Sistema de inodos en memoria montado en `/` con operaciones `ls`, `cat`, `write`, `rm` y código fuente del kernel precargado en `/src/`.

### 6. Agente Autónomo ReAct (Multi-Turn Bare-Metal Tool Calling)
- El kernel implementa un bucle autónomo **ReAct (Reasoning + Action)**:
  1. El usuario asigna una misión en lenguaje natural (`agent <mision>`).
  2. La IA analiza la tarea y emite una directiva estructurada (`CMD: <herramienta>`).
  3. El kernel ejecuta la orden en el hardware (`ping`, `stats`, `mem`, `cat`, `write`, `rm`).
  4. El kernel retroalimenta la telemetría en vivo a la IA manteniendo la **memoria contextual acumulativa**.
  5. La IA encadena hasta 4 pasos autónomos y concluye emitiendo su dictamen final.

---

## 🛠️ Requisitos de Desarrollo

El proyecto utiliza un entorno reproducible basado en **Docker** para la compilación cruzada x86_64 y **QEMU** para la emulación:

- **Host recomendado**: macOS (Apple Silicon / Intel) o Linux.
- **Docker**: Para el contenedor de compilación (`myos-toolchain`).
- **QEMU**: `qemu-system-x86_64`.
- **Python 3**: Para los scripts de compilación y orquestación.
- **Servidor LLM (Opcional para IA)**: Instancia de `llama.cpp` (`llama-server`) en la red local.

---

## 🚀 Compilación y Arranque

1. Clona el repositorio:
   ```bash
   git clone https://github.com/mlalejandre/myos.git
   cd myos
   ```

2. Ejecuta el runner automático:
   ```bash
   python3 ejecutar.py
   ```

El script se encargará automáticamente de:
1. Construir la imagen Docker con el toolchain de Debian bookworm (`gcc`, `binutils`, `grub-mkrescue`, `xorriso`).
2. Compilar los fuentes en C y ensamblador.
3. Generar la imagen ISO arrancable (`build/myos.iso`).
4. Iniciar QEMU con interfaz de red VirtIO-NET, captura PCAP en `build/net.pcap` y consola por terminal.

---

## 💻 Comandos del Shell (`myos> `)

Una vez iniciado el sistema en la terminal, dispones de una consola interactiva:

| Comando | Descripción |
| :--- | :--- |
| `help` | Muestra el catálogo completo de comandos del sistema |
| `test_suite` | Suite integral de no regresión (9 fases completas del kernel) |
| `ps` / `threads` | Supervisión de KThreads, estado de CPU y prueba de exclusión mutua |
| `spawn <cmd>` / `bg` | Lanza un comando o tarea en segundo plano con volcado a `/tmp/job_<TID>.log` |
| `kill <tid>` | Termina un hilo en segundo plano liberando sus mutexes y marcos PMM |
| `free` / `df` | Telemetría en vivo de RAM física, KHeap y capacidad del RamFS |
| `date` / `time <cmd>` | Reloj RTC CMOS en tiempo real y benchmarks en ms y ciclos TSC |
| `tree` | Muestra la estructura jerárquica de archivos y directorios del RamFS |
| `nano <archivo>` | Editor de texto en pantalla completa (80x25) con guardado (^S) y salida (^X) |
| `grep <patron> <arch>` | Búsqueda rápida de cadenas en archivos con número de línea |
| `head` / `tail` / `wc` | Visualización de inicio/fin de archivo y conteo de líneas, palabras y bytes |
| `cp` / `mv` / `touch` | Copiado, movimiento/renombrado y creación de archivos vacíos |
| `echo <txt> [> arch]` | Impresión de texto o redirección directa a archivo |
| `<cmd> > <archivo>` | Redirección universal de E/S silenciando pantalla hacia RamFS |
| `hexdump` / `xxd` | Volcado canónico en formato hexadecimal y ASCII |
| `reboot` / `poweroff` | Reinicio físico mediante el controlador 8042 o apagado ACPI |
| `status` / `mem` | Inspección de IP/MAC y registros de control `CR0`, `CR3`, `CR4` y `RSP` |
| `heap` / `pmm` / `vmm` | Auto-tests y telemetría de memoria dinámica, marcos físicos y paginación |
| `ping <ip>` / `dns` | Ping ICMP a IPv4 y resolución de nombres mediante servidor DNS |
| `curl <host> [pt] [r]` | Cliente HTTP/1.1 con resolución DNS dinámica y cabeceras dinámicas |
| `src_ls` / `src_cat` | Inspección del código fuente del kernel empaquetado en disco (`SRCFS`) |
| `myos <mision>` | Invocación del Agente IA ReAct con memoria episódica persistente |

---

## 🎮 Demostración del Agente Autónomo

### Lectura de archivos por IA:
```text
myos> agent lee el archivo /notes.txt y dime cual es el objetivo de MYOS
[AGENTE PLAN]: CMD: cat /notes.txt
>> [MYOS BARE-METAL EXEC]: Ejecutando 'cat /notes.txt' en el hardware...
[CONTENIDO DE /notes.txt]:
MYOS Objetivo: Proporcionar a una IA un entorno informatico propio y modificable en bare-metal.
>> [FEEDBACK A LA IA]: Enviando telemetria obtenida al agente...
[AGENTE DICTAMEN FINAL]:
MYOS establece un entorno bare-metal dedicado y modificable que permite a la IA operar con control directo sobre el hardware.
```

### Comprobación de red y persistencia autónoma:
```text
myos> agent haz un ping al gateway y guarda el resultado en /ping.log
>> [PASO 1 | ACCION IA]: Ejecutando 'ping 10.0.2.2' en el hardware...
Reply from 10.0.2.2: bytes=32 ttl=255 seq=1 time~126us
>> [PASO 2 | ACCION IA]: Ejecutando 'write /ping.log Ping exitoso: respuesta ICMP recibida...'
Archivo '/ping.log' escrito con exito.
[AGENTE DICTAMEN FINAL]:
Misión completada. El ping a 10.0.2.2 se ejecutó y el resultado fue persistido correctamente en /ping.log.
```

---

## 🗺️ Hoja de Ruta

- [x] Arranque x86_64 Long Mode & Paginación 4 KiB con protecciones hardware (NX / CR0.WP)
- [x] Enumeración PCI y Drivers VirtIO (`virtio-net` y `virtio-blk` persistente)
- [x] Pila de Red Completa desde Cero (Ethernet, ARP, IPv4, ICMP, TCP, UDP, DNS)
- [x] Cliente HTTP/1.1 y Parser JSON Bare-Metal
- [x] Asignador de Memoria Dinámica KHeap alineado a 16 bytes con expansión respaldada por VMM/PMM
- [x] Sistema de Archivos RamFS Persistente con Dirty-Track (MYOSFS01 en disco)
- [x] Multitarea Cooperativa: Planificador Round-Robin, KThreads, Demonios y Control de Trabajos (`spawn`, `kill`)
- [x] Suite de Coreutils Unix/DOS, Motor Readline Bare-Metal y Editor Nano en pantalla completa
- [x] Agente Autónomo ReAct con memoria contextual acumulativa y enlace LAN (`llama-server`)
- [x] Carga de fuentes del kernel en disco (`SRCFS`) y bucle de modificación autónoma (*The Singularity Loop v2*)
- [x] Consolidación Arquitectónica: Rollback atómico, recuperación de mutexes huérfanos y Sandboxing en Ring 0
- [ ] Portabilidad Multi-Arquitectura ARM64 (Raspberry Pi 4)

---

## 📄 Licencia

Este proyecto está bajo la Licencia **MIT**. Consulta el archivo [LICENSE](LICENSE) para más detalles.
