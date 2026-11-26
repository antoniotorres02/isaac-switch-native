#include "native_launcher.h"
#include "native_modsync.h"
#include "native_sha1.h"
#include "native_steamcm.h"
#include "native_steamproto.h"
#include "native_zip.h"
#include "switch.h"

#include <assert.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>
#include <lzma.h>

#define ACCOUNT 76561198000000003ull
#define ITEM 3181120001ull
#define DEP 3181120002ull
#define MANUAL 3181120003ull
#define BASE_MANIFEST 8501123682003000000ull
#define OTHER_MANIFEST (BASE_MANIFEST + 1)
#define CODE 365911335216870ull

static struct {
    uint64_t manifest;
    unsigned queries, calls, subscription_requests, chunks, writes, pages;
    int denied, malformed, network_error, corrupt_chunk, unsafe, cancel_at_chunk, unsubscribed;
    int clear_names, embedded_nul, wrapped_names, wrong_key, wrong_file_hash, full_browse;
    int delay_chunks, transfer_cancel, wrong_id, wrong_version, subscribed, account_error, codec, retry_once;
    uint64_t done, total;
    uint8_t data[3][4096];
    size_t sizes[3];
    uint8_t manifest_zip[8192];
    size_t manifest_len;
} mock;

static const uint8_t key_value[32] = {3, 2, 1, 4, 5};
static const char *contents[] = {"<metadata><name>Workshop Fixture</name></metadata>",
                                  "return 42\n", "return 43\n"};
static const char *paths[] = {"metadata.xml", "content\\main.lua", "content\\other.lua"};
static unsigned char ids[3][20];

static void le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static size_t encrypt(const void *plain, int len, uint8_t *out) {
    uint8_t iv[16] = {4, 7, 2};
    int n = 0, m = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    assert(ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_ecb(), NULL, key_value, NULL));
    assert(EVP_CIPHER_CTX_set_padding(ctx, 0) && EVP_EncryptUpdate(ctx, out, &n, iv, sizeof iv) && n == 16);
    assert(EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key_value, iv));
    assert(EVP_CIPHER_CTX_set_padding(ctx, 1));
    assert(EVP_EncryptUpdate(ctx, out + 16, &n, plain, len));
    assert(EVP_EncryptFinal_ex(ctx, out + 16 + n, &m));
    EVP_CIPHER_CTX_free(ctx);
    return 16 + n + m;
}

static size_t chunk_data(unsigned codec, const char *data, uint8_t *out, size_t cap) {
    size_t len = strlen(data);
    uint32_t crc = crc32(0, (const Bytef *)data, len);
    if (codec == 1) {
        memcpy(out, "VSZa", 4);
        le32(out + 4, crc);
        size_t packed = ZSTD_compress(out + 8, cap - 23, data, len, 1);
        assert(!ZSTD_isError(packed));
        uint8_t *tail = out + 8 + packed;
        memset(tail, 0, 15);
        le32(tail, crc);
        le32(tail + 4, len);
        memcpy(tail + 12, "zsv", 3);
        return packed + 23;
    }
    if (codec == 2) {
        memcpy(out, "VZa", 3);
        le32(out + 3, crc);
        lzma_options_lzma opt;
        assert(!lzma_lzma_preset(&opt, 1));
        #ifdef LZMA_FILTER_LZMA1EXT
        /* Valve's compressor omits the end-of-stream marker. */
        opt.ext_flags = 0;
        lzma_filter filters[] = {{LZMA_FILTER_LZMA1EXT, &opt}, {LZMA_VLI_UNKNOWN, NULL}};
        #else
        lzma_filter filters[] = {{LZMA_FILTER_LZMA1, &opt}, {LZMA_VLI_UNKNOWN, NULL}};
        #endif
        assert(lzma_properties_encode(&filters[0], out + 7) == LZMA_OK);
        size_t pos = 12;
        assert(lzma_raw_buffer_encode(filters, NULL, (const uint8_t *)data, len,
                                      out, &pos, cap - 10) == LZMA_OK);
        le32(out + pos, crc);
        le32(out + pos + 4, len);
        memcpy(out + pos + 8, "zv", 2);
        return pos + 10;
    }
    return isaac_zip_pack(out, cap, data, len);
}

