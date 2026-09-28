/*
 * SPDX-License-Identifier: MIT
 *
 * mesh_util.h - Small, pure helpers shared by both firmwares (key and MAC
 * parsing, formatting). Kept here so they are covered by the host tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_MAC_LEN 6
#define MESH_MAC_STR_LEN 18 /* "aa:bb:cc:dd:ee:ff" + NUL */
#define MESH_KEY_LEN 16     /* ESP_NOW_KEY_LEN */

/**
 * Parse exactly 2 * @p out_len hexadecimal characters (no separators) into bytes.
 * @return true on success; false on bad length or non-hex characters.
 */
bool mesh_parse_hex(const char *hex, uint8_t *out, size_t out_len);

/** Parse "aa:bb:cc:dd:ee:ff" (also accepts '-' separators, any case). */
bool mesh_parse_mac(const char *str, uint8_t mac[MESH_MAC_LEN]);

/** Format a MAC as lowercase "aa:bb:cc:dd:ee:ff". @p out must hold MESH_MAC_STR_LEN bytes. */
void mesh_format_mac(const uint8_t mac[MESH_MAC_LEN], char out[MESH_MAC_STR_LEN]);

/** True for ff:ff:ff:ff:ff:ff. */
bool mesh_mac_is_broadcast(const uint8_t mac[MESH_MAC_LEN]);

/** Derive a 16-bit node id from a MAC (CRC-16 of the MAC; never 0x0000 nor 0xFFFF). */
uint16_t mesh_node_id_from_mac(const uint8_t mac[MESH_MAC_LEN]);

/** Pack/format firmware versions as (major << 16) | (minor << 8) | patch. */
static inline uint32_t mesh_fw_version(uint8_t major, uint8_t minor, uint8_t patch)
{
    return ((uint32_t)major << 16) | ((uint32_t)minor << 8) | patch;
}

#ifdef __cplusplus
}
#endif
