#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "drivers/block/block.h"
#include "vfs/vfs.h"
#include "core/log.h"
#include "memory/heap.h"
#include "libc/string.h"

#define FAT_ATTR_READ_ONLY 0x01u
#define FAT_ATTR_HIDDEN    0x02u
#define FAT_ATTR_SYSTEM    0x04u
#define FAT_ATTR_VOLUME_ID 0x08u
#define FAT_ATTR_DIRECTORY 0x10u
#define FAT_ATTR_ARCHIVE   0x20u
#define FAT_ATTR_LFN       0x0fu

#define FAT_TYPE_12 12u
#define FAT_TYPE_16 16u
#define FAT_TYPE_32 32u

#define FAT_MAX_NAME_CHARS 255u

typedef struct {
    uint32_t first_cluster;
    uint8_t attr;
    bool fixed_root_dir;
    bool has_dirent;
    uint32_t dirent_lba;
    uint32_t dirent_offset;
    uint64_t mtime;
    uint64_t ctime;
} fat_node_t;

typedef struct {
    block_device_t* dev;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size_bytes;
    uint32_t fat_start_sector;
    uint32_t fat_size_sectors;
    uint32_t root_dir_start_sector;
    uint32_t root_dir_sectors;
    uint32_t data_start_sector;
    uint32_t root_cluster;
    uint32_t cluster_count;
    uint32_t fat_count;
    uint64_t total_sectors;
    uint8_t fat_type;
    fat_node_t root_node;
} fat_fs_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat_dirent_disk_t;

typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t first_cluster_lo;
    uint16_t name3[2];
} fat_lfn_disk_t;

typedef struct {
    char name[FAT_MAX_NAME_CHARS + 1u];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t slot_index;
    uint32_t dirent_lba;
    uint32_t dirent_offset;
    uint64_t mtime;
    uint64_t ctime;
} fat_dirent_info_t;

typedef bool (*fat_iter_cb)(const fat_dirent_info_t* ent, void* user);

typedef struct {
    char name[FAT_MAX_NAME_CHARS + 1u];
    uint8_t checksum;
    uint8_t next_ord;
    bool active;
} fat_lfn_state_t;