static void fixture(void) {
    uint8_t payload[4096], sections[8192];
    isaac_pbw manifest;
    isaac_pbw_init(&manifest, payload, sizeof payload);
    for (unsigned i = 0; i < 3; ++i) {
        const char *content = i == 2 && mock.manifest == OTHER_MANIFEST ? "return 99\n" : contents[i];
        const char *path = i == 1 && mock.unsafe ? "content\\..\\..\\saves\\run.dat" : paths[i];
        uint8_t zip[1024], file_msg[512], chunk_msg[128];
        size_t len = strlen(content);
        isaac_sha1_buf(content, len, ids[i]);
        size_t n = chunk_data(mock.codec, content, zip, sizeof zip);
        assert(n);
        mock.sizes[i] = encrypt(zip, n, mock.data[i]);

        isaac_pbw c, f;
        isaac_pbw_init(&c, chunk_msg, sizeof chunk_msg);
        isaac_pbw_bytes(&c, 1, ids[i], 20);
        isaac_pbw_fixed32(&c, 2, adler32(0, (const Bytef *)content, len));
        if (mock.wrong_version && i == 1) isaac_pbw_varint(&c, 3, 1);
        isaac_pbw_varint(&c, 4, len);
        isaac_pbw_varint(&c, 5, mock.sizes[i]);

        isaac_pbw_init(&f, file_msg, sizeof file_msg);
        if (i == 1 && mock.embedded_nul) {
            static const char invalid_name[] = "content\\main.lua\0other";
            isaac_pbw_bytes(&f, 1, invalid_name, sizeof invalid_name);
        } else if (mock.clear_names) {
            isaac_pbw_bytes(&f, 1, path, strlen(path) + 1);
        } else {
            uint8_t encrypted_path[256], name[352];
            size_t encrypted = encrypt(path, strlen(path) + 1, encrypted_path);
            int encoded = EVP_EncodeBlock(name + 1, encrypted_path, (int)encrypted);
            assert(encoded > 0);
            if (mock.wrapped_names) {
                name[0] = '\t';
                name[encoded + 1] = '\n';
                name[encoded + 2] = 0;
                isaac_pbw_bytes(&f, 1, name, (size_t)encoded + 3);
            } else isaac_pbw_bytes(&f, 1, name + 1, (size_t)encoded);
        }
        isaac_pbw_varint(&f, 2, len);
        static const uint8_t zero_digest[20];
        isaac_pbw_bytes(&f, 5, mock.wrong_file_hash && i == 1 ? zero_digest : ids[i], 20);
        isaac_pbw_bytes(&f, 6, chunk_msg, c.len);
        isaac_pbw_bytes(&manifest, 1, file_msg, f.len);
        assert(!c.overflow && !f.overflow && !manifest.overflow);
    }
    if (mock.clear_names) {
        uint8_t msg[128], zero[20] = {0};
        isaac_pbw file;
        isaac_pbw_init(&file, msg, sizeof msg);
        isaac_pbw_str(&file, 1, "content\\empty");
        isaac_pbw_varint(&file, 2, 0);
        isaac_pbw_bytes(&file, 5, zero, sizeof zero);
        isaac_pbw_bytes(&manifest, 1, msg, file.len);
    }
    size_t pos = 0;
    le32(sections + pos, 0x71f617d0);
    le32(sections + pos + 4, manifest.len);
    memcpy(sections + pos + 8, payload, manifest.len);
    pos += 8 + manifest.len;
    isaac_pbw metadata;
    isaac_pbw_init(&metadata, payload, sizeof payload);
    isaac_pbw_varint(&metadata, 1, mock.wrong_id ? 250901 : WORKSHOP_DEPOT);
    isaac_pbw_varint(&metadata, 2, mock.manifest);
    isaac_pbw_varint(&metadata, 4, !mock.clear_names);
    le32(sections + pos, 0x1f4812be);
    le32(sections + pos + 4, metadata.len);
    memcpy(sections + pos + 8, payload, metadata.len);
    pos += 8 + metadata.len;
    le32(sections + pos, 0x1b81b817);
    le32(sections + pos + 4, 0);
    le32(sections + pos + 8, 0x32c415ab);
    mock.manifest_len = isaac_zip_pack(mock.manifest_zip, sizeof mock.manifest_zip, sections, pos + 12);
    assert(mock.manifest_len);
}

static void item_details(isaac_pbw *w, unsigned field, uint64_t id) {
    uint8_t buf[1024], child[32];
    isaac_pbw item, dep;
    isaac_pbw_init(&item, buf, sizeof buf);
    isaac_pbw_varint(&item, 1, 1);
    isaac_pbw_varint(&item, 2, id);
    isaac_pbw_fixed64(&item, 3, ACCOUNT);
    isaac_pbw_varint(&item, 5, WORKSHOP_APP);
    isaac_pbw_varint(&item, 8, 123);
    isaac_pbw_fixed64(&item, 14, mock.manifest);
    isaac_pbw_str(&item, 16, id == ITEM ? "Steam fixture item" : "Steam fixture dependency");
    isaac_pbw_str(&item, 17, "An item description.");
    isaac_pbw_varint(&item, 20, 1700000000);
    isaac_pbw_varint(&item, 36, 6000);
    isaac_pbw_varint(&item, 49, 1);
    isaac_pbw_init(&dep, child, sizeof child);
    isaac_pbw_varint(&dep, 1, id == ITEM ? DEP : ITEM); /* a cycle */
    isaac_pbw_bytes(&item, 53, child, dep.len);
    isaac_pbw_varint(&item, 56, 1700000000);
    isaac_pbw_bytes(w, field, buf, item.len);
}

