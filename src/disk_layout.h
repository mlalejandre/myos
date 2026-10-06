#ifndef SOMA_DISK_LAYOUT_H
#define SOMA_DISK_LAYOUT_H

/* ====================================================================
 * MAPA FÍSICO DE DISCO SOMA (Sectores LBA de 512 bytes en virtio-blk)
 * Capacidad total estándar: 16 MiB (32.768 sectores)
 * ==================================================================== */

/* Zona 1: Buzón y Puente Host-Bridge (Singularity Loop) */
#define SOMA_LBA_MB_START       1      /* LBA 1..16: Buzón de parches (8 KiB) */
#define SOMA_LBA_MB_SECTORS     16
#define SOMA_LBA_RESULT_START   20     /* LBA 20..23: Resultado del host (2 KiB) */
#define SOMA_LBA_RESULT_SECTORS 4
#define SOMA_LBA_BOOT_GATE      24     /* LBA 24: Flag de prueba canaria */
#define SOMA_LBA_RESUME_FLAG    25     /* LBA 25: Flag de reanudación atómica */

/* Zona 2: Fuentes del Sistema (SRCFS) */
#define SOMA_LBA_SRCFS_INDEX    256    /* LBA 256..263: Índice de archivos (8 sectores) */
#define SOMA_LBA_SRCFS_DATA     264    /* LBA 264..1023: Código fuente empaquetado */
#define SOMA_LBA_SRCFS_LIMIT    1024

/* Zona 3: Sistema de Archivos Persistente (RamFS SOMAFS01) */
#define SOMA_LBA_FS_SUPER       1024   /* Superbloque del VFS */
#define SOMA_LBA_FS_DATA        1025   /* Inodos y bloques de datos (1025..2047) */

/* Zona 4: Scratch del Sistema y Zona de Usuario */
#define SOMA_LBA_SCRATCH_MIN    2048   /* LBA 2048..2055: Scratch para tests canarios */
#define SOMA_LBA_USER_MIN       2056   /* LBA >= 2056: Libre para el usuario/IA */

/* Zona 5: Checkpoints de Hardware */
#define SOMA_LBA_CHECKPOINT     4095   /* LBA 4095..4400: Checkpoint de respaldo RamFS */
#define SOMA_LBA_CHECKPOINT_END 4400

#endif