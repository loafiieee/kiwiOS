#pragma once
#include <stdint.h>
#include <stddef.h>

// Standard CRC-32 (IEEE 802.3 / Ethernet) polynomial 0xEDB88320.
// Initial value 0xFFFFFFFF, final xor 0xFFFFFFFF.
uint32_t crc32_ieee(const void* data, size_t len);
