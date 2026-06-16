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
    bool fixed_root_dir;
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

    (void)lba;
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
    node->fixed_root_dir = false;
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
        ctx->found = true;
        return false;
    }

    ctx->slot_index++;
    return true;
}

typedef struct {
    vfs_readdir_cb cb;
    void* user;
    uint32_t parent_ino;
    uint32_t slot_index;
} fat_readdir_ctx_t;

static bool fat_readdir_cb_wrap(const fat_dirent_info_t* ent, void* user) {
    fat_readdir_ctx_t* ctx = (fat_readdir_ctx_t*)user;
    uint32_t ino;

    if (!ctx || !ctx->cb || !ent) return false;
    ino = fat_make_ino(ent->first_cluster, ctx->parent_ino, ctx->slot_index++);
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
        .slot_index = 0u,
    };
    return fat_iter_dir(dir, fat_readdir_cb_wrap, &ctx);
}

static bool fat_vnode_stat(vnode_t* vn, vfs_stat_t* out) {
    const fat_node_t* node;

    if (!vn || !out) return false;
    node = fat_node_const(vn);
    if (!node) return false;

    memset(out, 0, sizeof(*out));
    out->type = vn->type;
    out->ino = vn->ino;
    out->size = vn->size;
    out->mode = (vn->type == VNODE_DIR) ? 0555u : 0444u;
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
    fs->root_node.fixed_root_dir = (fs->fat_type != FAT_TYPE_32);
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
    m->readonly = true;
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
