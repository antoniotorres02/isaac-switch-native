/* Isaac recomp installer: turns the Builder's payload into an installed title.
 *
 * Reads sdmc:/switch/isaac-recomp/install/ (payload.py), finishes the NCAs
 * with this console's header key (nca_install.c, switch_crypto.c), writes
 * them to the SD card's content storage through ncm, records the title's
 * content meta and pushes the application record to ns, so the game shows
 * on the HOME menu like any other. Installing again replaces the earlier
 * install (the title id and version stay the same). Every step goes to
 * sdmc:/switch/isaac-recomp/install-log.txt.
 *
 * Unsigned NCAs: the console runs them only with sigpatches (Atmosphère).
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "nca_install.h"

#define DIRECT_NRO "sdmc:/switch/isaac-repentance-ns1-recomp.nro"
#define PAYLOAD "sdmc:/switch/isaac-recomp/install"
#define LOG_PATH "sdmc:/switch/isaac-recomp/install-log.txt"
#define MAX_OLD 16

Result inst_switch_header_key(u8 out[32]);

/* ---- text -------------------------------------------------------------------------- */

static int g_es;
#define T(en, es) (g_es ? (es) : (en))

static FILE *g_log;
static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    consoleUpdate(NULL);
}

static void detect_language(void) {
    u64 code = 0;
    SetLanguage lang;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang)))
            g_es = lang == SetLanguage_ES || lang == SetLanguage_ES419;
        setExit();
    }
}

/* ---- ncm ----------------------------------------------------------------------------- */

static NcmContentStorage g_cs;
static NcmContentMetaDatabase g_db;
static struct { NcmPlaceHolderId ph; int live; } g_ph[INST_MAX_CONTENTS];
static int g_nph;

static NcmContentId content_id(const inst_content *c) {
    NcmContentId id;
    memcpy(&id, c->id, sizeof id);
    return id;
}

static int io_create(void *u, const inst_content *c) {
    (void)u;
    NcmContentId id = content_id(c);
    NcmPlaceHolderId ph;
    Result rc = ncmContentStorageGeneratePlaceHolderId(&g_cs, &ph);
    if (R_SUCCEEDED(rc)) rc = ncmContentStorageCreatePlaceHolder(&g_cs, &id, &ph, (s64)c->size);
    logf_("create %s (%llu bytes): 0x%x", c->file, (unsigned long long)c->size, rc);
    if (R_FAILED(rc)) return -1;
    g_ph[g_nph].ph = ph;
    g_ph[g_nph++].live = 1;
    return 0;
}

