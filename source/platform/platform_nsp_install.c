#include "platform_nsp_install.h"
#include "core/nsp_update.h"
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECTIONS_TITLE_ID UINT64_C(0x0100C04E4E530000)
#define INSTALL_BUFFER_SIZE (128 * 1024)
#define PACKAGE_ENTRIES 16
#define CNMT_MAX_SIZE 4096

typedef struct {
    NcmContentId id;
    uint64_t offset, size;
    uint8_t hash[32];
    bool meta, created, placeholder_active;
    NcmPlaceHolderId placeholder;
} InstallContent;

/* NS IApplicationManagerInterface record layout, also used by open-source
 * installers. Commands 16/17 are absent from libnx's public NS wrappers.
 * https://gist.github.com/ITotalJustice/dd5a3bb51ccf0a34b33a116bb170b2e7 */
typedef struct {
    NcmContentMetaKey key;
    uint8_t storage_id;
    uint8_t padding[7];
} ApplicationRecord;

static Result list_records(Service *service, ApplicationRecord records[16], s32 *count)
{
    const struct { u64 offset, title_id; } input = { 0, CONNECTIONS_TITLE_ID };
    return serviceDispatchInOut(service, 17, input, *count,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { records, sizeof(ApplicationRecord) * 16 } });
}

static Result push_records(Service *service, const ApplicationRecord *records, size_t count)
{
    const struct { u8 event; u8 padding[7]; u64 title_id; } input =
        { 3, {0}, CONNECTIONS_TITLE_ID };
    return serviceDispatchIn(service, 16, input,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { records, sizeof(ApplicationRecord) * count } });
}

static uint64_t little(const uint8_t *data, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= (uint64_t)data[i] << (8 * i);
    return value;
}

static bool read_at(FILE *file, uint64_t offset, void *data, size_t count)
{
    return offset <= INT64_MAX && fseeko(file, (off_t)offset, SEEK_SET) == 0 &&
           fread(data, 1, count, file) == count;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_package(FILE *file, InstallContent contents[3], uint32_t expected_version,
                           char error[128])
{
    uint8_t header[16], table[PACKAGE_ENTRIES * 24], names[2048];
    if (fseeko(file, 0, SEEK_END) != 0) return false;
    off_t length = ftello(file);
    if (length < 16 || !read_at(file, 0, header, sizeof(header)) ||
        memcmp(header, "PFS0", 4) != 0) return false;
    uint32_t count = little(header + 4, 4), strings = little(header + 8, 4);
    if (count != 4 || count > PACKAGE_ENTRIES || !strings || strings > sizeof(names)) return false;
    uint64_t data_start = 16 + (uint64_t)count * 24 + strings;
    if (data_start > (uint64_t)length ||
        !read_at(file, 16, table, count * 24) ||
        !read_at(file, 16 + count * 24, names, strings)) return false;
    unsigned ncas = 0, metas = 0, manifests = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *entry = table + i * 24;
        uint64_t offset = little(entry, 8), size = little(entry + 8, 8);
        uint32_t name_offset = little(entry + 16, 4);
        if (name_offset >= strings || !memchr(names + name_offset, 0, strings - name_offset) ||
            !size || offset > (uint64_t)length - data_start ||
            size > (uint64_t)length - data_start - offset) return false;
        for (unsigned j = 0; j < i; ++j) {
            uint64_t other_offset = little(table + j * 24, 8);
            uint64_t other_size = little(table + j * 24 + 8, 8);
            if (offset < other_offset + other_size && other_offset < offset + size) return false;
        }
        const char *name = (const char *)names + name_offset;
        if (strcmp(name, "connections-update.json") == 0) {
            uint8_t manifest[NSP_UPDATE_MANIFEST_LIMIT]; char version[32]; uint32_t native = 0;
            if (size >= sizeof(manifest) || ++manifests != 1 ||
                !read_at(file, data_start + offset, manifest, size) ||
                !nsp_update_manifest(manifest, size, version, sizeof(version), &native) ||
                native != expected_version) return false;
            continue;
        }
        size_t name_length = strlen(name);
        bool meta = name_length == 41 && strcmp(name + 32, ".cnmt.nca") == 0;
        if (ncas == 3 || (!meta && (name_length != 36 || strcmp(name + 32, ".nca") != 0))) return false;
        InstallContent *content = &contents[ncas++];
        content->meta = meta;
        content->offset = data_start + offset;
        content->size = size;
        if (size < 0xC00 || size > UINT64_C(512) * 1024 * 1024) return false;
        for (unsigned j = 0; j < 16; ++j) {
            int high = hex_digit(name[j * 2]), low = hex_digit(name[j * 2 + 1]);
            if (high < 0 || low < 0) return false;
            content->id.c[j] = high * 16 + low;
        }
        for (unsigned j = 0; j + 1 < ncas; ++j)
            if (memcmp(content->id.c, contents[j].id.c, 16) == 0) return false;
        metas += meta;
    }
    if (ncas != 3 || metas != 1 || manifests != 1) {
        snprintf(error, 128, "Invalid Connections package contents");
        return false;
    }
    return true;
}