static int rpc(void *ctx, const char *method, const void *req, size_t len, void *reply,
               size_t cap, size_t *n, char err[WORKSHOP_ERR]) {
    (void)ctx;
    ++mock.calls;
    if (mock.denied) { strcpy(err, "Access denied"); return -1; }
    isaac_pbw w;
    isaac_pbw_init(&w, reply, cap);
    if (!strcmp(method, "PublishedFile.QueryFiles#1")) {
        ++mock.queries;
        char cursor[WORKSHOP_CURSOR], string[96];
        assert(isaac_pbr_uint(req, len, 3, 0) == WORKSHOP_PAGE);
        assert(isaac_pbr_uint(req, len, 5, 0) == WORKSHOP_APP);
        assert(!isaac_pbr_uint(req, len, 22, 0));
        assert(isaac_pbr_str(req, len, 39, cursor, sizeof cursor) > 0);
        if (isaac_pbr_str(req, len, 11, string, sizeof string) > 0) {
            assert(!strcmp(string, "lua"));
            assert(isaac_pbr_uint(req, len, 1, 0) == 21);
            assert(isaac_pbr_str(req, len, 6, string, sizeof string) == 5 && !strcmp(string, "Items"));
        }
        int first = !strcmp(cursor, "*");
        unsigned count = first && mock.full_browse ? WORKSHOP_PAGE : 1;
        isaac_pbw_varint(&w, 1, mock.full_browse ? WORKSHOP_PAGE + 1 : 2);
        for (unsigned i = 0; i < count; ++i) item_details(&w, 2, first ? ITEM + i : DEP);
        if (first) isaac_pbw_str(&w, 3, "page-two");
    } else if (!strcmp(method, "PublishedFile.GetDetails#1")) {
        assert(isaac_pbr_uint(req, len, 14, 0) == WORKSHOP_APP);
        assert(isaac_pbr_uint(req, len, 4, 0) == 1);
        item_details(&w, 1, isaac_pbr_uint(req, len, 1, 0));
    } else if (!strcmp(method, "PublishedFile.GetUserFiles#1")) {
        unsigned page = isaac_pbr_uint(req, len, 4, 0);
        char selector[32];
        assert(isaac_pbr_uint(req, len, 1, 0) == ACCOUNT);
        assert(isaac_pbr_uint(req, len, 2, 0) == WORKSHOP_APP);
        assert(isaac_pbr_uint(req, len, 5, 0) == WORKSHOP_SUBS_PAGE);
        assert(isaac_pbr_str(req, len, 6, selector, sizeof selector) > 0 && !strcmp(selector, "mysubscriptions"));
        ++mock.subscription_requests;
        if (mock.pages) {
            isaac_pbw_varint(&w, 1, WORKSHOP_SUBS_PAGE + 1);
            unsigned count = page == 1 ? WORKSHOP_SUBS_PAGE : 1;
            for (unsigned i = 0; i < count; ++i)
                item_details(&w, 3, ITEM + 10 + i + WORKSHOP_SUBS_PAGE * (page - 1));
        } else {
            isaac_pbw_varint(&w, 1, 1);
            item_details(&w, 3, ITEM);
        }
    } else if (!strcmp(method, "PublishedFile.AreFilesInSubscriptionList#1")) {
        uint64_t id = isaac_pbr_uint(req, len, 2, 0);
        uint8_t membership[32];
        isaac_pbw inner;
        isaac_pbw_init(&inner, membership, sizeof membership);
        isaac_pbw_fixed64(&inner, 1, id);
        if ((id == ITEM && !mock.unsubscribed) || (id == DEP && mock.subscribed)) isaac_pbw_varint(&inner, 2, 1);
        isaac_pbw_bytes(&w, 1, membership, inner.len);
    } else if (!strcmp(method, "PublishedFile.Subscribe#1")) {
        assert(isaac_pbr_uint(req, len, 3, 0) == WORKSHOP_APP);
        assert(isaac_pbr_uint(req, len, 1, 0) == DEP);
        ++mock.writes;
        mock.subscribed = 1;
    } else if (!strcmp(method, "PublishedFile.Unsubscribe#1")) {
        assert(isaac_pbr_uint(req, len, 3, 0) == WORKSHOP_APP);
        assert(isaac_pbr_uint(req, len, 1, 0) == ITEM);
        ++mock.writes;
        mock.unsubscribed = 1;
    } else if (!strcmp(method, "ContentServerDirectory.GetManifestRequestCode#1")) {
        assert(isaac_pbr_uint(req, len, 1, 0) == WORKSHOP_APP);
        assert(isaac_pbr_uint(req, len, 2, 0) == WORKSHOP_DEPOT);
        assert(isaac_pbr_uint(req, len, 3, 0) == mock.manifest);
        isaac_pbw_varint(&w, 1, CODE);
    } else if (!strcmp(method, "ContentServerDirectory.GetServersForSteamPipe#1")) {
        uint8_t server[100];
        isaac_pbw s;
        isaac_pbw_init(&s, server, sizeof server);
        isaac_pbw_str(&s, 8, "invalid.example");
        isaac_pbw_bytes(&w, 1, server, s.len);
        isaac_pbw_init(&s, server, sizeof server);
        isaac_pbw_str(&s, 8, "mock.steamcontent.com");
        isaac_pbw_str(&s, 12, "mandatory");
        isaac_pbw_bytes(&w, 1, server, s.len);
    } else if (!strcmp(method, "ContentServerDirectory.GetCDNAuthToken#1")) {
        char host[64];
        assert(isaac_pbr_str(req, len, 2, host, sizeof host) > 0 && !strcmp(host, "mock.steamcontent.com"));
        isaac_pbw_str(&w, 1, "?token=fixture");
    } else if (strcmp(method, "Player.GetPlayerLinkDetails#1")) {
        fprintf(stderr, "Unexpected RPC %s\n", method);
        abort();
    }
    assert(!w.overflow);
    *n = w.len;
    if (mock.malformed) { ((uint8_t *)reply)[0] = 0; *n = 1; }
    return 0;
}

static int fetch(void *ctx, const char *host, const char *url, void *out, size_t cap, size_t *n,
                 int *status, char err[WORKSHOP_ERR]) {
    (void)ctx; (void)err;
    assert(!strcmp(host, "mock.steamcontent.com"));
    *status = 200;
    if (!strstr(url, "?token=fixture")) { *status = 403; return 0; }
    if (strstr(url, "/manifest/")) {
        assert(strtoull(strstr(url, "/manifest/") + 10, NULL, 10) == mock.manifest);
        assert(strtoull(strstr(url, "/5/") + 3, NULL, 10) == CODE);
        assert(cap >= mock.manifest_len);
        memcpy(out, mock.manifest_zip, mock.manifest_len);
        *n = mock.manifest_len;
        return 0;
    }
    for (unsigned i = 0; i < 3; ++i) {
        char hex[41];
        isaac_sha1_hex(ids[i], hex);
        if (!strstr(url, hex)) continue;
        ++mock.chunks;
        if (mock.delay_chunks) usleep(5000);
        if (i == 1 && mock.cancel_at_chunk) {
            mock.transfer_cancel = 1;
            return -1;
        }
        if (i == 1 && mock.network_error) return -1;
        if (i == 1 && mock.retry_once) {
            --mock.retry_once;
            return -1;
        }
        assert(cap >= mock.sizes[i]);
        memcpy(out, mock.data[i], mock.sizes[i]);
        *n = mock.sizes[i];
        if (i == 1 && mock.corrupt_chunk) ((uint8_t *)out)[16] ^= 3;
        return 0;
    }
    abort();
}

