/* Persistent user settings — implementation. See settings.h for the design. */

#include <string.h>
#include <stdio.h>

#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"

#include "settings.h"

#define SECTORS        2
/* v2: 512-byte records (2 flash pages) — the saved-device list outgrew one
   page. 8 records per sector; the program length stays a multiple of 256.
   v1 (256-byte 'PSET') records are migrated on first load. */
#define REC_SIZE       512
#define PAGES_PER_SEC  (FLASH_SECTOR_SIZE / REC_SIZE)          /* 8  */
#define REC_MAGIC      0x50535432u                             /* 'PST2' */
#define REC_MAGIC_V1   0x50534554u                             /* 'PSET' */
#define V1_REC_SIZE    256

/* Two sectors directly below the BTstack TLV bank (which is the LAST 2
   sectors — pico-sdk PICO_FLASH_BANK default). */
#define STORE_BASE     (PICO_FLASH_SIZE_BYTES - 4u * FLASH_SECTOR_SIZE)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_len;
    uint32_t seq;
    /* payload ... crc32 in the page tail */
} rec_hdr_t;

_Static_assert(sizeof(rec_hdr_t) + sizeof(user_settings_t) + 4 <= REC_SIZE,
               "settings payload must fit one record");

static user_settings_t s_cur;         /* the RAM cache */
static int      s_last_rc;            /* last flash_safe_execute rc (diag) */
static uint32_t s_seq;                /* seq of the loaded/last-written snapshot */
static int      s_sector;             /* active sector index (0/1) */
static int      s_page;               /* next free page in the active sector */
static volatile bool s_dirty;
static volatile uint32_t s_dirty_ms;  /* debounce clock, advanced by tick */