static Result read_cnmt(NcmContentStorage *storage, const InstallContent *meta,
                        uint8_t bytes[CNMT_MAX_SIZE], size_t *size)
{
    char path[FS_MAX_PATH];
    FsFileSystem fs = {0}; FsDir directory = {0}; FsFile file = {0};
    Result result = ncmContentStorageGetPath(storage, path, sizeof(path), &meta->id);
    if (R_SUCCEEDED(result)) result = fsOpenFileSystemWithId(&fs, CONNECTIONS_TITLE_ID,
        FsFileSystemType_ContentMeta, path, FsContentAttributes_None);
    if (R_SUCCEEDED(result)) result = fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &directory);
    FsDirectoryEntry entries[2]; s64 count = 0;
    if (R_SUCCEEDED(result)) result = fsDirRead(&directory, &count, 2, entries);
    if (R_SUCCEEDED(result) && (count != 1 || entries[0].file_size < 0x30 ||
        entries[0].file_size > CNMT_MAX_SIZE)) result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(result)) {
        size_t length = strnlen(entries[0].name, sizeof(entries[0].name));
        if (length < 5 || length >= sizeof(path) - 1 || strcmp(entries[0].name + length - 5, ".cnmt"))
            result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        else {
            snprintf(path, sizeof(path), "/%s", entries[0].name);
            result = fsFsOpenFile(&fs, path, FsOpenMode_Read, &file);
        }
    }
    u64 actual = 0;
    if (R_SUCCEEDED(result)) result = fsFileRead(&file, 0, bytes, entries[0].file_size,
                                                FsReadOption_None, &actual);
    if (R_SUCCEEDED(result) && actual != (u64)entries[0].file_size)
        result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *size = actual;
    if (serviceIsActive(&file.s)) fsFileClose(&file);
    if (serviceIsActive(&directory.s)) fsDirClose(&directory);
    if (serviceIsActive(&fs.s)) fsFsClose(&fs);
    return result;
}

/* Converts the packaged CNMT into NCM's installed representation. Only the
 * fixed application layout our hacPack pipeline produces is accepted. */