static int depot_key(void *ctx, uint32_t app, uint32_t depot, uint8_t key[32], char err[WORKSHOP_ERR]) {
    (void)ctx; (void)err;
    assert(app == WORKSHOP_APP && depot == WORKSHOP_DEPOT);
    memcpy(key, key_value, 32);
    if (mock.wrong_key) key[0] ^= 1;
    return 0;
}

static int cancelled(void *ctx) { (void)ctx; return mock.transfer_cancel; }
static void progress(void *ctx, uint64_t n, uint64_t total) {
    (void)ctx;
    assert(n <= total);
    mock.done = n;
    mock.total = total;
}

static const workshop_net net = {NULL, ACCOUNT, rpc, fetch, depot_key, cancelled, progress, 123, NULL};
static char err[WORKSHOP_ERR];

static int exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }

static void write_file(const char *root, const char *suffix, const char *data) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, suffix);
    FILE *f = fopen(path, "wb");
    assert(f && fputs(data, f) >= 0 && fclose(f) == 0);
}

static void read_content(const mod_store *store, const char *expected) {
    char path[1024], buf[64];
    snprintf(path, sizeof path, "%s/workshop_%llu/content/other.lua", store->mods, ITEM);
    FILE *f = fopen(path, "rb");
    assert(f && fgets(buf, sizeof buf, f) && fclose(f) == 0);
    assert(!strcmp(buf, expected));
}

static void local_fixture(mod_store *store, const char *root, const char *saves) {
    assert(!mod_store_init(store, root, saves, 0));
    char path[MOD_PATH + MOD_FOLDER];
    snprintf(path, sizeof path, "%s/manual", store->mods);
    assert(!mod_mkdir_tree(path));
    write_file(path, "metadata.xml", "<name>Manual mod</name>");
    snprintf(path, sizeof path, "%s/workshop_%llu", store->mods, MANUAL);
    assert(!mod_mkdir_tree(path));
    write_file(path, "metadata.xml", "<name>Manual mod with numeric name</name>");
}

static void test_unowned_workspace(const char *root) {
    char private_root[MOD_PATH], fixture_path[1024];
    snprintf(private_root, sizeof private_root, "%s/unowned", root);
    mod_store test;
    assert(!mod_store_init(&test, private_root, NULL, 0));
    snprintf(fixture_path, sizeof fixture_path, "%s/workshop_%llu.stage", test.work, ITEM);
    assert(!mod_mkdir_tree(fixture_path));
    write_file(fixture_path, "manual", "Preserve this file");
    assert(!mod_recover(&test) && exists(fixture_path));
    mod_install install;
    assert(mod_install_begin(&test, ITEM, 1, &install, err) < 0 && exists(fixture_path));
}

static void test_protocol(void) {
    workshop_page page;
    assert(!workshop_query(&net, WORKSHOP_UPDATED, "lua", "Items", "", &page, err));
    assert(page.count == 1 && page.total == 2 && page.items[0].id == ITEM);
    assert(page.items[0].author == ACCOUNT && page.items[0].subscribers == 6000);
    assert(!workshop_query(&net, WORKSHOP_UPDATED, "lua", "Items", page.cursor, &page, err));
    assert(page.count == 1 && page.items[0].id == DEP && !*page.cursor);
    assert(!workshop_subscriptions(&net, 1, &page, err));
    assert(page.count == 1 && page.items[0].subscribed_at == 1700000000);
    workshop_item item;
    assert(!workshop_details(&net, ITEM, &item, err));
    assert(item.manifest == BASE_MANIFEST && item.dep_count == 1 && item.deps[0] == DEP);
    mock.malformed = 1;
    assert(workshop_query(&net, WORKSHOP_NEW, "", "", "", &page, err) < 0 && strstr(err, "Malformed"));
    mock.malformed = 0;
    mock.denied = 1;
    assert(workshop_subscriptions(&net, 1, &page, err) < 0 && strstr(err, "denied"));
    mock.denied = 0;
}