static int io_write(void *u, const inst_content *c, uint64_t off, const void *data, size_t len) {
    (void)u;
    Result rc = ncmContentStorageWritePlaceHolder(&g_cs, &g_ph[g_nph - 1].ph, off, data, len);
    if (R_FAILED(rc)) logf_("write %s at %llu: 0x%x", c->file, (unsigned long long)off, rc);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

static int io_commit(void *u, const inst_content *c) {
    (void)u;
    NcmContentId id = content_id(c);
    bool has = false;
    Result rc = ncmContentStorageHas(&g_cs, &has, &id);
    if (R_SUCCEEDED(rc) && has) rc = ncmContentStorageDelete(&g_cs, &id);   /* the same build, again */
    if (R_SUCCEEDED(rc)) rc = ncmContentStorageRegister(&g_cs, &id, &g_ph[g_nph - 1].ph);
    logf_("register %s: 0x%x", c->file, rc);
    if (R_FAILED(rc)) return -1;
    g_ph[g_nph - 1].live = 0;
    return 0;
}

static u64 g_last_tick;
static void io_progress(void *u, const char *what, uint64_t done, uint64_t total) {
    (void)u;
    u64 now = armGetSystemTick();
    if (done < total && armTicksToNs(now - g_last_tick) < 200000000ULL) return;
    g_last_tick = now;
    int pct = total ? (int)(done * 100 / total) : 100, bar = pct * 40 / 100;
    char line[48];
    for (int i = 0; i < 40; ++i) line[i] = i < bar ? '#' : '.';
    line[40] = 0;
    printf("\r  [%s] %3d%%  %-12s", line, pct, what);
    consoleUpdate(NULL);
}

static void drop_placeholders(void) {
    for (int i = 0; i < g_nph; ++i)
        if (g_ph[i].live) ncmContentStorageDeletePlaceHolder(&g_cs, &g_ph[i].ph);
    g_nph = 0;
}

/* ---- ns -------------------------------------------------------------------------------- */

typedef struct {
    NcmContentMetaKey key;
    u8 storage_id;
    u8 padding[7];
} StorageRecord;

static Result ns_delete_record(Service *manager, u64 tid) {
    return serviceDispatchIn(manager, 27, tid);
}

static Result ns_push_record(Service *manager, u64 tid, const StorageRecord *r) {
    const struct { u8 last_event; u8 padding[7]; u64 tid; } in = { 3, {0}, tid };   /* 3: installed */
    return serviceDispatchIn(manager, 16, in,
                             .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                             .buffers = { { r, sizeof *r } });
}

static int ns_has_record(u64 tid) {
    NsApplicationRecord rec[32];
    s32 n = 0;
    for (s32 off = 0;; off += n) {
        if (R_FAILED(nsListApplicationRecord(rec, 32, off, &n)) || n <= 0) return 0;
        for (s32 i = 0; i < n; ++i)
            if (rec[i].application_id == tid) return 1;
    }
}

/* ---- install ----------------------------------------------------------------------------- */

static int install(inst_manifest *m) {
    char err[256] = "";
    u8 key[32];
    int ok = 0, ncm = 0, ns = 0;
    Result rc;
    Service session = {0}, *manager = &session;
    NcmContentMetaKey meta_key = { .id = m->title_id, .version = m->version,
                                   .type = NcmContentMetaType_Application, .install_type = NcmContentInstallType_Full };
    NcmContentId old[MAX_OLD];
    s32 nold = 0;

    say(T("\n Deriving this console's header key... ", "\n Obteniendo la clave de cabecera de esta consola... "));
    rc = inst_switch_header_key(key);
    logf_("header key: 0x%x", rc);
    if (R_FAILED(rc)) { snprintf(err, sizeof err, "spl 0x%x", rc); goto out; }
    say("OK\n");

    if (R_FAILED(rc = nsInitialize())) { snprintf(err, sizeof err, "ns 0x%x", rc); goto out; }
    ns = 1;
    /* Since Horizon 3.0, nsInitialize opens the getter service. The
       application manager needs a separate service session. */
    if (hosversionBefore(3, 0, 0)) {
        manager = nsGetServiceSession_ApplicationManagerInterface();
    } else {
        rc = nsGetApplicationManagerInterface(&session);
        logf_("application manager: 0x%x", rc);
        if (R_FAILED(rc)) { snprintf(err, sizeof err, "ns application manager 0x%x", rc); goto out; }
    }
    int replacing_record = ns_has_record(m->title_id);

    if (R_FAILED(rc = ncmInitialize())) { snprintf(err, sizeof err, "ncm 0x%x", rc); goto out; }
    ncm = 1;
    if (R_FAILED(rc = ncmOpenContentStorage(&g_cs, NcmStorageId_SdCard))) {
        snprintf(err, sizeof err, T("the SD card's content storage did not open (0x%x)",
                                    "no se pudo abrir el almacenamiento de la SD (0x%x)"), rc);
        ncm = 1;
        goto out;
    }
    ncm = 2;
    if (R_FAILED(rc = ncmOpenContentMetaDatabase(&g_db, NcmStorageId_SdCard))) {
        snprintf(err, sizeof err, "ncm db 0x%x", rc);
        goto out;
    }
    ncm = 3;

    s64 free_space = 0;
    ncmContentStorageGetFreeSpaceSize(&g_cs, &free_space);
    logf_("free space %lld, need %llu", (long long)free_space, (unsigned long long)m->total);
    if (free_space && (u64)free_space < m->total + (16u << 20)) {
        snprintf(err, sizeof err, T("not enough space on the SD card: %llu MB free, %llu MB needed",
                                    "no hay espacio en la SD: %llu MB libres, %llu MB necesarios"),
                 (unsigned long long)(free_space >> 20), (unsigned long long)(m->total >> 20) + 16);
        goto out;
    }

    bool had = false;
    if (R_SUCCEEDED(ncmContentMetaDatabaseHas(&g_db, &had, &meta_key)) && had) {
        NcmContentInfo info[MAX_OLD];
        s32 n = 0;
        if (R_SUCCEEDED(ncmContentMetaDatabaseListContentInfo(&g_db, &n, info, MAX_OLD, &meta_key, 0)))
            for (s32 i = 0; i < n; ++i) old[nold++] = info[i].content_id;
        logf_("an earlier install: %d content(s)", nold);
        say(T(" An earlier install was found; it will be replaced.\n",
              " Hay una instalación anterior; se sustituirá.\n"));
    }

    say(T(" Installing (the screen updates as it goes)...\n", " Instalando (la pantalla se actualiza sola)...\n"));
    inst_io io = { NULL, io_create, io_write, io_commit, io_progress };
    uint8_t record[0x100];
    size_t record_len = 0;
    g_last_tick = 0;
    if (inst_install(PAYLOAD, m, key, &io, record, sizeof record, &record_len, err, sizeof err) != 0) goto out;
    say("\n");

    rc = ncmContentMetaDatabaseSet(&g_db, &meta_key, record, record_len);
    if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&g_db);
    logf_("content meta: 0x%x", rc);
    if (R_FAILED(rc)) { snprintf(err, sizeof err, "ncm meta 0x%x", rc); goto out; }

    if (replacing_record) {
        rc = ns_delete_record(manager, m->title_id);
        logf_("old record removed: 0x%x", rc);
        if (R_FAILED(rc)) { snprintf(err, sizeof err, "ns remove old record 0x%x", rc); goto out; }
    }
    StorageRecord sr = { .key = meta_key, .storage_id = NcmStorageId_SdCard };
    rc = ns_push_record(manager, m->title_id, &sr);
    logf_("application record: 0x%x", rc);
    if (R_FAILED(rc)) { snprintf(err, sizeof err, "ns record 0x%x", rc); goto out; }

    for (s32 i = 0; i < nold; ++i) {
        int kept = 0;
        for (int k = 0; k <= m->count; ++k) kept |= !memcmp(&old[i], m->c[k].id, sizeof old[i]);
        bool has = false;
        if (!kept && R_SUCCEEDED(ncmContentStorageHas(&g_cs, &has, &old[i])) && has)
            logf_("old content removed: 0x%x", ncmContentStorageDelete(&g_cs, &old[i]));
    }

    ok = 1;
out:
    if (!ok) {
        logf_("FAILED: %s", err);
        say(T("\n\n Install FAILED: %s\n", "\n\n La instalación FALLÓ: %s\n"), err);
        if (ncm >= 2) drop_placeholders();
    }
    serviceClose(&session);
    if (ns) nsExit();
    if (ncm >= 3) ncmContentMetaDatabaseClose(&g_db);
    if (ncm >= 2) ncmContentStorageClose(&g_cs);
    if (ncm) ncmExit();
    memset(key, 0, sizeof key);
    return ok;
}