static bool validate_cnmt(const uint8_t *bytes, size_t size, uint32_t version,
                          InstallContent contents[3], NcmContentMetaKey *key,
                          uint8_t installed[128], size_t *installed_size)
{
    if (size != 0x20 + 0x10 + 2 * 0x38 + 0x20 ||
        little(bytes, 8) != CONNECTIONS_TITLE_ID || little(bytes + 8, 4) != version ||
        bytes[12] != NcmContentMetaType_Application || little(bytes + 14, 2) != 16 ||
        little(bytes + 16, 2) != 2 || little(bytes + 18, 2) != 0 ||
        little(bytes + 0x20, 8) != CONNECTIONS_TITLE_ID + 0x800) return false;
    uint8_t digest[32]; sha256CalculateHash(digest, bytes, size - 32);
    if (memcmp(digest, bytes + size - 32, 32)) return false;
    NcmContentInfo infos[3] = {0}; bool program = false, control = false;
    for (unsigned i = 0; i < 3; ++i) if (contents[i].meta) {
        infos[0].content_id = contents[i].id;
        infos[0].content_type = NcmContentType_Meta;
        ncmU64ToContentInfoSize(contents[i].size, &infos[0]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const uint8_t *record = bytes + 0x30 + i * 0x38;
        uint8_t type = record[54];
        if (record[55] != 0 || (type != NcmContentType_Program && type != NcmContentType_Control)) return false;
        if (type == NcmContentType_Program) { if (program) return false; program = true; }
        else { if (control) return false; control = true; }
        InstallContent *content = NULL;
        for (unsigned j = 0; j < 3; ++j)
            if (!contents[j].meta && !memcmp(record + 32, contents[j].id.c, 16)) content = &contents[j];
        if (!content || little(record + 48, 6) != content->size || memcmp(record, content->hash, 32)) return false;
        memcpy(&infos[i + 1], record + 32, sizeof(NcmContentInfo));
    }
    if (!program || !control) return false;
    memset(key, 0, sizeof(*key));
    key->id = CONNECTIONS_TITLE_ID; key->version = version; key->type = NcmContentMetaType_Application;
    NcmContentMetaHeader header = { .extended_header_size = 16, .content_count = 3,
                                   .attributes = bytes[20] };
    memcpy(installed, &header, sizeof(header));
    memcpy(installed + sizeof(header), bytes + 0x20, 16);
    memcpy(installed + sizeof(header) + 16, infos, sizeof(infos));
    *installed_size = sizeof(header) + 16 + sizeof(infos);
    return true;
}

static Result validate_control(NcmContentStorage *storage, InstallContent contents[3],
                                const uint8_t *cnmt, uint32_t version, uint8_t *buffer)
{
    NcmContentId control_id;
    bool found = false;
    for (unsigned i = 0; i < 2; ++i) {
        const uint8_t *record = cnmt + 0x30 + i * 0x38;
        if (record[54] == NcmContentType_Control) {
            memcpy(control_id.c, record + 32, 16); found = true;
        }
    }
    if (!found) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    (void)contents;
    char path[FS_MAX_PATH]; FsFileSystem fs = {0}; FsFile file = {0};
    Result result = ncmContentStorageGetPath(storage, path, sizeof(path), &control_id);
    if (R_SUCCEEDED(result)) result = fsOpenFileSystemWithId(&fs, CONNECTIONS_TITLE_ID,
        FsFileSystemType_ContentControl, path, FsContentAttributes_None);
    if (R_SUCCEEDED(result)) result = fsFsOpenFile(&fs, "/control.nacp", FsOpenMode_Read, &file);
    s64 size = 0; u64 actual = 0;
    if (R_SUCCEEDED(result)) result = fsFileGetSize(&file, &size);
    if (R_SUCCEEDED(result) && size != 0x4000) result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(result)) result = fsFileRead(&file, 0, buffer, 0x4000, FsReadOption_None, &actual);
    char display_version[16]; snprintf(display_version, sizeof(display_version), "%u.%u.%u",
        version >> 24, (version >> 16) & 255, (version >> 8) & 255);
    if (R_SUCCEEDED(result) && (actual != 0x4000 ||
        memcmp(buffer + 0x300, "Connections\0", 12) ||
        memcmp(buffer + 0x500, "Zoboo\0", 6) || buffer[0x3025] != 1 ||
        !memchr(buffer + 0x3060, 0, 16) || strcmp((char *)buffer + 0x3060, display_version) ||
        little(buffer + 0x3038, 8) != CONNECTIONS_TITLE_ID ||
        little(buffer + 0x3078, 8) != CONNECTIONS_TITLE_ID ||
        little(buffer + 0x30b0, 8) != CONNECTIONS_TITLE_ID))
        result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (serviceIsActive(&file.s)) fsFileClose(&file);
    if (serviceIsActive(&fs.s)) fsFsClose(&fs);
    return result;
}