static void test_install(mod_store *store) {
    workshop_item item;
    local_mod local;
    mod_install install;
    assert(mod_install_begin(store, ITEM, UINT64_MAX, &install, err) < 0 && strstr(err, "space"));
    assert(mod_install_begin(store, MANUAL, 0, &install, err) < 0 && strstr(err, "manual"));
    assert(mod_remove(store, MANUAL, err) < 0);

    const char *unsafe[] = {"/app", "a//file", "a/../x", "C:\\a", "aux.txt", "dir.", "x\\.isaac-workshop-v1"};
    for (unsigned i = 0; i < sizeof unsafe / sizeof *unsafe; ++i) {
        char path[64];
        strcpy(path, unsafe[i]);
        assert(mod_safe_path(path) < 0);
    }

    assert(!workshop_details(&net, ITEM, &item, err));
    mock.retry_once = 1;
    assert(!workshop_install(&net, store, &item, err));
    assert(mock.chunks == 4);
    assert(mock.total && mock.total == mock.done);
    assert(!mod_lookup(store, ITEM, &local));
    assert(local.id == ITEM && local.manifest == BASE_MANIFEST && local.dep_count == 1 &&
           strlen(local.digest) == 40 && local.installed > 0);
    assert(!mod_set_enabled(store, local.folder, 0, err));
    local_mods list = {0};
    assert(!mod_list(store, &list, err) && list.count == 3);
    assert(!mod_find(&list, ITEM)->enabled && mod_find(&list, MANUAL) == NULL);
    free(list.items);

    mock.manifest = OTHER_MANIFEST;
    assert(!workshop_details(&net, ITEM, &item, err));
    assert(mod_update_needed(&local, &item));
    mock.unsafe = 1;
    fixture();
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "Unsafe Workshop path (file 2)"));
    mock.unsafe = 0;
    mock.wrong_id = 1;
    fixture();
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "invalid"));
    mock.wrong_id = 0;
    mock.wrong_version = 1;
    fixture();
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "Invalid Workshop chunk offset (file 2)"));
    mock.wrong_version = 0;
    fixture();
    mock.wrong_key = 1;
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "Workshop depot key could not decrypt filename (file 1)"));
    mock.wrong_key = 0;
    mock.corrupt_chunk = 1;
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "integrity"));
    mock.corrupt_chunk = 0;
    mock.wrong_file_hash = 1;
    fixture();
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "SHA-1 verification"));
    mock.wrong_file_hash = 0;
    fixture();
    mock.network_error = 1;
    assert(workshop_install(&net, store, &item, err) < 0);
    mock.network_error = 0;
    mock.cancel_at_chunk = 1;
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "Cancelled"));
    mock.cancel_at_chunk = mock.transfer_cancel = 0;
    read_content(store, "return 43\n");
    assert(!mod_lookup(store, ITEM, &local) && local.manifest == BASE_MANIFEST);

    /* Stop between the two directory renames, as in a process interruption. */
    assert(!mod_install_begin(store, ITEM, 1, &install, err));
    write_file(install.stage, "incomplete", "temporary");
    assert(rename(install.final, install.backup) == 0);
    assert(!mod_recover(store) && !exists(install.stage) && !exists(install.backup));
    assert(!mod_lookup(store, ITEM, &local) && local.manifest == BASE_MANIFEST);

    mock.clear_names = mock.embedded_nul = 1;
    fixture();
    assert(workshop_install(&net, store, &item, err) < 0 && strstr(err, "Invalid Workshop filename (file 2)"));
    mock.embedded_nul = 0;
    mock.codec = 1;
    fixture();
    assert(!workshop_install(&net, store, &item, err));
    assert(!mod_lookup(store, ITEM, &local) && local.manifest == OTHER_MANIFEST);
    read_content(store, "return 99\n");
    char empty_file[1024];
    struct stat empty;
    snprintf(empty_file, sizeof empty_file, "%s/workshop_%llu/content/empty", store->mods, ITEM);
    assert(!stat(empty_file, &empty) && S_ISREG(empty.st_mode) && empty.st_size == 0);
    mock.clear_names = 0;
    mock.codec = 2;
    mock.wrapped_names = 1;
    fixture();
    mock.wrapped_names = 0;
    assert(!workshop_details(&net, DEP, &item, err));
    assert(!workshop_install(&net, store, &item, err));
    assert(!mod_lookup(store, DEP, &local));
    assert(mod_remove_checked(store, DEP, err) < 0 && strstr(err, "Required by"));
}

static void test_plans(mod_store *store) {
    mod_plan plan = {0};
    local_mods local = {0};
    mod_summary summary;
    assert(!mod_list(store, &local, err) && local.count == 4);
    assert(!mod_plan_subscriptions(&net, &local, MOD_SYNC_TWO_WAY, &plan, err));
    assert(plan.count == 2 && plan.rows[0].action == MOD_REMOVE && plan.rows[1].action == MOD_SUBSCRIBE);
    assert(!plan.rows[0].selected && !plan.rows[1].selected && !mock.writes);
    mod_plan_toggle(&plan, 0);
    mod_plan_toggle(&plan, 1);
    assert(!plan.rows[0].selected && plan.rows[1].selected);
    workshop_net other = net;
    other.steamid++;
    assert(mod_plan_apply(&other, store, &plan, &summary, err) < 0 && !mock.writes);
    assert(!mod_plan_apply(&net, store, &plan, &summary, err) && summary.done == 1 && mock.writes == 1);
    mock.subscribed = 0;
    mod_plan_toggle(&plan, 0);
    assert(!mod_plan_apply(&net, store, &plan, &summary, err) && summary.failed == 1);
    assert(!mod_lookup(store, DEP, NULL));

    assert(!mod_remove(store, DEP, err));
    assert(!mod_list(store, &local, err));
    workshop_item item;
    assert(!workshop_details(&net, ITEM, &item, err));
    assert(!mod_plan_dependencies(&net, &local, &item, &plan, err));
    assert(plan.count == 1 && plan.rows[0].id == DEP);
    assert(!mod_lookup(store, ITEM, NULL) && mod_lookup(store, DEP, NULL) != 0);

    const local_mods empty = {0};
    assert(!mod_plan_subscriptions(&net, &empty, MOD_SYNC_TWO_WAY, &plan, err));
    assert(plan.count == 2 && plan.rows[0].action == MOD_INSTALL && plan.rows[0].selected);
    assert(plan.rows[1].action == MOD_UNSUBSCRIBE && !plan.rows[1].selected);
    mod_plan_toggle(&plan, 1);
    assert(!plan.rows[0].selected && plan.rows[1].selected);
    unsigned writes = mock.writes;
    assert(!mod_plan_apply(&net, store, &plan, &summary, err) && summary.failed == 1);
    assert(mock.writes == writes); /* Local state differs from the plan. */

    mock.pages = 1;
    unsigned before = mock.subscription_requests;
    assert(!mod_plan_subscriptions(&net, &empty, MOD_SYNC_FROM_STEAM, &plan, err));
    assert(mock.subscription_requests - before == 2 && plan.count == WORKSHOP_SUBS_PAGE + 1);
    mock.pages = 0;
    mock.manifest = BASE_MANIFEST;
    fixture();
    assert(!mod_plan_updates(&net, &local, &plan, err));
    assert(plan.count == 1 && plan.rows[0].action == MOD_UPDATE);
    assert(!mod_plan_apply(&net, store, &plan, &summary, err) && summary.done == 1);
    assert(!mod_remove_checked(store, ITEM, err) && mod_lookup(store, ITEM, NULL) != 0);
    assert(!mod_recover(store));
    assert(!mod_list(store, &local, err) && local.count == 2);
    assert(!mod_plan_subscriptions(&net, &local, MOD_SYNC_TO_STEAM, &plan, err));
    assert(plan.count == 0);  /* Remote-only membership remains untouched in this mode. */
    assert(!mod_plan_subscriptions(&net, &local, MOD_SYNC_TWO_WAY, &plan, err));
    mod_plan_toggle(&plan, 1);
    assert(!mod_plan_apply(&net, store, &plan, &summary, err) && summary.done == 1);
    assert(mock.writes == writes + 1 && mock.unsubscribed);
    mock.unsubscribed = 0;
    mod_plan_clear(&plan);
    free(local.items);
}