static uint64_t u64_min(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void wr32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

static char ascii_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static bool is_pow2_u32(uint32_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

static uint32_t fat_days_in_month(uint32_t year, uint32_t month) {
    static const uint8_t k_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t days;

    if (month < 1u || month > 12u) return 0u;
    days = k_days[month - 1u];
    if (month == 2u) {
        bool leap = ((year % 4u) == 0u) && (((year % 100u) != 0u) || ((year % 400u) == 0u));
        if (leap) days = 29u;
    }
    return days;
}

static int64_t days_from_civil(int64_t year, uint32_t month, uint32_t day) {
    int32_t shifted_month;

    year -= (month <= 2u) ? 1 : 0;
    shifted_month = (int32_t)month + (month > 2u ? -3 : 9);

    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t yoe = (uint32_t)(year - era * 400);
    const uint32_t doy = (153u * (uint32_t)shifted_month + 2u) / 5u + day - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static uint64_t fat_datetime_to_unix(uint16_t date, uint16_t time) {
    uint32_t day = date & 0x1fu;
    uint32_t month = (date >> 5) & 0x0fu;
    uint32_t year = 1980u + ((date >> 9) & 0x7fu);
    uint32_t second = (time & 0x1fu) * 2u;
    uint32_t minute = (time >> 5) & 0x3fu;
    uint32_t hour = (time >> 11) & 0x1fu;

    if (month < 1u || month > 12u) return 0u;
    if (day < 1u || day > fat_days_in_month(year, month)) return 0u;
    if (minute > 59u || second > 59u || hour > 23u) return 0u;

    return (uint64_t)days_from_civil((int64_t)year, month, day) * 86400u +
           (uint64_t)hour * 3600u +
           (uint64_t)minute * 60u +
           second;
}

static bool fat_name_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (ascii_upper(*a) != ascii_upper(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static bool fat_is_dot_name(const char* name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool fat_short_name_char_allowed(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;

    switch (c) {
        case '$':
        case '%':
        case '\'':
        case '-':
        case '_':
        case '@':
        case '~':
        case '`':
        case '!':
        case '(':
        case ')':
        case '{':
        case '}':
        case '^':
        case '#':
        case '&':
            return true;
        default:
            return false;
    }
}

static bool fat_make_short_name(const char* name, uint8_t out[11]) {
    uint32_t base_len = 0u;
    uint32_t ext_len = 0u;
    bool in_ext = false;

    if (!name || !out || name[0] == 0 || fat_is_dot_name(name)) return false;

    memset(out, ' ', 11u);
    for (uint32_t i = 0u; name[i] != 0; i++) {
        char ch = ascii_upper(name[i]);

        if (ch == '.') {
            if (in_ext || base_len == 0u) return false;
            in_ext = true;
            continue;
        }

        if (!fat_short_name_char_allowed(ch)) return false;

        if (in_ext) {
            if (ext_len >= 3u) return false;
            out[8u + ext_len++] = (uint8_t)ch;
        } else {
            if (base_len >= 8u) return false;
            out[base_len++] = (uint8_t)ch;
        }
    }

    return base_len != 0u;
}

static bool fat_cluster_valid(const fat_fs_t* fs, uint32_t cluster) {
    if (!fs) return false;
    return cluster >= 2u && cluster < (fs->cluster_count + 2u);
}

static uint32_t fat_cluster_first_sector(const fat_fs_t* fs, uint32_t cluster) {
    return fs->data_start_sector + (cluster - 2u) * fs->sectors_per_cluster;
}

static fat_node_t* fat_node(vnode_t* vn) {
    return (fat_node_t*)vn->fs_private;
}

static const fat_node_t* fat_node_const(const vnode_t* vn) {
    return (const fat_node_t*)vn->fs_private;
}

static uint32_t fat_make_ino(uint32_t first_cluster, uint32_t parent_ino, uint32_t slot_index) {
    if (first_cluster >= 2u) {
        return first_cluster;
    }
    return 0x80000000u ^ (parent_ino * 131u) ^ slot_index;
}

static bool fat_read_dev_sector(block_device_t* dev, uint32_t lba, void* out, uint32_t sector_size) {
    if (!dev || !dev->read || !out) return false;
    if (dev->sector_size != sector_size) return false;
    return dev->read(dev, lba, 1u, out);
}

static bool fat_read_sector(const fat_fs_t* fs, uint32_t lba, void* out) {
    if (!fs) return false;
    return fat_read_dev_sector(fs->dev, lba, out, fs->bytes_per_sector);
}

static bool fat_write_dev_sector(block_device_t* dev, uint32_t lba, const void* in, uint32_t sector_size) {
    if (!dev || !dev->write || !in) return false;
    if (dev->sector_size != sector_size) return false;
    return dev->write(dev, lba, 1u, in);
}

static bool fat_write_sector(const fat_fs_t* fs, uint32_t lba, const void* in) {
    if (!fs) return false;
    return fat_write_dev_sector(fs->dev, lba, in, fs->bytes_per_sector);
}

static bool fat_flush(const fat_fs_t* fs) {
    if (!fs || !fs->dev) return false;
    if (!fs->dev->flush) return true;
    return fs->dev->flush(fs->dev);
}

static uint8_t fat_short_checksum(const uint8_t name[11]) {
    uint8_t sum = 0u;
    for (uint32_t i = 0u; i < 11u; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + name[i]);
    }
    return sum;
}

static void fat_lfn_reset(fat_lfn_state_t* st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
}

static bool fat_lfn_store_code_unit(char* dst, uint32_t idx, uint16_t code_unit) {
    if (idx >= FAT_MAX_NAME_CHARS) return false;
    if (code_unit == 0x0000u) {
        dst[idx] = 0;
        return true;
    }
    if (code_unit == 0xffffu) {
        return true;
    }
    dst[idx] = (code_unit <= 0x7fu) ? (char)code_unit : '?';
    return true;
}

static bool fat_lfn_append_chunk(fat_lfn_state_t* st, const fat_lfn_disk_t* lfn) {
    uint32_t ord = (uint32_t)(lfn->order & 0x1fu);
    uint32_t base = (ord - 1u) * 13u;
    uint32_t idx = base;

    for (uint32_t i = 0u; i < 5u; i++, idx++) {
        if (!fat_lfn_store_code_unit(st->name, idx, lfn->name1[i])) return false;
    }
    for (uint32_t i = 0u; i < 6u; i++, idx++) {
        if (!fat_lfn_store_code_unit(st->name, idx, lfn->name2[i])) return false;
    }
    for (uint32_t i = 0u; i < 2u; i++, idx++) {
        if (!fat_lfn_store_code_unit(st->name, idx, lfn->name3[i])) return false;
    }

    st->name[FAT_MAX_NAME_CHARS] = 0;
    return true;
}

static bool fat_lfn_process(const fat_lfn_disk_t* lfn, fat_lfn_state_t* st) {
    uint8_t ord;
    bool last;

    if (!lfn || !st) return false;
    if (lfn->attr != FAT_ATTR_LFN || lfn->type != 0u || lfn->first_cluster_lo != 0u) return false;

    ord = (uint8_t)(lfn->order & 0x1fu);
    last = (lfn->order & 0x40u) != 0u;
    if (ord == 0u) return false;
    if ((uint32_t)ord * 13u > FAT_MAX_NAME_CHARS + 13u) return false;

    if (last) {
        fat_lfn_reset(st);
        st->active = true;
        st->checksum = lfn->checksum;
        st->next_ord = ord;
    }

    if (!st->active) return false;
    if (st->checksum != lfn->checksum) return false;
    if (st->next_ord != ord) return false;
    if (!fat_lfn_append_chunk(st, lfn)) return false;

    st->next_ord = (ord == 1u) ? 0u : (uint8_t)(ord - 1u);
    return true;
}

static bool fat_next_cluster(const fat_fs_t* fs, uint32_t cluster, uint32_t* out_next, bool* out_eoc);

static bool fat_read_fat_bytes(const fat_fs_t* fs, uint32_t fat_offset, uint8_t* out, uint32_t len) {
    uint32_t sector_size;
    uint32_t fat_bytes;
    uint32_t sector;
    uint32_t in_sector;
    uint32_t sectors_to_read;
    uint8_t* buf;

    if (!fs || !out || len == 0u) return false;

    sector_size = fs->bytes_per_sector;
    fat_bytes = fs->fat_size_sectors * sector_size;
    if (fat_offset + len > fat_bytes) return false;

    sector = fs->fat_start_sector + (fat_offset / sector_size);
    in_sector = fat_offset % sector_size;
    sectors_to_read = ((in_sector + len) > sector_size) ? 2u : 1u;

    buf = (uint8_t*)kmalloc((size_t)(sector_size * sectors_to_read));
    if (!buf) return false;

    if (!fs->dev->read(fs->dev, sector, sectors_to_read, buf)) {
        kfree(buf);
        return false;
    }

    memcpy(out, buf + in_sector, len);
    kfree(buf);
    return true;
}

static bool fat_write_fat_bytes(const fat_fs_t* fs, uint32_t fat_offset, const uint8_t* in, uint32_t len) {
    uint32_t sector_size;
    uint32_t fat_bytes;
    uint32_t in_sector;
    uint32_t sectors_to_touch;
    uint8_t* buf;

    if (!fs || !in || len == 0u) return false;
    if (!fs->dev || !fs->dev->read || !fs->dev->write) return false;

    sector_size = fs->bytes_per_sector;
    fat_bytes = fs->fat_size_sectors * sector_size;
    if (fat_offset + len > fat_bytes) return false;

    in_sector = fat_offset % sector_size;
    sectors_to_touch = ((in_sector + len) > sector_size) ? 2u : 1u;
    buf = (uint8_t*)kmalloc((size_t)(sector_size * sectors_to_touch));
    if (!buf) return false;

    for (uint32_t copy = 0u; copy < fs->fat_count; copy++) {
        uint32_t sector = fs->fat_start_sector + (copy * fs->fat_size_sectors) + (fat_offset / sector_size);
        if (!fs->dev->read(fs->dev, sector, sectors_to_touch, buf)) {
            kfree(buf);
            return false;
        }

        memcpy(buf + in_sector, in, len);

        if (!fs->dev->write(fs->dev, sector, sectors_to_touch, buf)) {
            kfree(buf);
            return false;
        }
    }

    kfree(buf);
    return true;
}

static uint32_t fat_eoc_value(const fat_fs_t* fs) {
    if (!fs) return 0u;
    if (fs->fat_type == FAT_TYPE_12) return 0x0fffu;
    if (fs->fat_type == FAT_TYPE_16) return 0xffffu;
    return 0x0fffffffu;
}

static bool fat_read_cluster_entry(const fat_fs_t* fs, uint32_t cluster, uint32_t* out_value) {
    if (!fs || !out_value || !fat_cluster_valid(fs, cluster)) return false;

    if (fs->fat_type == FAT_TYPE_12) {
        uint32_t fat_offset = cluster + (cluster / 2u);
        uint8_t raw[2];
        uint16_t value;

        if (!fat_read_fat_bytes(fs, fat_offset, raw, sizeof(raw))) return false;
        if ((cluster & 1u) != 0u) {
            value = (uint16_t)(((uint16_t)raw[0] >> 4) | ((uint16_t)raw[1] << 4));
        } else {
            value = (uint16_t)((uint16_t)raw[0] | (((uint16_t)raw[1] & 0x0fu) << 8));
        }

        *out_value = value & 0x0fffu;
        return true;
    }

    if (fs->fat_type == FAT_TYPE_16) {
        uint8_t raw[2];
        if (!fat_read_fat_bytes(fs, cluster * 2u, raw, sizeof(raw))) return false;
        *out_value = rd16(raw);
        return true;
    }

    if (fs->fat_type == FAT_TYPE_32) {
        uint8_t raw[4];
        if (!fat_read_fat_bytes(fs, cluster * 4u, raw, sizeof(raw))) return false;
        *out_value = rd32(raw) & 0x0fffffffu;
        return true;
    }

    return false;
}

static bool fat_write_cluster_entry(const fat_fs_t* fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fat_cluster_valid(fs, cluster)) return false;

    if (fs->fat_type == FAT_TYPE_12) {
        uint32_t fat_offset = cluster + (cluster / 2u);
        uint8_t raw[2];

        if (!fat_read_fat_bytes(fs, fat_offset, raw, sizeof(raw))) return false;
        value &= 0x0fffu;
        if ((cluster & 1u) != 0u) {
            raw[0] = (uint8_t)((raw[0] & 0x0fu) | ((value << 4) & 0xf0u));
            raw[1] = (uint8_t)((value >> 4) & 0xffu);
        } else {
            raw[0] = (uint8_t)(value & 0xffu);
            raw[1] = (uint8_t)((raw[1] & 0xf0u) | ((value >> 8) & 0x0fu));
        }

        return fat_write_fat_bytes(fs, fat_offset, raw, sizeof(raw));
    }

    if (fs->fat_type == FAT_TYPE_16) {
        uint8_t raw[2];
        wr16(raw, (uint16_t)value);
        return fat_write_fat_bytes(fs, cluster * 2u, raw, sizeof(raw));
    }

    if (fs->fat_type == FAT_TYPE_32) {
        uint8_t raw[4];
        uint32_t old_value;

        if (!fat_read_fat_bytes(fs, cluster * 4u, raw, sizeof(raw))) return false;
        old_value = rd32(raw);
        wr32(raw, (old_value & 0xf0000000u) | (value & 0x0fffffffu));
        return fat_write_fat_bytes(fs, cluster * 4u, raw, sizeof(raw));
    }

    return false;
}

static bool fat_zero_cluster(const fat_fs_t* fs, uint32_t cluster) {
    uint8_t* zero;
    uint32_t first_sector;

    if (!fs || !fat_cluster_valid(fs, cluster)) return false;

    zero = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!zero) return false;
    memset(zero, 0, fs->bytes_per_sector);

    first_sector = fat_cluster_first_sector(fs, cluster);
    for (uint32_t i = 0u; i < fs->sectors_per_cluster; i++) {
        if (!fat_write_sector(fs, first_sector + i, zero)) {
            kfree(zero);
            return false;
        }
    }

    kfree(zero);
    return true;
}

static bool fat_alloc_cluster(const fat_fs_t* fs, uint32_t* out_cluster) {
    if (!fs || !out_cluster || !fs->dev || !fs->dev->write) return false;

    for (uint32_t cluster = 2u; cluster < fs->cluster_count + 2u; cluster++) {
        uint32_t value = 0u;
        if (!fat_read_cluster_entry(fs, cluster, &value)) return false;
        if (value != 0u) continue;

        if (!fat_write_cluster_entry(fs, cluster, fat_eoc_value(fs))) return false;
        if (!fat_zero_cluster(fs, cluster)) {
            (void)fat_write_cluster_entry(fs, cluster, 0u);
            return false;
        }

        *out_cluster = cluster;
        return true;
    }

    return false;
}

static bool fat_free_chain(const fat_fs_t* fs, uint32_t start_cluster) {
    uint32_t cluster = start_cluster;

    if (!fs) return false;
    if (start_cluster < 2u) return true;
    if (!fat_cluster_valid(fs, start_cluster)) return false;

    for (uint32_t steps = 0u; steps < fs->cluster_count; steps++) {
        uint32_t next = 0u;
        bool eoc = false;

        if (!fat_next_cluster(fs, cluster, &next, &eoc)) return false;
        if (!fat_write_cluster_entry(fs, cluster, 0u)) return false;
        if (eoc) return true;

        cluster = next;
    }

    return false;
}

static bool fat_file_cluster_for_index(fat_fs_t* fs,
                                       fat_node_t* node,
                                       uint32_t cluster_index,
                                       bool allocate,
                                       uint32_t* out_cluster) {
    uint32_t cluster;

    if (!fs || !node || !out_cluster) return false;

    if (node->first_cluster < 2u) {
        if (!allocate) return false;
        if (!fat_alloc_cluster(fs, &node->first_cluster)) return false;
    }

    cluster = node->first_cluster;
    for (uint32_t i = 0u; i < cluster_index; i++) {
        uint32_t next = 0u;
        bool eoc = false;

        if (!fat_next_cluster(fs, cluster, &next, &eoc)) return false;
        if (eoc) {
            if (!allocate) return false;
            if (!fat_alloc_cluster(fs, &next)) return false;
            if (!fat_write_cluster_entry(fs, cluster, next)) {
                (void)fat_write_cluster_entry(fs, next, 0u);
                return false;
            }
        }

        cluster = next;
    }

    *out_cluster = cluster;
    return true;
}

static bool fat_next_cluster(const fat_fs_t* fs, uint32_t cluster, uint32_t* out_next, bool* out_eoc) {
    if (!fs || !out_next || !out_eoc) return false;
    if (!fat_cluster_valid(fs, cluster)) return false;

    *out_next = 0u;
    *out_eoc = false;

    if (fs->fat_type == FAT_TYPE_12) {
        uint32_t fat_offset = cluster + (cluster / 2u);
        uint8_t raw[2];
        uint16_t value;

        if (!fat_read_fat_bytes(fs, fat_offset, raw, sizeof(raw))) return false;

        if ((cluster & 1u) != 0u) {
            value = (uint16_t)(((uint16_t)raw[0] >> 4) | ((uint16_t)raw[1] << 4));
        } else {
            value = (uint16_t)((uint16_t)raw[0] | (((uint16_t)raw[1] & 0x0fu) << 8));
        }
        value &= 0x0fffu;

        if (value >= 0x0ff8u) {
            *out_eoc = true;
            return true;
        }
        if (value == 0x0ff7u || value < 2u) {
            return false;
        }

        *out_next = value;
        return fat_cluster_valid(fs, *out_next);
    }

    if (fs->fat_type == FAT_TYPE_16) {
        uint8_t raw[2];
        uint16_t value;

        if (!fat_read_fat_bytes(fs, cluster * 2u, raw, sizeof(raw))) return false;
        value = rd16(raw);

        if (value >= 0xfff8u) {
            *out_eoc = true;
            return true;
        }
        if (value == 0xfff7u || value < 2u) {
            return false;
        }

        *out_next = value;
        return fat_cluster_valid(fs, *out_next);
    }

    if (fs->fat_type == FAT_TYPE_32) {
        uint8_t raw[4];
        uint32_t value;

        if (!fat_read_fat_bytes(fs, cluster * 4u, raw, sizeof(raw))) return false;
        value = rd32(raw) & 0x0fffffffu;

        if (value >= 0x0ffffff8u) {
            *out_eoc = true;
            return true;
        }
        if (value == 0x0ffffff7u || value < 2u) {
            return false;
        }

        *out_next = value;
        return fat_cluster_valid(fs, *out_next);
    }

    return false;
}

static bool fat_nth_cluster(const fat_fs_t* fs, uint32_t start_cluster, uint32_t cluster_index, uint32_t* out_cluster) {
    uint32_t cluster;

    if (!fs || !out_cluster) return false;
    if (!fat_cluster_valid(fs, start_cluster)) return false;

    cluster = start_cluster;
    for (uint32_t i = 0u; i < cluster_index; i++) {
        uint32_t next = 0u;
        bool eoc = false;
        if (!fat_next_cluster(fs, cluster, &next, &eoc) || eoc) {
            return false;
        }
        cluster = next;
    }

    *out_cluster = cluster;
    return true;
}

static bool fat_format_short_name(const fat_dirent_disk_t* de, char out[13]) {
    char base[9];
    char ext[4];
    uint32_t base_len = 0u;
    uint32_t ext_len = 0u;
    uint32_t n = 0u;

    if (!de || !out) return false;

    for (uint32_t i = 0u; i < 8u; i++) {
        uint8_t ch = de->name[i];
        if (ch == ' ') break;
        if (i == 0u && ch == 0x05u) ch = 0xe5u;
        base[base_len++] = (char)ch;
    }
    base[base_len] = 0;

    for (uint32_t i = 0u; i < 3u; i++) {
        uint8_t ch = de->name[8u + i];
        if (ch == ' ') break;
        ext[ext_len++] = (char)ch;
    }
    ext[ext_len] = 0;

    if (base_len == 0u) return false;

    for (uint32_t i = 0u; i < base_len; i++) {
        out[n++] = ascii_upper(base[i]);
    }

    if (ext_len != 0u) {
        out[n++] = '.';
        for (uint32_t i = 0u; i < ext_len; i++) {
            out[n++] = ascii_upper(ext[i]);
        }
    }

    out[n] = 0;
    return true;
}

static bool fat_parse_dir_sector(const fat_fs_t* fs,
                                 const uint8_t* sector,
                                 uint32_t lba,
                                 uint32_t parent_ino,
                                 uint32_t* io_slot_index,
                                 fat_lfn_state_t* lfn,
                                 fat_iter_cb cb,
                                 void* user,
                                 bool* out_end,
                                 bool* out_stop) {
    uint32_t entries_per_sector;

    (void)parent_ino;

    if (!fs || !sector || !io_slot_index || !lfn || !out_end || !out_stop) return false;

    *out_end = false;
    *out_stop = false;
    entries_per_sector = fs->bytes_per_sector / 32u;

    for (uint32_t entry = 0u; entry < entries_per_sector; entry++, (*io_slot_index)++) {
        const uint8_t* raw = sector + (entry * 32u);
        const fat_dirent_disk_t* de = (const fat_dirent_disk_t*)raw;

        if (raw[0] == 0x00u) {
            fat_lfn_reset(lfn);
            *out_end = true;
            return true;
        }

        if (raw[0] == 0xe5u) {
            fat_lfn_reset(lfn);
            continue;
        }

        if (de->attr == FAT_ATTR_LFN) {
            if (!fat_lfn_process((const fat_lfn_disk_t*)raw, lfn)) {
                fat_lfn_reset(lfn);
            }
            continue;
        }

        if ((de->attr & FAT_ATTR_VOLUME_ID) != 0u) {
            fat_lfn_reset(lfn);
            continue;
        }

        fat_dirent_info_t ent;
        bool have_name = false;
        memset(&ent, 0, sizeof(ent));

        if (lfn->active && lfn->next_ord == 0u && lfn->checksum == fat_short_checksum(de->name) && lfn->name[0] != 0) {
            strcpy(ent.name, lfn->name);
            have_name = true;
        } else {
            have_name = fat_format_short_name(de, ent.name);
        }
        fat_lfn_reset(lfn);

        if (!have_name) continue;
        if (fat_is_dot_name(ent.name)) continue;

        ent.attr = de->attr;
        ent.first_cluster = ((uint32_t)rd16((const uint8_t*)&de->first_cluster_hi) << 16) |
                            (uint32_t)rd16((const uint8_t*)&de->first_cluster_lo);
        ent.size = rd32((const uint8_t*)&de->file_size);
        ent.slot_index = *io_slot_index;
        ent.dirent_lba = lba;
        ent.dirent_offset = entry * 32u;
        ent.ctime = fat_datetime_to_unix(rd16((const uint8_t*)&de->crt_date),
                                         rd16((const uint8_t*)&de->crt_time));
        ent.mtime = fat_datetime_to_unix(rd16((const uint8_t*)&de->wrt_date),
                                         rd16((const uint8_t*)&de->wrt_time));

        if (cb && !cb(&ent, user)) {
            *out_stop = true;
            return true;
        }
    }

    return true;
}

static bool fat_iter_fixed_root(vnode_t* dir, fat_iter_cb cb, void* user) {
    fat_fs_t* fs;
    uint8_t* sector;
    fat_lfn_state_t lfn;
    uint32_t slot_index = 0u;

    if (!dir || !dir->mount || !dir->mount->fs_private) return false;
    fs = (fat_fs_t*)dir->mount->fs_private;

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return false;

    fat_lfn_reset(&lfn);
    for (uint32_t i = 0u; i < fs->root_dir_sectors; i++) {
        bool end = false;
        bool stop = false;
        if (!fat_read_sector(fs, fs->root_dir_start_sector + i, sector) ||
            !fat_parse_dir_sector(fs, sector, fs->root_dir_start_sector + i, dir->ino, &slot_index, &lfn, cb, user, &end, &stop)) {
            kfree(sector);
            return false;
        }
        if (end || stop) {
            kfree(sector);
            return true;
        }
    }

    kfree(sector);
    return true;
}

static bool fat_iter_cluster_dir(vnode_t* dir, uint32_t start_cluster, fat_iter_cb cb, void* user) {
    fat_fs_t* fs;
    uint8_t* sector;
    fat_lfn_state_t lfn;
    uint32_t slot_index = 0u;
    uint32_t cluster;

    if (!dir || !dir->mount || !dir->mount->fs_private) return false;
    fs = (fat_fs_t*)dir->mount->fs_private;
    if (!fat_cluster_valid(fs, start_cluster)) return false;

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return false;

    fat_lfn_reset(&lfn);
    cluster = start_cluster;
    for (uint32_t steps = 0u; steps < fs->cluster_count; steps++) {
        uint32_t first_sector = fat_cluster_first_sector(fs, cluster);
        for (uint32_t s = 0u; s < fs->sectors_per_cluster; s++) {
            bool end = false;
            bool stop = false;
            if (!fat_read_sector(fs, first_sector + s, sector) ||
                !fat_parse_dir_sector(fs, sector, first_sector + s, dir->ino, &slot_index, &lfn, cb, user, &end, &stop)) {
                kfree(sector);
                return false;
            }
            if (end || stop) {
                kfree(sector);
                return true;
            }
        }

        {
            uint32_t next = 0u;
            bool eoc = false;
            if (!fat_next_cluster(fs, cluster, &next, &eoc)) {
                kfree(sector);
                return false;
            }
            if (eoc) {
                kfree(sector);
                return true;
            }
            cluster = next;
        }
    }

    kfree(sector);
    return false;
}

static bool fat_iter_dir(vnode_t* dir, fat_iter_cb cb, void* user) {
    const fat_node_t* node;

    if (!dir || dir->type != VNODE_DIR || !dir->mount || !dir->mount->fs_private) return false;
    node = fat_node_const(dir);
    if (!node) return false;

    if (node->fixed_root_dir) {
        return fat_iter_fixed_root(dir, cb, user);
    }

    return fat_iter_cluster_dir(dir, node->first_cluster, cb, user);
}

static bool fat_read_dirent_at(const fat_fs_t* fs, uint32_t lba, uint32_t offset, fat_dirent_disk_t* out) {
    uint8_t* sector;

    if (!fs || !out || offset + sizeof(fat_dirent_disk_t) > fs->bytes_per_sector) return false;

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return false;

    if (!fat_read_sector(fs, lba, sector)) {
        kfree(sector);
        return false;
    }

    memcpy(out, sector + offset, sizeof(*out));
    kfree(sector);
    return true;
}

static bool fat_write_dirent_at(const fat_fs_t* fs, uint32_t lba, uint32_t offset, const fat_dirent_disk_t* in) {
    uint8_t* sector;

    if (!fs || !in || offset + sizeof(fat_dirent_disk_t) > fs->bytes_per_sector) return false;

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return false;

    if (!fat_read_sector(fs, lba, sector)) {
        kfree(sector);
        return false;
    }

    memcpy(sector + offset, in, sizeof(*in));

    if (!fat_write_sector(fs, lba, sector)) {
        kfree(sector);
        return false;
    }

    kfree(sector);
    return true;
}

static bool fat_update_node_dirent(vnode_t* vn) {
    fat_fs_t* fs;
    fat_node_t* node;
    fat_dirent_disk_t de;
    uint32_t first_cluster;

    if (!vn || !vn->mount || !vn->mount->fs_private) return false;
    fs = (fat_fs_t*)vn->mount->fs_private;
    node = fat_node(vn);
    if (!node || !node->has_dirent) return false;

    if (!fat_read_dirent_at(fs, node->dirent_lba, node->dirent_offset, &de)) return false;

    first_cluster = node->first_cluster >= 2u ? node->first_cluster : 0u;
    wr16((uint8_t*)&de.first_cluster_hi, (uint16_t)(first_cluster >> 16));
    wr16((uint8_t*)&de.first_cluster_lo, (uint16_t)(first_cluster & 0xffffu));
    wr32((uint8_t*)&de.file_size, vn->type == VNODE_FILE ? (uint32_t)vn->size : 0u);

    return fat_write_dirent_at(fs, node->dirent_lba, node->dirent_offset, &de);
}

static void fat_fill_dirent(fat_dirent_disk_t* de, const uint8_t short_name[11], uint8_t attr, uint32_t first_cluster, uint32_t size) {
    memset(de, 0, sizeof(*de));
    memcpy(de->name, short_name, 11u);
    de->attr = attr;
    wr16((uint8_t*)&de->first_cluster_hi, (uint16_t)(first_cluster >> 16));
    wr16((uint8_t*)&de->first_cluster_lo, (uint16_t)(first_cluster & 0xffffu));
    wr32((uint8_t*)&de->file_size, size);
}

static bool fat_dirent_slot_is_free(uint8_t first_byte) {
    return first_byte == 0x00u || first_byte == 0xe5u;
}

static bool fat_find_free_dir_slot(vnode_t* dir, uint32_t* out_lba, uint32_t* out_offset, uint32_t* out_slot_index) {
    fat_fs_t* fs;
    const fat_node_t* node;
    uint8_t* sector;
    uint32_t slot_index = 0u;

    if (!dir || !out_lba || !out_offset || !out_slot_index || dir->type != VNODE_DIR) return false;
    if (!dir->mount || !dir->mount->fs_private) return false;

    fs = (fat_fs_t*)dir->mount->fs_private;
    node = fat_node_const(dir);
    if (!node) return false;

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return false;

    if (node->fixed_root_dir) {
        for (uint32_t s = 0u; s < fs->root_dir_sectors; s++) {
            uint32_t lba = fs->root_dir_start_sector + s;
            if (!fat_read_sector(fs, lba, sector)) {
                kfree(sector);
                return false;
            }
            for (uint32_t off = 0u; off < fs->bytes_per_sector; off += 32u, slot_index++) {
                if (fat_dirent_slot_is_free(sector[off])) {
                    *out_lba = lba;
                    *out_offset = off;
                    *out_slot_index = slot_index;
                    kfree(sector);
                    return true;
                }
            }
        }

        kfree(sector);
        return false;
    }

    if (!fat_cluster_valid(fs, node->first_cluster)) {
        kfree(sector);
        return false;
    }

    {
        uint32_t cluster = node->first_cluster;
        for (uint32_t steps = 0u; steps < fs->cluster_count; steps++) {
            uint32_t first_sector = fat_cluster_first_sector(fs, cluster);
            for (uint32_t s = 0u; s < fs->sectors_per_cluster; s++) {
                uint32_t lba = first_sector + s;
                if (!fat_read_sector(fs, lba, sector)) {
                    kfree(sector);
                    return false;
                }
                for (uint32_t off = 0u; off < fs->bytes_per_sector; off += 32u, slot_index++) {
                    if (fat_dirent_slot_is_free(sector[off])) {
                        *out_lba = lba;
                        *out_offset = off;
                        *out_slot_index = slot_index;
                        kfree(sector);
                        return true;
                    }
                }
            }

            uint32_t next = 0u;
            bool eoc = false;
            if (!fat_next_cluster(fs, cluster, &next, &eoc)) {
                kfree(sector);
                return false;
            }
            if (eoc) {
                uint32_t new_cluster = 0u;
                if (!fat_alloc_cluster(fs, &new_cluster)) {
                    kfree(sector);
                    return false;
                }
                if (!fat_write_cluster_entry(fs, cluster, new_cluster)) {
                    (void)fat_write_cluster_entry(fs, new_cluster, 0u);
                    kfree(sector);
                    return false;
                }
                *out_lba = fat_cluster_first_sector(fs, new_cluster);
                *out_offset = 0u;
                *out_slot_index = slot_index;
                kfree(sector);
                return true;
            }
            cluster = next;
        }
    }

    kfree(sector);
    return false;
}

static bool fat_build_child_vnode(vnode_t* dir, const fat_dirent_info_t* ent, uint32_t slot_index, vnode_t** out) {
    vnode_t* vn;
    fat_node_t* node;

    if (!dir || !ent || !out) return false;
    *out = NULL;

    vn = (vnode_t*)kmalloc(sizeof(vnode_t));
    node = (fat_node_t*)kmalloc(sizeof(fat_node_t));
    if (!vn || !node) {
        if (vn) kfree(vn);
        if (node) kfree(node);
        return false;
    }

    memset(vn, 0, sizeof(*vn));
    memset(node, 0, sizeof(*node));

    node->first_cluster = ent->first_cluster;
    node->attr = ent->attr;
    node->fixed_root_dir = false;
    node->has_dirent = true;
    node->dirent_lba = ent->dirent_lba;
    node->dirent_offset = ent->dirent_offset;
    node->mtime = ent->mtime;
    node->ctime = ent->ctime;

    vn->mount = dir->mount;
    vn->type = ((ent->attr & FAT_ATTR_DIRECTORY) != 0u) ? VNODE_DIR : VNODE_FILE;
    vn->ino = fat_make_ino(ent->first_cluster, dir->ino, slot_index);
    vn->size = ent->size;
    vn->ops = dir->ops;
    vn->fs_private = node;

    *out = vn;
    return true;
}

typedef struct {
    const char* name;
    fat_dirent_info_t ent;
    uint32_t slot_index;
    bool found;
} fat_lookup_ctx_t;

static bool fat_lookup_cb(const fat_dirent_info_t* ent, void* user) {
    fat_lookup_ctx_t* ctx = (fat_lookup_ctx_t*)user;
    if (!ctx || !ent) return true;

    if (fat_name_equal(ent->name, ctx->name)) {
        ctx->ent = *ent;
        ctx->slot_index = ent->slot_index;
        ctx->found = true;
        return false;
    }
    return true;
}

typedef struct {
    vfs_readdir_cb cb;
    void* user;
    uint32_t parent_ino;
} fat_readdir_ctx_t;

static bool fat_readdir_cb_wrap(const fat_dirent_info_t* ent, void* user) {
    fat_readdir_ctx_t* ctx = (fat_readdir_ctx_t*)user;
    uint32_t ino;

    if (!ctx || !ctx->cb || !ent) return false;
    ino = fat_make_ino(ent->first_cluster, ctx->parent_ino, ent->slot_index);
    return ctx->cb(ent->name, ino, ctx->user);
}

static bool fat_vnode_lookup(vnode_t* dir, const char* name, vnode_t** out) {
    fat_lookup_ctx_t ctx;

    if (!dir || !name || !out) return false;
    *out = NULL;

    memset(&ctx, 0, sizeof(ctx));
    ctx.name = name;

    if (!fat_iter_dir(dir, fat_lookup_cb, &ctx)) return false;
    if (!ctx.found) return false;

    return fat_build_child_vnode(dir, &ctx.ent, ctx.slot_index, out);
}

static bool fat_vnode_readdir(vnode_t* dir, vfs_readdir_cb cb, void* user) {
    fat_readdir_ctx_t ctx = {
        .cb = cb,
        .user = user,
        .parent_ino = dir ? dir->ino : 0u,
    };
    return fat_iter_dir(dir, fat_readdir_cb_wrap, &ctx);
}

static bool fat_vnode_stat(vnode_t* vn, vfs_stat_t* out) {
    const fat_node_t* node;
    bool writable;

    if (!vn || !out) return false;
    node = fat_node_const(vn);
    if (!node) return false;

    memset(out, 0, sizeof(*out));
    writable = vn->mount && !vn->mount->readonly && ((node->attr & FAT_ATTR_READ_ONLY) == 0u);
    out->type = vn->type;
    out->ino = vn->ino;
    out->size = vn->size;
    out->mode = (vn->type == VNODE_DIR) ? (writable ? 0755u : 0555u) : (writable ? 0644u : 0444u);
    out->link_count = 1u;
    out->mtime = node->mtime;
    out->ctime = node->ctime;
    return true;
}

static int64_t fat_vnode_read(vnode_t* vn, uint64_t offset, void* buf, uint64_t len) {
    fat_fs_t* fs;
    const fat_node_t* node;
    uint8_t* sector;
    uint8_t* out;
    uint64_t to_read;
    uint64_t done = 0u;
    uint32_t cluster;
    uint32_t cluster_index;
    uint32_t in_cluster;

    if (!vn || !buf) return -1;
    if (len == 0u) return 0;
    if (!vn->mount || !vn->mount->fs_private) return -1;
    if (vn->type != VNODE_FILE) return -1;
    if (offset >= vn->size) return 0;

    fs = (fat_fs_t*)vn->mount->fs_private;
    node = fat_node_const(vn);
    if (!node) return -1;
    if (node->first_cluster < 2u) {
        return vn->size == 0u ? 0 : -1;
    }

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return -1;

    to_read = u64_min(len, vn->size - offset);
    out = (uint8_t*)buf;
    cluster_index = (uint32_t)(offset / fs->cluster_size_bytes);
    in_cluster = (uint32_t)(offset % fs->cluster_size_bytes);

    if (!fat_nth_cluster(fs, node->first_cluster, cluster_index, &cluster)) {
        kfree(sector);
        return -1;
    }

    while (done < to_read) {
        uint32_t first_sector;
        uint32_t sector_index;
        uint32_t sector_offset;

        if (!fat_cluster_valid(fs, cluster)) {
            kfree(sector);
            return -1;
        }

        first_sector = fat_cluster_first_sector(fs, cluster);
        sector_index = in_cluster / fs->bytes_per_sector;
        sector_offset = in_cluster % fs->bytes_per_sector;

        while (done < to_read && sector_index < fs->sectors_per_cluster) {
            uint64_t chunk;
            if (!fat_read_sector(fs, first_sector + sector_index, sector)) {
                kfree(sector);
                return -1;
            }

            chunk = u64_min(to_read - done, (uint64_t)(fs->bytes_per_sector - sector_offset));
            memcpy(out + done, sector + sector_offset, (size_t)chunk);

            done += chunk;
            in_cluster += (uint32_t)chunk;
            sector_index = in_cluster / fs->bytes_per_sector;
            sector_offset = in_cluster % fs->bytes_per_sector;
        }

        if (done >= to_read) break;

        {
            uint32_t next = 0u;
            bool eoc = false;
            if (!fat_next_cluster(fs, cluster, &next, &eoc) || eoc) {
                kfree(sector);
                return -1;
            }
            cluster = next;
        }
        in_cluster = 0u;
    }

    kfree(sector);
    return (int64_t)done;
}

static bool fat_vnode_create(vnode_t* dir, const char* name, uint32_t mode, vnode_t** out) {
    fat_fs_t* fs;
    uint8_t short_name[11];
    fat_dirent_disk_t de;
    fat_dirent_info_t ent;
    vnode_t* existing = NULL;
    uint32_t lba = 0u;
    uint32_t offset = 0u;
    uint32_t slot_index = 0u;

    (void)mode;

    if (out) *out = NULL;
    if (!dir || !name || dir->type != VNODE_DIR || !dir->mount || !dir->mount->fs_private) return false;
    if (dir->mount->readonly) return false;
    if (!fat_make_short_name(name, short_name)) return false;
    if (fat_vnode_lookup(dir, name, &existing)) {
        if (existing) vfs_vnode_put(existing);
        return false;
    }

    fs = (fat_fs_t*)dir->mount->fs_private;
    if (!fat_find_free_dir_slot(dir, &lba, &offset, &slot_index)) return false;

    fat_fill_dirent(&de, short_name, FAT_ATTR_ARCHIVE, 0u, 0u);
    if (!fat_write_dirent_at(fs, lba, offset, &de)) return false;
    if (!fat_flush(fs)) return false;

    memset(&ent, 0, sizeof(ent));
    strcpy(ent.name, name);
    ent.attr = FAT_ATTR_ARCHIVE;
    ent.first_cluster = 0u;
    ent.size = 0u;
    ent.slot_index = slot_index;
    ent.dirent_lba = lba;
    ent.dirent_offset = offset;

    if (out) {
        return fat_build_child_vnode(dir, &ent, slot_index, out);
    }

    return true;
}

static int64_t fat_vnode_write(vnode_t* vn, uint64_t offset, const void* buf, uint64_t len) {
    fat_fs_t* fs;
    fat_node_t* node;
    const uint8_t* in = (const uint8_t*)buf;
    uint8_t* sector;
    uint64_t done = 0u;
    uint64_t end_offset;
    uint32_t original_first_cluster;

    if (!vn || !buf || !vn->mount || !vn->mount->fs_private) return -1;
    if (vn->mount->readonly || vn->type != VNODE_FILE) return -1;
    if (len == 0u) return 0;
    if (offset > 0xffffffffULL || len > 0xffffffffULL || offset + len < offset || offset + len > 0xffffffffULL) return -1;
    if (offset > vn->size) return -1;

    fs = (fat_fs_t*)vn->mount->fs_private;
    node = fat_node(vn);
    if (!node || (node->attr & FAT_ATTR_READ_ONLY) != 0u) return -1;

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return -1;

    original_first_cluster = node->first_cluster;
    end_offset = offset + len;

    while (done < len) {
        uint64_t file_pos = offset + done;
        uint32_t cluster_index = (uint32_t)(file_pos / fs->cluster_size_bytes);
        uint32_t in_cluster = (uint32_t)(file_pos % fs->cluster_size_bytes);
        uint32_t cluster = 0u;
        uint32_t first_sector;
        uint32_t sector_index;
        uint32_t sector_offset;

        if (!fat_file_cluster_for_index(fs, node, cluster_index, true, &cluster)) {
            if (original_first_cluster < 2u && node->first_cluster >= 2u) {
                (void)fat_free_chain(fs, node->first_cluster);
                node->first_cluster = original_first_cluster;
            }
            kfree(sector);
            return -1;
        }

        first_sector = fat_cluster_first_sector(fs, cluster);
        sector_index = in_cluster / fs->bytes_per_sector;
        sector_offset = in_cluster % fs->bytes_per_sector;

        while (done < len && sector_index < fs->sectors_per_cluster) {
            uint64_t chunk = u64_min(len - done, (uint64_t)(fs->bytes_per_sector - sector_offset));
            uint32_t lba = first_sector + sector_index;

            if (chunk != fs->bytes_per_sector || sector_offset != 0u) {
                if (!fat_read_sector(fs, lba, sector)) {
                    kfree(sector);
                    return -1;
                }
            }

            if (chunk == fs->bytes_per_sector && sector_offset == 0u) {
                memcpy(sector, in + done, fs->bytes_per_sector);
            } else {
                memcpy(sector + sector_offset, in + done, (size_t)chunk);
            }

            if (!fat_write_sector(fs, lba, sector)) {
                kfree(sector);
                return -1;
            }

            done += chunk;
            in_cluster += (uint32_t)chunk;
            sector_index = in_cluster / fs->bytes_per_sector;
            sector_offset = in_cluster % fs->bytes_per_sector;
        }
    }

    if (end_offset > vn->size) {
        vn->size = end_offset;
    }

    if (!fat_update_node_dirent(vn) || !fat_flush(fs)) {
        kfree(sector);
        return -1;
    }

    kfree(sector);
    return (int64_t)done;
}

static bool fat_vnode_truncate(vnode_t* vn, uint64_t size) {
    fat_fs_t* fs;
    fat_node_t* node;
    uint32_t old_first;

    if (!vn || !vn->mount || !vn->mount->fs_private || vn->type != VNODE_FILE) return false;
    if (vn->mount->readonly) return false;
    if (size == vn->size) return true;

    /* Phase 18 starts with the O_TRUNC path used by copy/write tools. */
    if (size != 0u) return false;

    fs = (fat_fs_t*)vn->mount->fs_private;
    node = fat_node(vn);
    if (!node || (node->attr & FAT_ATTR_READ_ONLY) != 0u) return false;

    old_first = node->first_cluster;
    node->first_cluster = 0u;
    vn->size = 0u;

    if (!fat_update_node_dirent(vn)) {
        node->first_cluster = old_first;
        return false;
    }

    if (old_first >= 2u && !fat_free_chain(fs, old_first)) return false;
    return fat_flush(fs);
}

static bool fat_write_initial_dir_cluster(const fat_fs_t* fs, uint32_t cluster, uint32_t parent_cluster) {
    uint8_t dot_name[11];
    uint8_t dotdot_name[11];
    uint8_t* sector;
    fat_dirent_disk_t dot;
    fat_dirent_disk_t dotdot;
    bool ok;

    if (!fs || !fat_cluster_valid(fs, cluster)) return false;

    memset(dot_name, ' ', sizeof(dot_name));
    memset(dotdot_name, ' ', sizeof(dotdot_name));
    dot_name[0] = '.';
    dotdot_name[0] = '.';
    dotdot_name[1] = '.';

    fat_fill_dirent(&dot, dot_name, FAT_ATTR_DIRECTORY, cluster, 0u);
    fat_fill_dirent(&dotdot, dotdot_name, FAT_ATTR_DIRECTORY, parent_cluster, 0u);

    sector = (uint8_t*)kmalloc(fs->bytes_per_sector);
    if (!sector) return false;

    memset(sector, 0, fs->bytes_per_sector);
    memcpy(sector, &dot, sizeof(dot));
    memcpy(sector + sizeof(dot), &dotdot, sizeof(dotdot));

    ok = fat_write_sector(fs, fat_cluster_first_sector(fs, cluster), sector);
    kfree(sector);
    return ok;
}

static bool fat_vnode_mkdir(vnode_t* dir, const char* name, uint32_t mode) {
    fat_fs_t* fs;
    const fat_node_t* parent_node;
    uint8_t short_name[11];
    fat_dirent_disk_t de;
    vnode_t* existing = NULL;
    uint32_t lba = 0u;
    uint32_t offset = 0u;
    uint32_t slot_index = 0u;
    uint32_t cluster = 0u;
    uint32_t parent_cluster = 0u;

    (void)mode;

    if (!dir || !name || dir->type != VNODE_DIR || !dir->mount || !dir->mount->fs_private) return false;
    if (dir->mount->readonly) return false;
    if (!fat_make_short_name(name, short_name)) return false;
    if (fat_vnode_lookup(dir, name, &existing)) {
        if (existing) vfs_vnode_put(existing);
        return false;
    }

    fs = (fat_fs_t*)dir->mount->fs_private;
    parent_node = fat_node_const(dir);
    if (!parent_node) return false;

    if (!fat_find_free_dir_slot(dir, &lba, &offset, &slot_index)) return false;
    if (!fat_alloc_cluster(fs, &cluster)) return false;

    parent_cluster = parent_node->fixed_root_dir ? 0u : parent_node->first_cluster;
    if (!fat_write_initial_dir_cluster(fs, cluster, parent_cluster)) {
        (void)fat_write_cluster_entry(fs, cluster, 0u);
        return false;
    }

    fat_fill_dirent(&de, short_name, FAT_ATTR_DIRECTORY, cluster, 0u);
    if (!fat_write_dirent_at(fs, lba, offset, &de)) {
        (void)fat_free_chain(fs, cluster);
        return false;
    }

    return fat_flush(fs);
}

typedef struct {
    bool empty;
} fat_empty_ctx_t;

static bool fat_empty_cb(const fat_dirent_info_t* ent, void* user) {
    fat_empty_ctx_t* ctx = (fat_empty_ctx_t*)user;
    if (!ctx || !ent) return false;
    ctx->empty = false;
    return false;
}

static bool fat_directory_is_empty(vnode_t* dir) {
    fat_empty_ctx_t ctx = { .empty = true };
    if (!dir || dir->type != VNODE_DIR) return false;
    if (!fat_iter_dir(dir, fat_empty_cb, &ctx)) return false;
    return ctx.empty;
}

static bool fat_vnode_unlink(vnode_t* dir, const char* name) {
    fat_fs_t* fs;
    vnode_t* victim = NULL;
    fat_node_t* victim_node;
    fat_dirent_disk_t de;
    uint8_t short_name[11];
    uint32_t first_cluster;

    if (!dir || !name || dir->type != VNODE_DIR || !dir->mount || !dir->mount->fs_private) return false;
    if (dir->mount->readonly) return false;
    if (!fat_make_short_name(name, short_name)) return false;

    fs = (fat_fs_t*)dir->mount->fs_private;
    if (!fat_vnode_lookup(dir, name, &victim) || !victim) return false;

    victim_node = fat_node(victim);
    if (!victim_node || !victim_node->has_dirent) {
        vfs_vnode_put(victim);
        return false;
    }
    if ((victim_node->attr & FAT_ATTR_READ_ONLY) != 0u) {
        vfs_vnode_put(victim);
        return false;
    }

    if (victim->type == VNODE_DIR && !fat_directory_is_empty(victim)) {
        vfs_vnode_put(victim);
        return false;
    }

    if (!fat_read_dirent_at(fs, victim_node->dirent_lba, victim_node->dirent_offset, &de)) {
        vfs_vnode_put(victim);
        return false;
    }

    if (memcmp(de.name, short_name, 11u) != 0) {
        vfs_vnode_put(victim);
        return false;
    }

    first_cluster = victim_node->first_cluster;
    de.name[0] = 0xe5u;
    if (!fat_write_dirent_at(fs, victim_node->dirent_lba, victim_node->dirent_offset, &de)) {
        vfs_vnode_put(victim);
        return false;
    }

    if (first_cluster >= 2u && !fat_free_chain(fs, first_cluster)) {
        vfs_vnode_put(victim);
        return false;
    }

    vfs_vnode_put(victim);
    return fat_flush(fs);
}

static void fat_vnode_release(vnode_t* vn) {
    fat_fs_t* fs;
    fat_node_t* node;

    if (!vn || !vn->mount || !vn->mount->fs_private || !vn->fs_private) return;

    fs = (fat_fs_t*)vn->mount->fs_private;
    node = fat_node(vn);
    if (node != &fs->root_node) {
        kfree(node);
    }
}

static const vnode_ops_t g_fat_vops = {
    .lookup = fat_vnode_lookup,
    .readdir = fat_vnode_readdir,
    .read = fat_vnode_read,
    .write = fat_vnode_write,
    .truncate = fat_vnode_truncate,
    .create = fat_vnode_create,
    .mkdir = fat_vnode_mkdir,
    .unlink = fat_vnode_unlink,
    .stat = fat_vnode_stat,
    .release = fat_vnode_release,
};

static bool fat_parse_boot_sector(block_device_t* dev, fat_fs_t* out) {
    uint8_t* bs;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint16_t fat_size_16;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint32_t root_cluster;
    uint64_t total_sectors;
    uint32_t fat_size;
    uint32_t root_dir_sectors;
    uint64_t first_data_sector;
    uint64_t data_sectors;
    uint32_t cluster_count;
    uint8_t fat_type;

    if (!dev || !out) return false;
    if (!dev->read) return false;
    if (dev->sector_size < 512u || dev->sector_size > 4096u || !is_pow2_u32(dev->sector_size)) return false;

    bs = (uint8_t*)kmalloc(dev->sector_size);
    if (!bs) return false;

    if (!fat_read_dev_sector(dev, 0u, bs, dev->sector_size)) {
        kfree(bs);
        return false;
    }

    if (bs[510] != 0x55u || bs[511] != 0xaau) {
        kfree(bs);
        return false;
    }

    bytes_per_sector = rd16(bs + 11);
    sectors_per_cluster = bs[13];
    reserved_sectors = rd16(bs + 14);
    fat_count = bs[16];
    root_entry_count = rd16(bs + 17);
    total_sectors_16 = rd16(bs + 19);
    fat_size_16 = rd16(bs + 22);
    total_sectors_32 = rd32(bs + 32);
    fat_size_32 = rd32(bs + 36);
    root_cluster = rd32(bs + 44);
    kfree(bs);

    if (bytes_per_sector != dev->sector_size) return false;
    if (bytes_per_sector < 512u || bytes_per_sector > 4096u || !is_pow2_u32(bytes_per_sector)) return false;
    if (!is_pow2_u32(sectors_per_cluster) || sectors_per_cluster == 0u || sectors_per_cluster > 128u) return false;
    if (reserved_sectors == 0u || fat_count == 0u) return false;

    total_sectors = total_sectors_16 != 0u ? total_sectors_16 : total_sectors_32;
    fat_size = fat_size_16 != 0u ? fat_size_16 : fat_size_32;
    if (total_sectors == 0u || fat_size == 0u) return false;
    if (dev->total_sectors != 0u && total_sectors > dev->total_sectors) return false;

    root_dir_sectors = ((uint32_t)root_entry_count * 32u + (bytes_per_sector - 1u)) / bytes_per_sector;
    first_data_sector = (uint64_t)reserved_sectors + ((uint64_t)fat_count * fat_size) + root_dir_sectors;
    if (first_data_sector >= total_sectors) return false;

    data_sectors = total_sectors - first_data_sector;
    cluster_count = (uint32_t)(data_sectors / sectors_per_cluster);
    if (cluster_count == 0u) return false;

    if (cluster_count < 4085u) {
        fat_type = FAT_TYPE_12;
    } else if (cluster_count < 65525u) {
        fat_type = FAT_TYPE_16;
    } else {
        fat_type = FAT_TYPE_32;
    }

    if (fat_type == FAT_TYPE_32) {
        if (root_cluster < 2u) return false;
    } else {
        if (root_entry_count == 0u) return false;
        root_cluster = 0u;
    }

    memset(out, 0, sizeof(*out));
    out->dev = dev;
    out->bytes_per_sector = bytes_per_sector;
    out->sectors_per_cluster = sectors_per_cluster;
    out->cluster_size_bytes = bytes_per_sector * sectors_per_cluster;
    out->fat_start_sector = reserved_sectors;
    out->fat_size_sectors = fat_size;
    out->root_dir_start_sector = reserved_sectors + (uint32_t)fat_count * fat_size;
    out->root_dir_sectors = root_dir_sectors;
    out->data_start_sector = out->root_dir_start_sector + root_dir_sectors;
    out->root_cluster = root_cluster;
    out->cluster_count = cluster_count;
    out->fat_count = fat_count;
    out->total_sectors = total_sectors;
    out->fat_type = fat_type;

    if (fat_type == FAT_TYPE_32 && !fat_cluster_valid(out, root_cluster)) {
        return false;
    }

    return true;
}

bool fat_probe(block_device_t* dev) {
    fat_fs_t fs;
    return fat_parse_boot_sector(dev, &fs);
}

bool fat_mount(block_device_t* dev, vfs_mount_t** out_mount) {
    fat_fs_t parsed;
    fat_fs_t* fs;
    vnode_t* root;
    vfs_mount_t* m;

    if (!out_mount) return false;
    *out_mount = NULL;

    if (!fat_parse_boot_sector(dev, &parsed)) return false;

    fs = (fat_fs_t*)kmalloc(sizeof(fat_fs_t));
    root = (vnode_t*)kmalloc(sizeof(vnode_t));
    m = (vfs_mount_t*)kmalloc(sizeof(vfs_mount_t));
    if (!fs || !root || !m) {
        if (fs) kfree(fs);
        if (root) kfree(root);
        if (m) kfree(m);
        return false;
    }

    *fs = parsed;
    memset(root, 0, sizeof(*root));
    memset(m, 0, sizeof(*m));

    fs->root_node.first_cluster = fs->root_cluster;
    fs->root_node.attr = FAT_ATTR_DIRECTORY;
    fs->root_node.fixed_root_dir = (fs->fat_type != FAT_TYPE_32);
    fs->root_node.has_dirent = false;
    fs->root_node.mtime = 0u;
    fs->root_node.ctime = 0u;

    root->type = VNODE_DIR;
    root->ino = 1u;
    root->size = (fs->fat_type == FAT_TYPE_32) ? 0u : (uint64_t)fs->root_dir_sectors * fs->bytes_per_sector;
    root->ops = &g_fat_vops;
    root->mount = m;
    root->fs_private = &fs->root_node;

    m->fs_name = "fat";
    m->dev = dev;
    m->root = root;
    m->readonly = dev->write == NULL;
    m->fs_private = fs;

    *out_mount = m;

    log_infof("fat", "Mounted FAT%u on %s (bps=%u spc=%u clusters=%u)",
              (unsigned)fs->fat_type,
              dev && dev->name ? dev->name : "(noname)",
              (unsigned)fs->bytes_per_sector,
              (unsigned)fs->sectors_per_cluster,
              (unsigned)fs->cluster_count);
    return true;
}