static const uint8_t *flash_ptr(uint32_t off) {
    return (const uint8_t *)(XIP_BASE + off);
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static uint32_t page_off(int sector, int page) {
    return STORE_BASE + (uint32_t)sector * FLASH_SECTOR_SIZE
                      + (uint32_t)page * REC_SIZE;
}

/* Validate one record; returns its seq via *seq. */
static bool page_valid(int sector, int page, uint32_t *seq) {
    const uint8_t *p = flash_ptr(page_off(sector, page));
    const rec_hdr_t *h = (const rec_hdr_t *)p;
    if (h->magic != REC_MAGIC) return false;
    if (h->payload_len > REC_SIZE - sizeof(rec_hdr_t) - 4) return false;
    uint32_t stored;
    memcpy(&stored, p + REC_SIZE - 4, 4);
    if (crc32(p, REC_SIZE - 4) != stored) return false;
    *seq = h->seq;
    return true;
}

/* v1 (256-byte 'PSET') record check, for one-time migration. */
static bool v1_rec_valid(uint32_t off, uint32_t *seq, uint16_t *payload_len) {
    const uint8_t *p = flash_ptr(off);
    const rec_hdr_t *h = (const rec_hdr_t *)p;
    if (h->magic != REC_MAGIC_V1) return false;
    if (h->payload_len > V1_REC_SIZE - sizeof(rec_hdr_t) - 4) return false;
    uint32_t stored;
    memcpy(&stored, p + V1_REC_SIZE - 4, 4);
    if (crc32(p, V1_REC_SIZE - 4) != stored) return false;
    *seq = h->seq;
    *payload_len = h->payload_len;
    return true;
}

/* ---- flash write plumbing (flash_safe_execute needs a callback) ---------- */
static struct { uint32_t erase_off; uint32_t prog_off; } s_op;
static uint8_t s_pagebuf[REC_SIZE] __attribute__((aligned(4)));

static void flash_cb(void *unused) {
    (void)unused;
    if (s_op.erase_off != UINT32_MAX) {
        flash_range_erase(s_op.erase_off, FLASH_SECTOR_SIZE);
    }
    flash_range_program(s_op.prog_off, s_pagebuf, REC_SIZE);
}

static bool commit_snapshot(void) {
    /* Sector full -> ping-pong: erase the other sector, continue there. */
    bool need_erase = false;
    if (s_page >= PAGES_PER_SEC) {
        s_sector ^= 1;
        s_page = 0;
        need_erase = true;
    }

    /* Build the record. Snapshot the cache with IRQs off so a BT/USB-context
       mutation can never tear the copy. */
    memset(s_pagebuf, 0xFF, sizeof(s_pagebuf));
    rec_hdr_t *h = (rec_hdr_t *)s_pagebuf;
    h->magic = REC_MAGIC;
    h->version = SETTINGS_VERSION;
    h->payload_len = sizeof(user_settings_t);
    h->seq = ++s_seq;
    uint32_t save = save_and_disable_interrupts();
    memcpy(s_pagebuf + sizeof(rec_hdr_t), &s_cur, sizeof(user_settings_t));
    restore_interrupts(save);
    uint32_t crc = crc32(s_pagebuf, REC_SIZE - 4);
    memcpy(s_pagebuf + REC_SIZE - 4, &crc, 4);

    s_op.erase_off = need_erase ? page_off(s_sector, 0) : UINT32_MAX;
    s_op.prog_off  = page_off(s_sector, s_page);
    s_last_rc = flash_safe_execute(flash_cb, NULL, 500);
    if (s_last_rc != PICO_OK) {
        printf("[settings] flash write FAILED rc=%d (erase 0x%lx prog 0x%lx)\n",
               s_last_rc, (unsigned long)s_op.erase_off, (unsigned long)s_op.prog_off);
        s_seq--;                       /* reuse the seq next attempt */
        if (need_erase) { s_sector ^= 1; s_page = PAGES_PER_SEC; }  /* undo flip */
        return false;
    }
    s_page++;
    /* routine success — every settings change would announce itself on the
       user's console. Failures above always print, and 'R' dumps the store
       health on demand (settings_debug_dump). */
    if (s_cur.dbg_mode)
        printf("[settings] committed seq %lu (sector %d page %d)\n",
               (unsigned long)s_seq, s_sector, s_page - 1);
    return true;
}

/* ---- legacy migration (old stores inside the BTstack bank sector) -------- */
static void migrate_legacy(void) {
    const uint8_t *pg = flash_ptr(PICO_FLASH_SIZE_BYTES - FLASH_PAGE_SIZE);

    memset(&s_cur, 0, sizeof(s_cur));
    memcpy(s_cur.slot[0].mac, pg + 0, 6);
    memcpy(s_cur.slot[1].mac, pg + 6, 6);
    for (int i = 0; i < 2; i++) {
        const uint8_t *nm = pg + 16 + i * SETTINGS_NAME_LEN;
        memcpy(s_cur.slot[i].name, nm, SETTINGS_NAME_LEN);
        s_cur.slot[i].name[SETTINGS_NAME_LEN - 1] = 0;
        for (int k = 0; k < SETTINGS_NAME_LEN; k++) {
            uint8_t c = (uint8_t)s_cur.slot[i].name[k];
            if (c == 0) break;
            if (c < 0x20 || c == 0xFF) { s_cur.slot[i].name[0] = 0; break; }
        }
    }
    uint8_t slot = pg[FLASH_PAGE_SIZE - 1];              /* legacy byte at FLASH_SIZE-1 */
    s_cur.cur_slot = (slot == 2) ? 2 : 1;
    uint8_t us = pg[80];                                 /* legacy settings byte */
    /* Fresh flash: UART console defaults ON — debug-pinless boards
       (RP2350B-Plus-W) have no SWD, the UART is the only boot console. */
    /* NO USBSET_UAC1 in the default: UAC1-first as fresh default meant every
       settings wipe silently flipped the device to the UAC1 personality —
       "UAC2 broken on Windows" while the UAC2 function sat unused (config 1
       is what hosts bind). UAC2-first is the sane default; legacy hosts opt
       in via the web switch. */
    s_cur.usbset = (us == 0xFF) ? (USBSET_BOOTCONN | USBSET_PAUSEDC | USBSET_UART)
                                : (us & 0x0F);          /* rate44 defaults OFF */
    printf("[settings] migrated legacy store (slot %u, usbset 0x%02x)\n",
           s_cur.cur_slot, s_cur.usbset);
}

/* ---- public API ----------------------------------------------------------- */
void settings_init(void) {
    uint32_t best_seq = 0;
    int best_sec = -1, best_page = -1;

    for (int sec = 0; sec < SECTORS; sec++) {
        for (int pg = 0; pg < PAGES_PER_SEC; pg++) {
            uint32_t seq;
            if (page_valid(sec, pg, &seq) && seq >= best_seq) {
                best_seq = seq; best_sec = sec; best_page = pg;
            }
        }
    }

    if (best_sec < 0) {
        /* No v2 records — look for v1 (256-byte) records to migrate: same
           two sectors, 16 slots each, old magic/CRC layout. The v2 struct
           is a strict prefix-extension, so a prefix copy + zero-fill gives
           the new fields (saved list) their empty defaults. */
        uint32_t v1_best_seq = 0; uint32_t v1_best_off = 0; uint16_t v1_len = 0;
        for (int sec = 0; sec < SECTORS; sec++) {
            for (int pg = 0; pg < (int)(FLASH_SECTOR_SIZE / V1_REC_SIZE); pg++) {
                uint32_t off = STORE_BASE + (uint32_t)sec * FLASH_SECTOR_SIZE
                                          + (uint32_t)pg * V1_REC_SIZE;
                uint32_t seq; uint16_t plen;
                if (v1_rec_valid(off, &seq, &plen) && seq >= v1_best_seq) {
                    v1_best_seq = seq; v1_best_off = off; v1_len = plen;
                }
            }
        }
        if (v1_best_seq > 0) {
            memset(&s_cur, 0, sizeof(s_cur));
            size_t n = v1_len < sizeof(user_settings_t) ? v1_len : sizeof(user_settings_t);
            memcpy(&s_cur, flash_ptr(v1_best_off) + sizeof(rec_hdr_t), n);
            /* Seed the saved list from the two slots so nothing is lost. */
            for (int s = 0; s < 2; s++) {
                bool any = false, zero = true;
                for (int k = 0; k < 6; k++) { any |= s_cur.slot[s].mac[k] != 0xFF; zero &= s_cur.slot[s].mac[k] == 0; }
                if (any && !zero) settings_saved_add(s_cur.slot[s].mac, s_cur.slot[s].name);
            }
            printf("[settings] migrated v1 record seq %lu -> v2\n", (unsigned long)v1_best_seq);
        } else {
            migrate_legacy();
        }
        s_seq = 0;
        s_sector = 0;
        s_page = PAGES_PER_SEC;        /* force erase of sector 0 on first commit */
        s_dirty = true;                /* persist the migrated snapshot soon */
        s_dirty_ms = 1000;             /* no debounce wait for the first save */
        return;
    }

    const uint8_t *p = flash_ptr(page_off(best_sec, best_page));
    const rec_hdr_t *h = (const rec_hdr_t *)p;
    memset(&s_cur, 0, sizeof(s_cur));  /* new fields default to 0 */
    size_t n = h->payload_len < sizeof(user_settings_t) ? h->payload_len
                                                        : sizeof(user_settings_t);
    memcpy(&s_cur, p + sizeof(rec_hdr_t), n);
    s_seq = best_seq;
    s_sector = best_sec;

    /* Next free page = first erased page after the newest record. */
    s_page = PAGES_PER_SEC;
    for (int pg = best_page + 1; pg < PAGES_PER_SEC; pg++) {
        const uint32_t *m = (const uint32_t *)flash_ptr(page_off(best_sec, pg));
        if (*m == 0xFFFFFFFFu) { s_page = pg; break; }
    }
    printf("[settings] loaded seq %lu (sector %d page %d, v%u)\n",
           (unsigned long)best_seq, best_sec, best_page, h->version);
}

user_settings_t *settings(void) { return &s_cur; }

void settings_mark_dirty(void) {
    s_dirty = true;
    s_dirty_ms = 0;
}

static bool s_retrying;               /* commit failed: back off to 1 s */

void settings_tick(void) {
    if (!s_dirty) return;
    s_dirty_ms += 50;                  /* called on the 50 ms main-loop cadence */
    if (s_dirty_ms < (s_retrying ? 1000u : 200u)) return;
    s_dirty = false;
    if (commit_snapshot()) {
        s_retrying = false;
    } else {
        /* Failure must NOT orphan the data (a one-shot attempt silently
           lost every list change) — keep retrying, loudly, with backoff. */
        s_retrying = true;
        s_dirty = true;
        s_dirty_ms = 0;
    }
}

void settings_debug_dump(void) {
    printf("[settings] btmac %02X%02X%02X%02X%02X%02X\n",
           s_cur.bt_mac[0], s_cur.bt_mac[1], s_cur.bt_mac[2],
           s_cur.bt_mac[3], s_cur.bt_mac[4], s_cur.bt_mac[5]);
    printf("[settings] seq %lu sector %d page %d dirty %d retrying %d last_rc %d struct %u\n",
           (unsigned long)s_seq, s_sector, s_page, s_dirty, s_retrying,
           s_last_rc, (unsigned)sizeof(user_settings_t));
}

bool settings_flush(void) {
    if (!s_dirty) return true;
    s_dirty = false;
    return commit_snapshot();
}

/* ---- saved-device list ----------------------------------------------------- */
static bool saved_mac_valid(const uint8_t mac[6]) {
    bool any = false, zero = true;
    for (int k = 0; k < 6; k++) { any |= mac[k] != 0xFF; zero &= mac[k] == 0; }
    return any && !zero;
}

/* Generic MRU-list machinery (saved headsets + relay phones share the same
   {mac6,name24} entry shape). */
typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    char    name[SETTINGS_SAVED_NAME_LEN];
} mru_entry_t;

