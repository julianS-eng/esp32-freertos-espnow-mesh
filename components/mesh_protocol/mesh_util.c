/*
 * SPDX-License-Identifier: MIT
 */
#include "mesh_util.h"

#include <stdio.h>
#include <string.h>

#include "mesh_crc.h"
#include "mesh_protocol.h"

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool mesh_parse_hex(const char *hex, uint8_t *out, size_t out_len)
{
    if (hex == NULL || out == NULL || strlen(hex) != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool mesh_parse_mac(const char *str, uint8_t mac[MESH_MAC_LEN])
{
    if (str == NULL || mac == NULL || strlen(str) != 17) {
        return false;
    }
    for (int i = 0; i < MESH_MAC_LEN; i++) {
        const char *p = str + 3 * i;
        const int hi = hex_nibble(p[0]);
        const int lo = hex_nibble(p[1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        if (i < MESH_MAC_LEN - 1 && p[2] != ':' && p[2] != '-') {
            return false;
        }
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void mesh_format_mac(const uint8_t mac[MESH_MAC_LEN], char out[MESH_MAC_STR_LEN])
{
    snprintf(out, MESH_MAC_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
}

bool mesh_mac_is_broadcast(const uint8_t mac[MESH_MAC_LEN])
{
    for (int i = 0; i < MESH_MAC_LEN; i++) {
        if (mac[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

uint16_t mesh_node_id_from_mac(const uint8_t mac[MESH_MAC_LEN])
{
    uint16_t id = mesh_crc16(mac, MESH_MAC_LEN);
    if (id == MESH_GATEWAY_NODE_ID || id == MESH_BROADCAST_NODE_ID) {
        id = 0x0001;
    }
    return id;
}