uint64_t armTicksToNs(uint64_t ticks) { return ticks; }
uint64_t armGetSystemTick(void) { return 0; }
Result socketInitializeDefault(void) { return 0; }
void socketExit(void) {}
Result swkbdCreate(SwkbdConfig *cfg, int x) { (void)cfg; (void)x; return 1; }
void swkbdClose(SwkbdConfig *cfg) { (void)cfg; }
void swkbdConfigMakePresetDefault(SwkbdConfig *cfg) { (void)cfg; }
void swkbdConfigSetGuideText(SwkbdConfig *cfg, const char *s) { (void)cfg; (void)s; }
void swkbdConfigSetInitialText(SwkbdConfig *cfg, const char *s) { (void)cfg; (void)s; }
void swkbdConfigSetStringLenMax(SwkbdConfig *cfg, u32 n) { (void)cfg; (void)n; }
Result swkbdShow(SwkbdConfig *cfg, char *s, size_t n) { (void)cfg; (void)s; (void)n; return 1; }
int isaac_launcher_workshop_description_pages(const char *s) { return !!*s; }
unsigned isaac_launcher_workshop_description_media(const char *s, int p) { (void)s; (void)p; return 0; }

int isaac_steamsync_read_token(const char *path, uint64_t *steamid, char *token, size_t cap,
                               char *error, size_t errcap) {
    (void)path; (void)error; (void)errcap;
    *steamid = ACCOUNT;
    snprintf(token, cap, "synthetic");
    return 0;
}

static int owns(void *s, uint32_t app, char *e, size_t size) {
    (void)s; (void)e; (void)size;
    return app == WORKSHOP_APP;
}
static void close_session(void *s) { (void)s; }

int isaac_steamcm_open(uint64_t id, const char *token, int timeout, isaac_steam_transport *out,
                       char *error, size_t cap) {
    (void)timeout; (void)error; (void)cap;
    assert(id == ACCOUNT && !strcmp(token, "synthetic"));
    if (mock.account_error) return -1;
    *out = (isaac_steam_transport){.owns = owns, .close = close_session};
    return 0;
}
int isaac_steamcm_token_refused(void) { return mock.account_error; }
unsigned isaac_steamcm_cell(void) { return 0; }
void isaac_steamcm_set_cancel(int (*cancel)(void *), void *user) { (void)cancel; (void)user; }
int isaac_steamcm_web_open(char *error, size_t cap) { (void)error; (void)cap; return 0; }
void isaac_steamcm_web_close(void) {}

int isaac_steamcm_service(const char *method, const void *req, size_t len, void *reply, size_t cap,
                          size_t *n, char *error, size_t errcap) {
    (void)errcap;
    return rpc(NULL, method, req, len, reply, cap, n, error);
}

int isaac_steamcm_https_get(const char *host, const char *url, void *out, size_t cap,
                            size_t *len, int *status, int (*cancel)(void *), void *user,
                            char *error, size_t errcap) {
    (void)errcap;
    if (cancel(user)) return -1;
    return fetch(NULL, host, url, out, cap, len, status, error);
}

int isaac_steamcm_depot_key(uint32_t app, uint32_t depot, uint8_t out[32], char *error, size_t cap) {
    (void)cap;
    return depot_key(NULL, app, depot, out, error);
}

static void settle(int signed_in) {
    for (unsigned i = 0; i < 2000; ++i) {
        workshop_browser_input(0, signed_in);
        if (!workshop_browser_view()->busy) return;
        usleep(1000);
    }
    abort();
}

typedef struct {
    const uint8_t *gif;
    size_t size;
    unsigned requests;
} media_fixture;