bool platform_nsp_install(const char *path, uint32_t expected_version,
                          SDL_atomic_t *cancel, SDL_atomic_t *progress, char *out_error, size_t error_capacity)
{
    char error[128];
    FILE *file = NULL;
    uint8_t *buffer = NULL;
    InstallContent contents[3] = {0};
    NcmContentStorage storage = {0}; NcmContentMetaDatabase database = {0};
    Service manager = {0}; bool ncm_active = false, ns_active = false;
    bool success = false, key_added = false, record_pushed = false;
    ApplicationRecord old_records[16] = {0}; s32 old_count = 0;
    NcmContentMetaKey key = {0}, old_key = {0};
    Result result = 0; const char *operation = "package validation";
    snprintf(error, 128, "Invalid Connections update package");
#define CALL(name, call) do { operation = name; result = (call); if (R_FAILED(result)) goto cleanup; } while (0)
#define REJECT(message) do { snprintf(error, sizeof(error), "%s", message); goto cleanup; } while (0)
#define CANCELLED() (cancel && SDL_AtomicGet(cancel))
    if (appletGetAppletType() != AppletType_Application || !envIsNso())
        REJECT("Native update requires installed Connections");
    u64 program_id = 0;
    CALL("svcGetInfo ProgramId", svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0));
    FILE *environment_log = fopen("sdmc:/switch/Connections/update.log", "a");
    if (environment_log) {
        fprintf(environment_log, "Native install: applet=%u NSO=%u program=%016llX\n",
            (unsigned)appletGetAppletType(), envIsNso(), (unsigned long long)program_id);
        fclose(environment_log);
    }
    if (program_id != CONNECTIONS_TITLE_ID) REJECT("Running application title ID mismatch");
    file = fopen(path, "rb");
    if (!file || !parse_package(file, contents, expected_version, error)) goto cleanup;
    uint64_t total_bytes = 0, verified_bytes = 0, installed_bytes = 0;
    for (unsigned i = 0; i < 3; ++i) total_bytes += contents[i].size;
    if (progress) SDL_AtomicSet(progress, 80);
    buffer = malloc(INSTALL_BUFFER_SIZE);
    if (!buffer) { snprintf(error, 128, "Update memory unavailable"); goto cleanup; }
    /* Verify every full content hash before touching installed storage. */
    for (unsigned i = 0; i < 3; ++i) {
        Sha256Context hash; sha256ContextCreate(&hash);
        for (uint64_t offset = 0; offset < contents[i].size;) {
            if (CANCELLED()) goto cleanup;
            size_t count = contents[i].size - offset < INSTALL_BUFFER_SIZE ?
                contents[i].size - offset : INSTALL_BUFFER_SIZE;
            if (!read_at(file, contents[i].offset + offset, buffer, count)) goto cleanup;
            sha256ContextUpdate(&hash, buffer, count); offset += count;
            verified_bytes += count;
            if (progress && total_bytes) SDL_AtomicSet(progress, 80 + (int)(verified_bytes * 5 / total_bytes));
        }
        sha256ContextGetHash(&hash, contents[i].hash);
        if (memcmp(contents[i].hash, contents[i].id.c, 16)) goto cleanup;
    }
    CALL("ncmInitialize", ncmInitialize()); ncm_active = true;
    CALL("nsInitialize", nsInitialize()); ns_active = true;
    CALL("nsGetApplicationManagerInterface", nsGetApplicationManagerInterface(&manager));
    CALL("NS ListApplicationRecordContentMeta", list_records(&manager, old_records, &old_count));
    if (old_count < 1 || old_count >= 16) REJECT("Installed application record count invalid");
    NcmStorageId storage_id = NcmStorageId_None;
    for (s32 i = 0; i < old_count; ++i) {
        if (old_records[i].key.id == CONNECTIONS_TITLE_ID &&
            old_records[i].key.type == NcmContentMetaType_Application) {
            if (storage_id != NcmStorageId_None) REJECT("Multiple installed base records found");
            storage_id = old_records[i].storage_id;
            old_key = old_records[i].key;
        }
    }
    if (storage_id != NcmStorageId_SdCard && storage_id != NcmStorageId_BuiltInUser)
        REJECT("Installed Connections storage unavailable");
    if (expected_version <= old_key.version) REJECT("Downloaded version is not newer than installed");
    CALL("ncmOpenContentStorage", ncmOpenContentStorage(&storage, storage_id));
    CALL("ncmOpenContentMetaDatabase", ncmOpenContentMetaDatabase(&database, storage_id));
    bool old_present = false;
    CALL("ncm database current key", ncmContentMetaDatabaseHas(&database, &old_present, &old_key));
    if (!old_present) REJECT("Installed metadata key missing");
    s64 free_space = 0; u64 needed = 0;
    for (unsigned i = 0; i < 3; ++i) needed += contents[i].size;
    CALL("ncm free space", ncmContentStorageGetFreeSpaceSize(&storage, &free_space));
    if (free_space < 0 || (u64)free_space < needed + 1024 * 1024) {
        snprintf(error, 128, "Insufficient update storage"); goto cleanup;
    }
    for (unsigned i = 0; i < 3; ++i) {
        InstallContent *content = &contents[i]; bool exists = false;
        if (CANCELLED()) goto cleanup;
        CALL("ncm content presence", ncmContentStorageHas(&storage, &exists, &content->id));
        if (exists) {
            /* Shared/retried contents must also be intact. Never replace them. */
            s64 stored_size = 0;
            CALL("ncm existing content size", ncmContentStorageGetSizeFromContentId(&storage, &stored_size, &content->id));
            if (stored_size < 0 || (u64)stored_size != content->size) REJECT("Existing content size mismatch");
            Sha256Context stored_hash; uint8_t digest[32]; sha256ContextCreate(&stored_hash);
            for (uint64_t offset = 0; offset < content->size;) {
                if (CANCELLED()) goto cleanup;
                size_t count = content->size - offset < INSTALL_BUFFER_SIZE ? content->size - offset : INSTALL_BUFFER_SIZE;
                CALL("ncm existing content read", ncmContentStorageReadContentIdFile(&storage, buffer, count, &content->id, offset));
                sha256ContextUpdate(&stored_hash, buffer, count); offset += count;
                installed_bytes += count;
                if (progress && total_bytes) SDL_AtomicSet(progress, 85 + (int)(installed_bytes * 14 / total_bytes));
            }
            sha256ContextGetHash(&stored_hash, digest);
            if (memcmp(digest, content->hash, sizeof(digest))) REJECT("Existing content hash mismatch");
            continue;
        }
        CALL("ncm placeholder ID", ncmContentStorageGeneratePlaceHolderId(&storage, &content->placeholder));
        CALL("ncm create placeholder", ncmContentStorageCreatePlaceHolder(&storage, &content->id,
            &content->placeholder, content->size)); content->placeholder_active = true;
        Sha256Context written_hash; uint8_t written_digest[32]; sha256ContextCreate(&written_hash);
        for (uint64_t offset = 0; offset < content->size;) {
            if (CANCELLED()) goto cleanup;
            size_t count = content->size - offset < INSTALL_BUFFER_SIZE ? content->size - offset : INSTALL_BUFFER_SIZE;
            if (!read_at(file, content->offset + offset, buffer, count)) goto cleanup;
            sha256ContextUpdate(&written_hash, buffer, count);
            CALL("ncm write placeholder", ncmContentStorageWritePlaceHolder(&storage,
                &content->placeholder, offset, buffer, count)); offset += count;
            installed_bytes += count;
            if (progress && total_bytes) SDL_AtomicSet(progress, 85 + (int)(installed_bytes * 14 / total_bytes));
        }
        sha256ContextGetHash(&written_hash, written_digest);
        if (memcmp(written_digest, content->hash, sizeof(written_digest))) REJECT("Package changed while installing");
        CALL("ncm flush placeholder", ncmContentStorageFlushPlaceHolder(&storage));
        CALL("ncm register content", ncmContentStorageRegister(&storage, &content->id, &content->placeholder));
        content->created = true; content->placeholder_active = false;
    }
    uint8_t cnmt[CNMT_MAX_SIZE], installed[128]; size_t cnmt_size = 0, installed_size = 0;
    for (unsigned i = 0; i < 3; ++i) if (contents[i].meta)
        CALL("FS mount/read CNMT", read_cnmt(&storage, &contents[i], cnmt, &cnmt_size));
    if (!validate_cnmt(cnmt, cnmt_size, expected_version, contents, &key, installed, &installed_size))
        REJECT("CNMT title/version/content validation failed");
    CALL("FS validate Control NACP", validate_control(&storage, contents, cnmt, expected_version, buffer));
    bool new_present = false;
    CALL("ncm new key presence", ncmContentMetaDatabaseHas(&database, &new_present, &key));
    if (new_present) REJECT("Downloaded metadata key already exists");
    if (CANCELLED()) goto cleanup;
    CALL("ncm set metadata", ncmContentMetaDatabaseSet(&database, &key, installed, installed_size)); key_added = true;
    if (CANCELLED()) goto cleanup;
    CALL("ncm commit metadata", ncmContentMetaDatabaseCommit(&database));
    ApplicationRecord new_records[16]; memcpy(new_records, old_records, sizeof(new_records));
    for (s32 i = 0; i < old_count; ++i)
        if (new_records[i].key.id == CONNECTIONS_TITLE_ID && new_records[i].key.type == NcmContentMetaType_Application)
            new_records[i].key = key;
    if (CANCELLED()) goto cleanup;
    record_pushed = true; /* A failed IPC reply must also attempt restoration. */
    CALL("NS PushApplicationRecord", push_records(&manager, new_records, old_count));
    /* A failed cache invalidation does not invalidate the committed install. */
    Result cache_result = serviceDispatchIn(&manager, 404, key.id);
    if (R_FAILED(cache_result)) {
        FILE *log = fopen("sdmc:/switch/Connections/update.log", "a");
        if (log) { fprintf(log, "NS InvalidateApplicationControlCache: 0x%08lX\n", (unsigned long)cache_result); fclose(log); }
    }
    success = true;
    if (progress) SDL_AtomicSet(progress, 100);
