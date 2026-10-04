#ifndef MYOS_SRCFS_H
#define MYOS_SRCFS_H

#include <stdint.h>

/*
 * Mapa del disco (sectores de 512 bytes):
 *   0 .. 1023   reservado al sistema (buzon LBA 1-16, resultado LBA 20-23,
 *               indice y fuentes SRCFS en LBA 256-1023)
 *   >= 1024     libre para el usuario / la IA (sector_write)
 */
/*
 * Mapa del disco (sectores de 512 bytes):
 *   0 .. 1023   sistema (buzon, resultado, gate canary, fuentes SRCFS)
 *   1024..2047  sistema de archivos persistente RamFS (MYOSFS01)
 *   >= 2048     libre para el usuario / la IA (sector_write)
 */
#define SECTOR_USER_MIN 2048

/* Lista los fuentes empaquetados por el host en out. Devuelve n o -1. */
int srcfs_ls(char *out, uint32_t max);

/* Hasta 768 bytes de 'name' desde 'offset'. Devuelve bytes o -1. */
int srcfs_cat(const char *name, uint32_t offset, char *out, uint32_t max);

/* Lineas de 'name' que contienen 'pat', como "OFFSET: linea". */
int srcfs_grep(const char *name, const char *pat, char *out, uint32_t max);

/*
 * Herramientas de shell/agente: src_ls | src_cat f [off] | src_grep f texto.
 * Imprime por consola y rellena out. Devuelve 1 si el comando era suyo.
 */
int src_tool(const char *cmd, char *out, uint32_t max);

#endif