static void mru_add(mru_entry_t *list, int n, const uint8_t mac[6], const char *name) {
    if (!saved_mac_valid(mac)) return;
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (memcmp(list[i].mac, mac, 6) == 0) { found = i; break; }
    }
    char keep[SETTINGS_SAVED_NAME_LEN] = {0};
    if (found >= 0) memcpy(keep, list[found].name, SETTINGS_SAVED_NAME_LEN);
    int evict = (found >= 0) ? found : n - 1;   /* matched entry, or the OLDEST */
    for (int i = evict; i > 0; i--) list[i] = list[i - 1];
    memcpy(list[0].mac, mac, 6);
    memset(list[0].name, 0, SETTINGS_SAVED_NAME_LEN);
    if (name && name[0]) {
        strncpy(list[0].name, name, SETTINGS_SAVED_NAME_LEN - 1);
    } else if (found >= 0) {
        memcpy(list[0].name, keep, SETTINGS_SAVED_NAME_LEN);
        list[0].name[SETTINGS_SAVED_NAME_LEN - 1] = 0;
    }
    settings_mark_dirty();
}

static void mru_remove(mru_entry_t *list, int n, uint8_t idx) {
    if (idx >= n) return;
    for (int i = idx; i < n - 1; i++) list[i] = list[i + 1];
    memset(&list[n - 1], 0, sizeof(list[0]));
    settings_mark_dirty();
}

void settings_saved_add(const uint8_t mac[6], const char *name) {
    mru_add((mru_entry_t *)s_cur.saved, SETTINGS_SAVED_LIMIT, mac, name);
}
void settings_saved_forget(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < SETTINGS_SAVED_N; i++) {
        if (memcmp(s_cur.saved[i].mac, mac, 6) != 0) continue;
        mru_remove((mru_entry_t *)s_cur.saved, SETTINGS_SAVED_N, i);
        return;
    }
}
void settings_saved_remove(uint8_t idx) {
    mru_remove((mru_entry_t *)s_cur.saved, SETTINGS_SAVED_N, idx);
}
void settings_phone_add(const uint8_t mac[6], const char *name) {
    mru_add((mru_entry_t *)s_cur.phones, SETTINGS_PHONES_N, mac, name);
}
void settings_phone_remove(uint8_t idx) {
    mru_remove((mru_entry_t *)s_cur.phones, SETTINGS_PHONES_N, idx);
}
