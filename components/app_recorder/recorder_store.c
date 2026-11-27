#include "recorder_store.h"

#include "recorder_store_lock.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The host tests build this file without ESP-IDF, so the log is only wired
 * up for the firmware build. */
#if defined(ESP_PLATFORM)
#include "esp_log.h"
static const char *TAG = "recorder_store";
#define STORE_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#else
#define STORE_LOGE(...) ((void)0)
#endif

/* Every function that touches the recordings tree holds the shared lock for
 * the whole LittleFS transaction. The lock is recursive, so a locked function
 * may call another locked function from the same task. */
#define RECORDER_STORE_LOCK() recorder_store_lock_acquire(RECORDER_STORE_LOCK_TIMEOUT_MS)

/* How many recordings one scan reads. The names are held on the stack of the
 * caller, so the number is kept small: the panel shows a handful of rows at
 * a time. */
#define RECORDER_STORE_SCAN_MAX 64
#define RECORDER_STORE_UNLOCK() recorder_store_lock_release()

/* The path buffers belong to the instance, not to the stack of the caller.
 *
 * The write path runs on a task with a stack of a few kilobytes, and the
 * LittleFS commit below it needs several hundred more. Three buffers of five
 * hundred and twelve bytes on that stack were enough to run the stack out,
 * and the board died with a stack protection fault in the middle of the
 * commit, so the recording was lost. The store is on the heap, and the
 * buffers are used under the store lock, so they belong here. */
#define RECORDER_STORE_PATH_MAX 512

struct recorder_store {
    char root[384];
    uint32_t stamp_session;
    char recorded_at[16];
    int has_stamp;
    /* The scratch paths of the write path. */
    char dir[RECORDER_STORE_PATH_MAX];
    char path[RECORDER_STORE_PATH_MAX];
    char meta_path[RECORDER_STORE_PATH_MAX];
};

static uint32_t rotr(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static void sha256_block(uint32_t state[8], const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + s0 + maj;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t offset = 0;
    while (offset + 64 <= len) {
        sha256_block(state, data + offset);
        offset += 64;
    }
    uint8_t block[64];
    size_t rem = len - offset;
    memset(block, 0, sizeof(block));
    if (rem > 0) {
        memcpy(block, data + offset, rem);
    }
    block[rem] = 0x80;
    if (rem >= 56) {
        sha256_block(state, block);
        memset(block, 0, sizeof(block));
    }
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) {
        block[63 - i] = (uint8_t)((bits >> (8 * i)) & 0xffu);
    }
    sha256_block(state, block);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
}