static int fetch_preview(void *ctx, const char *host, const char *path, void *out, size_t cap,
                         size_t *len, int *status, char error[WORKSHOP_ERR]) {
    (void)error;
    media_fixture *fixture = ctx;
    if (++fixture->requests == 1) {
        assert(!strcmp(host, "steamuserimages-a.akamaihd.net"));
        assert(!strcmp(path, "/ugc/123/ABCD/"));
        *status = 404;
        *len = 0;
    } else {
        assert(!strcmp(host, "images.steamusercontent.com"));
        assert(strstr(path, "/ugc/123/ABCD/?ima=fit") && strstr(path, "imw=268"));
        assert(fixture->size <= cap);
        *status = 200;
        *len = fixture->size;
        memcpy(out, fixture->gif, fixture->size);
    }
    return 0;
}

static void test_media(void) {
    static const uint8_t gif[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x03, 0x00, 0x02, 0x00, 0x81, 0x00,
        0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45,
        0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x00,
        0x08, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02,
        0x00, 0x00, 0x08, 0x06, 0x00, 0x01, 0x08, 0x1c, 0x18, 0x10, 0x00, 0x21,
        0xf9, 0x04, 0x01, 0x0e, 0x00, 0x01, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x02, 0x00, 0x81, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x06, 0x00, 0x01, 0x08, 0x1c, 0x18,
        0x10, 0x00, 0x21, 0xf9, 0x04, 0x01, 0x16, 0x00, 0x01, 0x00, 0x2c, 0x00,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x06, 0x00, 0x01,
        0x08, 0x1c, 0x18, 0x10, 0x00, 0x3b
    };
    char urls[WORKSHOP_MEDIA_IMAGES][WORKSHOP_MEDIA_URL], description[256];
    unsigned count = workshop_media_description(
        "[h1]Preview[/h1] [img]https://i.imgur.com/a.gif[/img] Text\nhttps://example.com/x.png",
        description, sizeof description, urls);
    assert(count == 2 && strstr(description, "Preview") && strstr(description, "Text"));
    assert(strchr(description, WORKSHOP_MEDIA_MARKER) != NULL);
    assert(strchr(description, WORKSHOP_STYLE_MARKER) != NULL);
    unsigned char rgba[4] = {13, 22, 33, 111};
    workshop_thumb thumbnail;
    workshop_thumbnail_scale(&thumbnail, rgba, 1, 1, 4);
    assert(thumbnail.pixels[0] == 0x6f21160du);
    assert(!strstr(description, "https://") && !strcmp(urls[0], "https://i.imgur.com/a.gif"));
    workshop_animation animation = {0};
    assert(workshop_illustration(&net, urls[1], &animation, err) < 0 && !animation.count);
    assert(!workshop_gif(gif, sizeof gif, &animation, WORKSHOP_MEDIA_FRAMES));
    assert(animation.count == 3 && animation.delay[0] == 80 && animation.delay[1] == 140);
    assert(animation.frames[0]->pixels[0] == 0xff0000ffu);
    assert(animation.frames[1]->pixels[0] == 0xff00ff00u);
    assert(animation.frames[2]->pixels[0] == 0xffff0000u);
    workshop_animation_clear(&animation);
    media_fixture fixture = {gif, sizeof gif, 0};
    workshop_net images = {.ctx = &fixture, .get = fetch_preview};
    assert(!workshop_preview(&images, "http://steamuserimages-a.akamaihd.net/ugc/123/ABCD/", &animation, err));
    assert(fixture.requests == 2 && animation.count == 3);
    workshop_animation_clear(&animation);
    assert(!workshop_gif(gif, sizeof gif, &animation, 2));
    assert(animation.count == 2 && animation.delay[0] + animation.delay[1] == 440);
    assert(animation.frames[1]->pixels[0] == 0xffff0000u);
    workshop_animation_clear(&animation);
    assert(workshop_gif(gif, sizeof gif / 2, &animation, WORKSHOP_MEDIA_FRAMES) < 0);
    workshop_animation_clear(&animation);
}

static void test_browse_scrolling(const char *root, const char *saves) {
    unsigned chunks = mock.chunks, queries = mock.queries;
    mock.full_browse = 1;
    assert(!workshop_browser_open(root, saves, "ignored", 0));
    settle(1);
    const workshop_view *v = workshop_browser_view();
    assert(v->grid && v->list_count == WORKSHOP_PAGE && v->row_count == WORKSHOP_GRID_CARDS);
    assert(v->list_start == 0 && v->selected == 0);
    assert(v->hints[WORKSHOP_HINT_SCROLL] && v->hints[WORKSHOP_HINT_PAGES]);
    workshop_browser_input(HidNpadButton_AnyRight, 1);
    assert(workshop_browser_view()->selected == 1);
    workshop_browser_input(HidNpadButton_AnyRight, 1);
    assert(workshop_browser_view()->selected == 1);
    for (unsigned i = 0; i < WORKSHOP_GRID_CARDS / WORKSHOP_GRID_COLUMNS; ++i)
        workshop_browser_input(HidNpadButton_AnyDown, 1);
    v = workshop_browser_view();
    assert(v->list_start == WORKSHOP_GRID_COLUMNS && v->selected == WORKSHOP_GRID_CARDS - 1);
    workshop_browser_input(HidNpadButton_AnyUp, 1);
    v = workshop_browser_view();
    assert(v->list_start == WORKSHOP_GRID_COLUMNS && v->selected == WORKSHOP_GRID_CARDS - 3);
    for (unsigned i = WORKSHOP_GRID_CARDS - WORKSHOP_GRID_COLUMNS + 1; i < WORKSHOP_PAGE;
         i += WORKSHOP_GRID_COLUMNS)
        workshop_browser_input(HidNpadButton_AnyDown, 1);
    settle(1);
    v = workshop_browser_view();
    assert(v->list_count == 1 && v->selected == 0 && strstr(v->context, "Page 2"));
    workshop_browser_input(HidNpadButton_AnyUp, 1);
    settle(1);
    v = workshop_browser_view();
    assert(v->list_count == WORKSHOP_PAGE && v->list_start == WORKSHOP_PAGE - WORKSHOP_GRID_CARDS);
    assert(v->selected == WORKSHOP_GRID_CARDS - 1);
    workshop_browser_input(HidNpadButton_ZR, 1);
    settle(1);
    assert(workshop_browser_view()->list_count == 1);
    workshop_browser_input(HidNpadButton_ZL, 1);
    settle(1);
    v = workshop_browser_view();
    assert(v->list_count == WORKSHOP_PAGE && !v->selected && !v->list_start);
    assert(mock.queries == queries + 2);
    workshop_browser_input(HidNpadButton_A, 1);
    settle(1);
    workshop_browser_input(HidNpadButton_A, 1);
    assert(workshop_browser_view()->detail && workshop_browser_view()->confirming);
    workshop_browser_input(HidNpadButton_B, 1);
    assert(!workshop_browser_view()->confirming && mock.chunks == chunks);
    workshop_browser_close();
    mock.full_browse = 0;
}