static void delete_payload(const inst_manifest *m) {
    char path[300];
    for (int i = 0; i < m->count; ++i) {
        snprintf(path, sizeof path, PAYLOAD "/%s", m->c[i].file);
        remove(path);
    }
    remove(PAYLOAD "/install.txt");
    rmdir(PAYLOAD);
    logf_("payload deleted");
}

/* ---- screen ---------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    detect_language();
    g_log = fopen(LOG_PATH, "w");

    say("\n \x1b[1mISAAC REPENTANCE+ NS1 RECOMP\x1b[0m  -  %s\n\n", T("installer", "instalador"));
    inst_manifest m;
    char err[256];
    int state;                                   /* 0 ready, 1 installed, 2 nothing to do */
    if (inst_read_manifest(PAYLOAD, &m, err, sizeof err) != 0) {
        logf_("manifest: %s", err);
        say(T(" No install files: %s\n\n Build them with Isaac Switch Builder and copy its sd-card folder\n"
              " to the root of the SD card.\n",
              " No hay ficheros de instalación: %s\n\n Genéralos con Isaac Switch Builder y copia su carpeta sd-card\n"
              " a la raíz de la SD.\n"), err);
        state = 2;
    } else {
        logf_("manifest: title %016llx, %d contents, %llu bytes", (unsigned long long)m.title_id, m.count,
              (unsigned long long)m.total);
        say(T(" Title id      %016llX\n Version       %s\n Size          %llu MB\n Installs to   SD card\n\n",
              " Id de título  %016llX\n Versión       %s\n Tamaño        %llu MB\n Se instala en la tarjeta SD\n\n"),
            (unsigned long long)m.title_id, m.display_version, (unsigned long long)(m.total >> 20));
        say(T(" Needs Atmosphère with sigpatches. Mods go in sdmc:/switch/isaac-recomp/mods/\n"
              " and saves stay in sdmc:/switch/isaac-recomp/saves/.\n\n",
              " Necesita Atmosphère con sigpatches. Los mods van en sdmc:/switch/isaac-recomp/mods/\n"
              " y las partidas siguen en sdmc:/switch/isaac-recomp/saves/.\n\n"));
        say(T(" \x1b[1mA\x1b[0m install    \x1b[1m+\x1b[0m exit\n", " \x1b[1mA\x1b[0m instalar    \x1b[1m+\x1b[0m salir\n"));
        state = 0;
    }
    if (state == 2) say(T("\n \x1b[1m+\x1b[0m exit\n", "\n \x1b[1m+\x1b[0m salir\n"));

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) break;
        if (state == 0 && (down & HidNpadButton_A)) {
            appletBeginBlockingHomeButton(0);
            appletSetAutoSleepDisabled(true);
            int ok = install(&m);
            appletSetAutoSleepDisabled(false);
            appletEndBlockingHomeButton();
            if (ok) {
                say(T("\n \x1b[32mInstalled.\x1b[0m The game is on the HOME menu.\n\n"
                      " \x1b[1mX\x1b[0m delete the install files (%llu MB)    \x1b[1m+\x1b[0m exit\n",
                      "\n \x1b[32mInstalado.\x1b[0m El juego está en el menú HOME.\n\n"
                      " \x1b[1mX\x1b[0m borrar los ficheros de instalación (%llu MB)    \x1b[1m+\x1b[0m salir\n"),
                    (unsigned long long)(m.total >> 20));
                state = 1;
            } else {
                say(T("\n Details in %s\n \x1b[1mA\x1b[0m try again    \x1b[1m+\x1b[0m exit\n",
                      "\n Detalles en %s\n \x1b[1mA\x1b[0m reintentar    \x1b[1m+\x1b[0m salir\n"), LOG_PATH);
                if (access(DIRECT_NRO, F_OK) == 0)
                    say(T(" Direct launch: exit and open ISAAC REPENTANCE+ NS1 RECOMP in the Homebrew Menu.\n",
                          " Inicio directo: sal y abre ISAAC REPENTANCE+ NS1 RECOMP desde el menú homebrew.\n"));
            }
        } else if (state == 1 && (down & HidNpadButton_X)) {
            delete_payload(&m);
            say(T(" Install files deleted.  \x1b[1m+\x1b[0m exit\n", " Ficheros borrados.  \x1b[1m+\x1b[0m salir\n"));
            state = 2;
        }
        consoleUpdate(NULL);
    }
    if (g_log) fclose(g_log);
    consoleExit(NULL);
    return 0;
}