static void hex32(const uint8_t in[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[64] = '\0';
}

static int session_dir(const recorder_store_t *store, uint32_t session_id, char *out, size_t cap)
{
    int written = snprintf(out, cap, "%s/%" PRIu32, store->root, session_id);
    if (written < 0 || (size_t)written >= cap) {
        return -1;
    }
    return 0;
}

static int slice_path(const recorder_store_t *store, uint32_t session_id, uint16_t index, const char *suffix,
                      char *out, size_t cap)
{
    int written = snprintf(out, cap, "%s/%" PRIu32 "/%u.%s", store->root, session_id, (unsigned)index, suffix);
    if (written < 0 || (size_t)written >= cap) {
        return -1;
    }
    return 0;
}

recorder_store_t *recorder_store_open(const char *root)
{
    if (!root || root[0] == '\0' || strlen(root) >= sizeof(((recorder_store_t *)0)->root)) {
        return NULL;
    }
    recorder_store_t *store = calloc(1, sizeof(*store));
    if (!store) {
        return NULL;
    }
    memcpy(store->root, root, strlen(root) + 1);
    return store;
}

void recorder_store_close(recorder_store_t *store)
{
    free(store);
}

int recorder_store_ready(const recorder_store_t *store)
{
    struct stat st;
    if (!store) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    if (stat(store->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    RECORDER_STORE_UNLOCK();
    return 0;
}

int recorder_store_put(recorder_store_t *store, const recorder_slice_t *slice, const uint8_t *data, size_t len)
{
    uint8_t digest[32];
    char sha[65];
    int result = -1;
    /* The paths live in the store, so the frame of this function stays
     * small enough for the task that writes the recording. */
    char *dir = store ? store->dir : NULL;
    char *path = store ? store->path : NULL;
    char *meta_path = store ? store->meta_path : NULL;

    if (!store || !slice || !slice->closed || (len > 0 && data == NULL)) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    if (session_dir(store, slice->session_id, dir, RECORDER_STORE_PATH_MAX) != 0) {
        goto done;
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        goto done;
    }
    if (slice_path(store, slice->session_id, slice->index, "ogg", path, RECORDER_STORE_PATH_MAX) != 0 ||
        slice_path(store, slice->session_id, slice->index, "meta", meta_path, RECORDER_STORE_PATH_MAX) != 0) {
        goto done;
    }
    FILE *audio = fopen(path, "wb");
    if (!audio) {
        goto done;
    }
    if (len > 0 && fwrite(data, 1, len, audio) != len) {
        fclose(audio);
        unlink(path);
        goto done;
    }
    if (fclose(audio) != 0) {
        unlink(path);
        goto done;
    }
    sha256(data, len, digest);
    hex32(digest, sha);
    FILE *meta = fopen(meta_path, "wb");
    if (!meta) {
        unlink(path);
        goto done;
    }
    if (fprintf(meta, "duration_ms=%" PRIu32 "\nis_last=%d\nsha256=%s\n", slice->duration_ms, slice->is_last ? 1 : 0,
                sha) < 0 ||
        (store->has_stamp && store->stamp_session == slice->session_id &&
         fprintf(meta, "recorded_at=%s\n", store->recorded_at) < 0)) {
        fclose(meta);
        unlink(path);
        unlink(meta_path);
        goto done;
    }
    if (fclose(meta) != 0) {
        unlink(path);
        unlink(meta_path);
        goto done;
    }
    result = 0;
done:
    RECORDER_STORE_UNLOCK();
    return result;
}

static int stamp_text_ok(const char *stamp)
{
    if (!stamp) {
        return 0;
    }
    for (int i = 0; i < 13; i++) {
        if (i == 8) {
            if (stamp[i] != '-') {
                return 0;
            }
        } else if (stamp[i] < '0' || stamp[i] > '9') {
            return 0;
        }
    }
    return stamp[13] == '\0' || stamp[13] == '\n' || stamp[13] == '\r';
}

int recorder_store_set_recorded_at(recorder_store_t *store, uint32_t session_id, const char *stamp)
{
    if (!store || !stamp_text_ok(stamp)) {
        return -1;
    }
    store->stamp_session = session_id;
    memcpy(store->recorded_at, stamp, 14);
    store->has_stamp = 1;
    return 0;
}

int recorder_store_recorded_at(recorder_store_t *store, uint32_t session_id, char *out, size_t cap)
{
    char path[512];
    char line[128];
    if (!store || !out || cap < 14 || slice_path(store, session_id, 0, "meta", path, sizeof(path)) != 0) {
        return -1;
    }
    out[0] = '\0';
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *meta = fopen(path, "rb");
    if (!meta) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    while (fgets(line, sizeof(line), meta)) {
        if (strncmp(line, "recorded_at=", 12) == 0 && stamp_text_ok(line + 12)) {
            memcpy(out, line + 12, 13);
            out[13] = '\0';
            fclose(meta);
            RECORDER_STORE_UNLOCK();
            return 0;
        }
    }
    fclose(meta);
    RECORDER_STORE_UNLOCK();
    return 0;
}

int recorder_store_get(recorder_store_t *store, uint32_t session_id, uint16_t index, uint8_t *out, size_t cap,
                       size_t *out_len)
{
    char path[512];
    int result = -1;
    if (!store || !out || !out_len || slice_path(store, session_id, index, "ogg", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *audio = fopen(path, "rb");
    if (!audio) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    size_t read_len = fread(out, 1, cap, audio);
    int extra = fgetc(audio);
    fclose(audio);
    if (extra != EOF) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    *out_len = read_len;
    result = 0;
    RECORDER_STORE_UNLOCK();
    return result;
}

int recorder_store_sha256(recorder_store_t *store, uint32_t session_id, uint16_t index, char sha[65])
{
    uint8_t *data = NULL;
    size_t len = 0;
    size_t cap = 64;
    char path[512];
    int result = -1;
    if (!store || !sha || slice_path(store, session_id, index, "ogg", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *audio = fopen(path, "rb");
    if (!audio) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    data = malloc(cap);
    if (!data) {
        fclose(audio);
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    while (1) {
        if (len == cap) {
            size_t grown = cap * 2u;
            uint8_t *next = realloc(data, grown);
            if (!next) {
                free(data);
                fclose(audio);
                RECORDER_STORE_UNLOCK();
                return -1;
            }
            data = next;
            cap = grown;
        }
        size_t got = fread(data + len, 1, cap - len, audio);
        len += got;
        if (got == 0) {
            break;
        }
    }
    fclose(audio);
    uint8_t digest[32];
    sha256(data ? data : (const uint8_t *)"", len, digest);
    free(data);
    hex32(digest, sha);
    result = 0;
    RECORDER_STORE_UNLOCK();
    return result;
}

int recorder_store_duration_ms(recorder_store_t *store, uint32_t session_id, uint16_t index, uint32_t *out_ms)
{
    char path[512];
    char line[128];
    int result = -1;
    if (!store || !out_ms || slice_path(store, session_id, index, "meta", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *meta = fopen(path, "rb");
    if (!meta) {
        STORE_LOGE("duration: cannot open %s (errno=%d)", path, errno);
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    while (fgets(line, sizeof(line), meta)) {
        unsigned value = 0;
        if (sscanf(line, "duration_ms=%u", &value) == 1) {
            *out_ms = value;
            result = 0;
            break;
        }
    }
    if (result != 0) {
        /* The read failed, so the file is worth showing. It usually means a
         * recording that was damaged by an earlier crash. */
        char content[256];
        size_t total = 0;
        rewind(meta);
        while (total < sizeof(content) - 1 && fgets(content + total, (int)(sizeof(content) - total), meta)) {
            total += strlen(content + total);
        }
        content[total] = '\0';
        STORE_LOGE("duration: no duration_ms in %s, content '%s'", path, content);
    }
    /* One close, after the loop. Closing the file inside the loop as well
     * gave the same FILE two closes, and the second one corrupted newlib's
     * stdio lock: the upload path aborted the board with "assert failed:
     * spinlock_acquire_impl (lock->count == 0)". */
    fclose(meta);
    RECORDER_STORE_UNLOCK();
    return result;
}

int recorder_store_is_last(recorder_store_t *store, uint32_t session_id, uint16_t index)
{
    char path[512];
    char line[128];
    int result = -1;
    if (!store || slice_path(store, session_id, index, "meta", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *meta = fopen(path, "rb");
    if (!meta) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    while (fgets(line, sizeof(line), meta)) {
        if (strncmp(line, "is_last=", 8) == 0) {
            result = line[8] == '1' ? 1 : 0;
            break;
        }
    }
    fclose(meta);
    RECORDER_STORE_UNLOCK();
    return result;
}

int recorder_store_is_uploaded(recorder_store_t *store, uint32_t session_id, uint16_t index)
{
    char path[512];
    char current[65];
    char acknowledged[66] = {0};
    int result = -1;
    if (!store || recorder_store_sha256(store, session_id, index, current) != 0 ||
            slice_path(store, session_id, index, "ack", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        result = 0;
        RECORDER_STORE_UNLOCK();
        return result;
    }
    size_t read = fread(acknowledged, 1, 65, file);
    fclose(file);
    if (read < 64) {
        result = 0;
    } else {
        acknowledged[64] = '\0';
        result = strcmp(current, acknowledged) == 0 ? 1 : 0;
    }
    RECORDER_STORE_UNLOCK();
    return result;
}

int recorder_store_mark_uploaded(recorder_store_t *store, uint32_t session_id, uint16_t index)
{
    char path[512];
    char sha[65];
    int result = -1;
    if (!store || recorder_store_sha256(store, session_id, index, sha) != 0 ||
            slice_path(store, session_id, index, "ack", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    int written = fprintf(file, "%s\n", sha);
    int closed = fclose(file);
    result = (written == 65 && closed == 0) ? 0 : -1;
    RECORDER_STORE_UNLOCK();
    return result;
}

/* Whether the slice was acknowledged as uploaded.
 *
 * The acknowledgement file holds the checksum of the recording that was
 * uploaded. The metadata of the slice holds the checksum of the recording
 * that is on the device, so the two files answer the question without
 * reading the recording itself. Reading a long recording on this flash
 * takes seconds, and the lock is held while it is read.
 *
 * A slice whose metadata carries no checksum (a recording written before
 * the metadata grew one) is counted as uploaded when it has an
 * acknowledgement. The upload itself always compares the checksum of the
 * recording, so a recording that changed after its upload is sent again.
 *
 * Returns 1 when the slice was uploaded, 0 when it was not, -1 on error. */
int recorder_store_acknowledged(recorder_store_t *store, uint32_t session_id, uint16_t index, const char *sha)
{
    char path[512];
    char acknowledged[66] = {0};
    int result = -1;
    if (!store || slice_path(store, session_id, index, "ack", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        /* No acknowledgement: the slice was never uploaded. */
        result = 0;
    } else {
        size_t read = fread(acknowledged, 1, 65, file);
        fclose(file);
        if (read < 64) {
            result = 0;
        } else if (sha == NULL || sha[0] == '\0') {
            /* The recording carries no checksum to compare with. */
            result = 1;
        } else {
            acknowledged[64] = '\0';
            result = strcmp(sha, acknowledged) == 0 ? 1 : 0;
        }
    }
    RECORDER_STORE_UNLOCK();
    return result;
}

int recorder_store_slice_info(recorder_store_t *store, uint32_t session_id, uint16_t index,
                               recorder_slice_info_t *out)
{
    char path[512];
    char line[128];
    char sha[65] = {0};
    FILE *meta;
    if (!store || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (slice_path(store, session_id, index, "meta", path, sizeof(path)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    meta = fopen(path, "rb");
    if (!meta) {
        RECORDER_STORE_UNLOCK();
        return -1;
    }
    /* One pass over the meta file answers every question about the slice. */
    while (fgets(line, sizeof(line), meta)) {
        if (strncmp(line, "duration_ms=", 12) == 0) {
            out->duration_ms = (uint32_t)strtoul(line + 12, NULL, 10);
        } else if (strncmp(line, "is_last=", 8) == 0) {
            out->is_last = line[8] == '1';
        } else if (strncmp(line, "recorded_at=", 12) == 0 && stamp_text_ok(line + 12)) {
            memcpy(out->recorded_at, line + 12, 13);
            out->recorded_at[13] = '\0';
        } else if (strncmp(line, "sha256=", 7) == 0) {
            memcpy(sha, line + 7, 64);
            sha[64] = '\0';
        }
    }
    fclose(meta);
    out->readable = true;
    /* The acknowledgement is a second file, but it is the only other one.
     * The listing does not read the recording: the checksum of the
     * recording is in its metadata file, and reading a long recording on
     * this flash takes seconds while the lock is held. */
    out->uploaded = recorder_store_acknowledged(store, session_id, index, sha) == 1;
    RECORDER_STORE_UNLOCK();
    return 0;
}

/* Read the recordings tree in one pass.
 *
 * Every recording opens its meta file and its acknowledgement file, and
 * opening a file on this flash costs a moment. The counts and the list used
 * to be read by two passes over the same files, so a listing took twice as
 * long as it had to. */
int recorder_store_scan(recorder_store_t *store, recorder_session_info_t *out, size_t cap, size_t *out_count,
                        uint32_t *saved_sessions, uint32_t *pending_sessions)
{
    int result = 0;
    DIR *dir;
    struct dirent *entry;
    /* The names of the recordings, read before the files are opened. The
     * array is on the stack of the caller, so it is kept small: the panel
     * shows a handful of rows at a time. */
    uint32_t ids[RECORDER_STORE_SCAN_MAX];
    size_t id_count = 0;
    size_t count = 0;
    uint32_t saved = 0;
    uint32_t pending = 0;

    if (!store || !out_count || (cap > 0 && !out)) {
        return -1;
    }
    *out_count = 0;
    if (saved_sessions) {
        *saved_sessions = 0;
    }
    if (pending_sessions) {
        *pending_sessions = 0;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    dir = opendir(store->root);
    if (!dir) {
        result = errno == ENOENT ? 0 : -1;
        RECORDER_STORE_UNLOCK();
        return result;
    }
    /* The names are read first, so the lock is not held while the files of
     * every recording are opened. Holding it for the whole pass would keep
     * the uploader and the recorder out of the tree for seconds. */
    while ((entry = readdir(dir)) != NULL) {
        char *end = NULL;
        unsigned long value;
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        value = strtoul(entry->d_name, &end, 10);
        if (!end || *end != '\0' || value > UINT32_MAX) {
            continue;
        }
        if (id_count >= RECORDER_STORE_SCAN_MAX) {
            break;
        }
        ids[id_count++] = (uint32_t)value;
    }
    closedir(dir);
    RECORDER_STORE_UNLOCK();
    for (size_t i = 0; i < id_count; i++) {
        recorder_session_info_t info = {
            .session_id = ids[i],
        };
        bool complete = false;
        bool any_slice = false;
        bool pending_slice = false;
        for (uint16_t index = 0; index < RECORDER_SESSION_MAX_SLICES; index++) {
            recorder_slice_info_t slice;
            if (recorder_store_slice_info(store, info.session_id, index, &slice) != 0) {
                break;
            }
            any_slice = true;
            if (UINT32_MAX - info.duration_ms < slice.duration_ms) {
                break;
            }
            info.duration_ms += slice.duration_ms;
            if (!slice.uploaded) {
                info.pending = true;
                pending_slice = true;
            }
            if (index == 0) {
                memcpy(info.recorded_at, slice.recorded_at, sizeof(info.recorded_at));
            }
            if (slice.is_last) {
                complete = true;
                break;
            }
        }
        if (any_slice) {
            saved++;
            if (pending_slice) {
                pending++;
            }
        }
        if (!complete || cap == 0) {
            continue;
        }
        size_t position = 0;
        while (position < count && out[position].session_id > info.session_id) {
            position++;
        }
        if (position >= cap) {
            continue;
        }
        size_t new_count = count < cap ? count + 1 : count;
        for (size_t j = new_count - 1; j > position; j--) {
            out[j] = out[j - 1];
        }
        out[position] = info;
        count = new_count;
    }
    *out_count = count;
    if (saved_sessions) {
        *saved_sessions = saved;
    }
    if (pending_sessions) {
        *pending_sessions = pending;
    }
    return result;
}

int recorder_store_inventory(recorder_store_t *store, uint32_t *saved_sessions, uint32_t *pending_sessions)
{
    size_t count = 0;
    return recorder_store_scan(store, NULL, 0, &count, saved_sessions, pending_sessions);
}

int recorder_store_list_sessions(recorder_store_t *store, recorder_session_info_t *out, size_t cap,
                                 size_t *out_count)
{
    return recorder_store_scan(store, out, cap, out_count, NULL, NULL);
}

int recorder_store_delete_session(recorder_store_t *store, uint32_t session_id)
{
    char dir[512];
    int result = -1;
    if (!store || session_dir(store, session_id, dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (RECORDER_STORE_LOCK() != 0) {
        return -1;
    }
    for (uint16_t index = 0; index < RECORDER_SESSION_MAX_SLICES; index++) {
        char path[512];
        if (slice_path(store, session_id, index, "ogg", path, sizeof(path)) != 0) {
            goto done;
        }
        unlink(path);
        if (slice_path(store, session_id, index, "meta", path, sizeof(path)) != 0) {
            goto done;
        }
        unlink(path);
        if (slice_path(store, session_id, index, "ack", path, sizeof(path)) != 0) {
            goto done;
        }
        unlink(path);
    }
    if (rmdir(dir) != 0 && errno != ENOENT) {
        goto done;
    }
    result = 0;
done:
    RECORDER_STORE_UNLOCK();
    return result;
}