static void test_buttons(const mod_store *store, const char *root, const char *saves) {
    unsigned subs = mock.subscription_requests, chunks = mock.chunks, writes = mock.writes;
    assert(!workshop_browser_open(root, saves, "ignored", 0));
    settle(0);
    assert(workshop_browser_input(HidNpadButton_Y, 0) == WORKSHOP_SIGNIN);
    workshop_browser_close();
    assert(!workshop_browser_open(root, saves, "ignored", 0));
    settle(1);
    assert(mock.subscription_requests == subs && mock.chunks == chunks && mock.writes == writes);
    workshop_browser_input(HidNpadButton_L, 1);
    assert(workshop_browser_view()->tab == WORKSHOP_SYNC);
    settle(1);
    assert(mock.subscription_requests == subs);
    workshop_browser_input(HidNpadButton_A, 1);
    settle(1);
    assert(mock.subscription_requests == subs + 1 && mock.chunks == chunks);
    assert(workshop_browser_view()->row_count == 1);
    workshop_browser_input(HidNpadButton_X, 1);
    assert(workshop_browser_view()->confirming);
    workshop_browser_input(HidNpadButton_B, 1);
    assert(mock.chunks == chunks && mock.writes == writes);
    workshop_browser_input(HidNpadButton_X, 1);
    assert(workshop_browser_view()->confirming);
    workshop_browser_input(HidNpadButton_A, 1);
    settle(1);
    assert(workshop_browser_view()->tab == WORKSHOP_SYNC);
    assert(strstr(workshop_browser_view()->notice, "1 succeeded"));
    assert(mock.chunks > chunks && mock.writes == writes);
    workshop_browser_close();
    assert(!mod_lookup(store, ITEM, NULL));
    assert(!mod_remove(store, ITEM, err));

    mod_store title;
    assert(!mod_store_init(&title, root, saves, 1));
    assert(strcmp(title.mods, store->mods) && strcmp(title.work, store->work));
    mock.full_browse = 1;
    mock.delay_chunks = 1;
    assert(!workshop_browser_open(root, saves, "ignored", 1));
    settle(1);
    unsigned start_chunks = mock.chunks;
    workshop_browser_input(HidNpadButton_Plus, 1);
    assert(!workshop_browser_view()->detail && workshop_browser_view()->busy);
    assert(workshop_browser_view()->tab == WORKSHOP_BROWSE);
    workshop_browser_input(HidNpadButton_Plus, 1); /* Duplicate of the active request. */
    workshop_browser_input(HidNpadButton_AnyRight, 1);
    workshop_browser_input(HidNpadButton_Plus, 1);
    settle(1);
    assert(!workshop_browser_view()->detail && strstr(workshop_browser_view()->notice, "2 installed"));
    assert(mock.chunks == start_chunks + 6);
    assert(!mod_lookup(&title, ITEM, NULL) && !mod_lookup(&title, DEP, NULL));
    assert(mod_lookup(store, ITEM, NULL) && mod_lookup(store, DEP, NULL));
    assert(workshop_browser_input(HidNpadButton_B, 1) == WORKSHOP_EXIT);
    assert(mock.subscription_requests == subs + 1 && mock.writes == writes);
    workshop_browser_close();
    assert(!mod_remove(&title, ITEM, err) && !mod_remove(&title, DEP, err));
    mock.full_browse = mock.delay_chunks = 0;

    assert(!workshop_browser_open(root, saves, "ignored", 0));
    mock.account_error = 1;
    settle(1);
    assert(!workshop_browser_view()->signed_in);
    assert(strstr(workshop_browser_view()->notice, "expired"));
    mock.account_error = 0;
    workshop_browser_close();
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const char *root = argv[1];
    char saves[MOD_PATH];
    snprintf(saves, sizeof saves, "%s/saves", root);
    mod_store store;
    local_fixture(&store, root, saves);
    mock.manifest = BASE_MANIFEST;
    fixture();
    test_unowned_workspace(root);
    test_protocol();
    test_install(&store);
    test_plans(&store);
    test_media();
    test_browse_scrolling(root, saves);
    test_buttons(&store, root, saves);
    puts("Workshop protocol, CDN, registry, sync planning and controller tests passed");
    return 0;
}