cleanup:
    if (!success) {
        if (CANCELLED()) snprintf(error, 128, "Update cancelled");
        else if (R_FAILED(result)) snprintf(error, 128, "%s failed: 0x%08lX", operation, (unsigned long)result);
        bool can_remove_content = true;
        /* Roll back only keys/content introduced by this invocation. Old
         * metadata and old running NCAs are deliberately retained. */
        Result record_rollback = 0, metadata_rollback = 0;
        if (record_pushed) {
            record_rollback = push_records(&manager, old_records, old_count);
            if (R_FAILED(record_rollback)) can_remove_content = false;
        }
        if (key_added && can_remove_content) {
            metadata_rollback = ncmContentMetaDatabaseRemove(&database, &key);
            if (R_SUCCEEDED(metadata_rollback)) metadata_rollback = ncmContentMetaDatabaseCommit(&database);
            if (R_FAILED(metadata_rollback)) can_remove_content = false;
        }
        if (serviceIsActive(&storage.s)) for (unsigned i = 0; i < 3; ++i) {
            Result cleanup_result = 0;
            if (contents[i].placeholder_active) cleanup_result = ncmContentStorageDeletePlaceHolder(&storage, &contents[i].placeholder);
            if (can_remove_content && contents[i].created) cleanup_result = ncmContentStorageDelete(&storage, &contents[i].id);
            if (R_FAILED(cleanup_result)) {
                FILE *cleanup_log = fopen("sdmc:/switch/Connections/update.log", "a");
                if (cleanup_log) { fprintf(cleanup_log, "Native install content rollback: 0x%08lX\n", (unsigned long)cleanup_result); fclose(cleanup_log); }
            }
        }
        FILE *log = fopen("sdmc:/switch/Connections/update.log", "a");
        if (log) {
            fprintf(log, "Native install: %s\n", error);
            if (R_FAILED(record_rollback)) fprintf(log, "NS record rollback: 0x%08lX\n", (unsigned long)record_rollback);
            if (R_FAILED(metadata_rollback)) fprintf(log, "NCM metadata rollback: 0x%08lX\n", (unsigned long)metadata_rollback);
            fclose(log);
        }
    }
    if (serviceIsActive(&database.s)) ncmContentMetaDatabaseClose(&database);
    if (serviceIsActive(&storage.s)) ncmContentStorageClose(&storage);
    if (serviceIsActive(&manager)) serviceClose(&manager);
    if (ns_active) nsExit();
    if (ncm_active) ncmExit();
    free(buffer); if (file) fclose(file);
    snprintf(out_error, error_capacity, "%s", success ? "" : error);
    return success;
#undef CALL
#undef REJECT
#undef CANCELLED
}
