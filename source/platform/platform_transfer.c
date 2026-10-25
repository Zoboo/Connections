#include "platform_transfer.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>
#include <switch.h>

#define TRANSFER_SCENE_ID 0x434F
#define TRANSFER_VERSION 1
#define TRANSFER_LDN_VERSION 0
#define TRANSFER_SCAN_INTERVAL_MS 500
#define TRANSFER_SEND_DURATION_MS 45000
#define TRANSFER_SEND_INTERVAL_MS 1500
#define TRANSFER_RECEIVER_STALE_MS 5000
#define TRANSFER_MAX_RECEIVERS 8
#define TRANSFER_RECEIVER_ADVERT_PREFIX "CNXRCV1:"
#define TRANSFER_CHUNK_DATA_SIZE 96
#define TRANSFER_MAX_CHUNKS 16
#define TRANSFER_DATA_ADVERT_MAGIC "CNXDAT"
#define TRANSFER_LOG_DIR "sdmc:/switch/Connections"
#define TRANSFER_LOG_PATH TRANSFER_LOG_DIR "/transfer.log"

typedef struct {
    char magic[8];
    u32 version;
    u32 transfer_id;
    GameSaveCustomPuzzle custom_puzzle;
} TransferPacket;

typedef struct {
    char magic[8];
    u32 version;
    u16 sequence;
    u16 total;
    u16 data_size;
    u32 transfer_id;
    LdnMacAddress target_bssid;
    u8 data[TRANSFER_CHUNK_DATA_SIZE];
} TransferChunk;

static const u8 transfer_passphrase[0x10] = {
    0x43, 0x6F, 0x6E, 0x6E, 0x65, 0x63, 0x74, 0x73,
    0x54, 0x72, 0x61, 0x6E, 0x73, 0x66, 0x65, 0x72
};

static PlatformTransferState current_state = PLATFORM_TRANSFER_IDLE;
static char status_text[96] = "Idle";
static char debug_lines[4][96];
static bool ldn_initialized = false;
static TransferPacket packet;
static TransferChunk chunks[TRANSFER_MAX_CHUNKS];
static int chunk_count = 0;
static int next_chunk_index = 0;
static u8 receive_buffer[sizeof(TransferPacket)];
static bool received_chunks[TRANSFER_MAX_CHUNKS];
static int received_chunk_count = 0;
static int expected_chunk_count = 0;
static u32 active_receive_transfer_id = 0;
static u32 completed_transfer_id = 0;
static int received_frame_count = 0;
static int invalid_frame_count = 0;
static Result last_transfer_result = 0;
static GameSaveCustomPuzzle received_puzzle;
static bool has_received_puzzle = false;
static Uint32 started_at = 0;
static Uint32 last_scan_at = 0;
static Uint32 last_send_at = 0;
static LdnNetworkInfo receiver_networks[TRANSFER_MAX_RECEIVERS];
static char receiver_names[TRANSFER_MAX_RECEIVERS][0x21];
static int receiver_count = 0;
static Uint32 last_receiver_seen_at = 0;
static LdnMacAddress target_destination;
static LdnMacAddress target_bssid;
static s16 target_channel = 0;
static LdnMacAddress receiver_self_bssid;

static void transfer_log(const char *format, ...) {
    mkdir("sdmc:/switch", 0777);
    mkdir(TRANSFER_LOG_DIR, 0777);

    FILE *file = fopen(TRANSFER_LOG_PATH, "ab");
    if (!file)
        return;

    fprintf(file, "[%u] ", (unsigned int)SDL_GetTicks());

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fprintf(file, "\n");
    fclose(file);
}

static void reset_transfer_log(const char *mode) {
    mkdir("sdmc:/switch", 0777);
    mkdir(TRANSFER_LOG_DIR, 0777);

    FILE *file = fopen(TRANSFER_LOG_PATH, "wb");
    if (!file)
        return;

    fprintf(file, "Connections transfer log: %s\n", mode);
    fclose(file);
}

static void set_status(const char *text) {
    if (strcmp(status_text, text) == 0)
        return;

    strncpy(status_text, text, sizeof(status_text));
    status_text[sizeof(status_text) - 1] = '\0';
    transfer_log("STATUS %s", status_text);
}

static void set_debug_line(int index, const char *text) {
    if (index < 0 || index >= 4)
        return;

    strncpy(debug_lines[index], text, sizeof(debug_lines[index]));
    debug_lines[index][sizeof(debug_lines[index]) - 1] = '\0';
}

static void clear_debug_lines(void) {
    for (int i = 0; i < 4; i++)
        debug_lines[i][0] = '\0';
}

static unsigned int mac_tail(LdnMacAddress mac) {
    return ((unsigned int)mac.addr[3] << 16) |
           ((unsigned int)mac.addr[4] << 8) |
           (unsigned int)mac.addr[5];
}

static void capture_state_debug(const char *label, int line) {
    LdnState state = LdnState_None;
    Result rc = ldnGetState(&state);
    char text[96];

    snprintf(text, sizeof(text), "%s state rc %08X s%d", label, (unsigned int)rc, (int)state);
    set_debug_line(line, text);
    transfer_log("DEBUG %s", text);
}

static void capture_network_debug(const char *label, int line) {
    LdnNetworkInfo info = {0};
    Result rc = ldnGetNetworkInfo(&info);
    char text[96];

    if (R_SUCCEEDED(rc)) {
        snprintf(
            text,
            sizeof(text),
            "%s ch%d nodes%d b%06X a%d",
            label,
            (int)info.common.channel,
            (int)info.node_count,
            mac_tail(info.common.bssid),
            (int)info.advertise_data_size
        );
    } else {
        snprintf(text, sizeof(text), "%s info rc %08X", label, (unsigned int)rc);
    }

    set_debug_line(line, text);
    transfer_log("DEBUG %s", text);
}

static bool mac_equal(LdnMacAddress a, LdnMacAddress b) {
    return memcmp(a.addr, b.addr, sizeof(a.addr)) == 0;
}

static void init_security_config(LdnSecurityConfig *sec_config) {
    memset(sec_config, 0, sizeof(*sec_config));
    sec_config->security_mode = LdnSecurityMode_Product;
    sec_config->passphrase_size = sizeof(transfer_passphrase);
    memcpy(sec_config->passphrase, transfer_passphrase, sizeof(transfer_passphrase));
}

static void init_network_config(LdnNetworkConfig *network_config) {
    memset(network_config, 0, sizeof(*network_config));
    network_config->intent_id.local_communication_id = -1;
    network_config->intent_id.scene_id = TRANSFER_SCENE_ID;
    network_config->node_count_max = 8;
    network_config->local_communication_version = TRANSFER_LDN_VERSION;
}

static void set_chunk_targets(LdnMacAddress target) {
    for (int i = 0; i < chunk_count; i++)
        chunks[i].target_bssid = target;
}

static void init_scan_filter(LdnScanFilter *filter) {
    memset(filter, 0, sizeof(*filter));
    filter->network_id.intent_id.scene_id = TRANSFER_SCENE_ID;
    filter->flags = LdnScanFilterFlag_SceneId;
}

static void load_user_name(LdnUserConfig *user_config) {
    strncpy(user_config->user_name, "Connections", sizeof(user_config->user_name) - 1);

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        return;

    AccountUid uid = {0};
    AccountProfile profile = {0};
    AccountProfileBase profile_base = {0};

    rc = accountGetPreselectedUser(&uid);
    if (R_FAILED(rc))
        rc = accountTrySelectUserWithoutInteraction(&uid, false);

    if (R_SUCCEEDED(rc))
        rc = accountGetProfile(&profile, uid);

    if (R_SUCCEEDED(rc))
        rc = accountProfileGet(&profile, NULL, &profile_base);

    if (R_SUCCEEDED(rc)) {
        strncpy(user_config->user_name, profile_base.nickname, sizeof(user_config->user_name) - 1);
        user_config->user_name[sizeof(user_config->user_name) - 1] = '\0';
    }

    accountProfileClose(&profile);
    accountExit();
}

static void leave_network(void) {
    LdnState state;

    if (!ldn_initialized)
        return;

    if (R_SUCCEEDED(ldnGetState(&state))) {
        if (state == LdnState_AccessPointCreated)
            ldnDestroyNetwork();

        if (state == LdnState_AccessPoint || state == LdnState_AccessPointCreated)
            ldnCloseAccessPoint();

        if (state == LdnState_StationConnected)
            ldnDisconnect();

        if (state == LdnState_Station || state == LdnState_StationConnected)
            ldnCloseStation();
    }
}

static void cleanup_transfer(void) {
    transfer_log("cleanup state=%d", (int)current_state);
    leave_network();

    if (ldn_initialized) {
        ldnExit();
        ldn_initialized = false;
    }

}

static bool fail_start(Result rc, const char *step) {
    char text[96];

    cleanup_transfer();
    snprintf(text, sizeof(text), "%s failed: %08X", step, (unsigned int)rc);
    set_status(text);
    capture_state_debug("fail", 0);
    current_state = PLATFORM_TRANSFER_ERROR;
    return false;
}

static bool fail_runtime(const char *text) {
    cleanup_transfer();
    set_status(text);
    capture_state_debug("fail", 0);
    current_state = PLATFORM_TRANSFER_ERROR;
    return false;
}

static bool packet_is_valid(const TransferPacket *incoming) {
    return memcmp(incoming->magic, "CNXFER", 6) == 0 &&
           incoming->version == TRANSFER_VERSION &&
           incoming->custom_puzzle.group_names[0][0] != '\0';
}

static void build_chunks(void) {
    const u8 *packet_bytes = (const u8*)&packet;
    size_t packet_size = sizeof(packet);

    chunk_count = (int)((packet_size + TRANSFER_CHUNK_DATA_SIZE - 1) / TRANSFER_CHUNK_DATA_SIZE);
    if (chunk_count > TRANSFER_MAX_CHUNKS)
        chunk_count = TRANSFER_MAX_CHUNKS;

    for (int i = 0; i < chunk_count; i++) {
        size_t offset = (size_t)i * TRANSFER_CHUNK_DATA_SIZE;
        size_t remaining = packet_size - offset;
        size_t data_size = remaining > TRANSFER_CHUNK_DATA_SIZE ? TRANSFER_CHUNK_DATA_SIZE : remaining;

        memset(&chunks[i], 0, sizeof(chunks[i]));
        memcpy(chunks[i].magic, TRANSFER_DATA_ADVERT_MAGIC, 6);
        chunks[i].version = TRANSFER_VERSION;
        chunks[i].sequence = (u16)i;
        chunks[i].total = (u16)chunk_count;
        chunks[i].data_size = (u16)data_size;
        chunks[i].transfer_id = packet.transfer_id;
        memcpy(chunks[i].data, packet_bytes + offset, data_size);
    }

    next_chunk_index = 0;
    transfer_log("build chunks packet_size=%zu chunk_size=%d chunk_count=%d", packet_size, TRANSFER_CHUNK_DATA_SIZE, chunk_count);
}

static void reset_receive_chunks(void) {
    memset(receive_buffer, 0, sizeof(receive_buffer));
    memset(received_chunks, 0, sizeof(received_chunks));
    received_chunk_count = 0;
    expected_chunk_count = 0;
    active_receive_transfer_id = 0;
}

static void reset_receive_debug(void) {
    received_frame_count = 0;
    invalid_frame_count = 0;
    last_transfer_result = 0;
}

static bool chunk_is_valid(const TransferChunk *chunk) {
    return memcmp(chunk->magic, TRANSFER_DATA_ADVERT_MAGIC, 6) == 0 &&
           chunk->version == TRANSFER_VERSION &&
           chunk->transfer_id != 0 &&
           chunk->total > 0 &&
           chunk->total <= TRANSFER_MAX_CHUNKS &&
           chunk->sequence < chunk->total &&
           chunk->data_size <= TRANSFER_CHUNK_DATA_SIZE;
}

static void receive_chunk(const TransferChunk *chunk) {
    if (!chunk_is_valid(chunk)) {
        transfer_log(
            "chunk invalid magic=%.6s version=%u seq=%u total=%u size=%u",
            chunk->magic,
            (unsigned int)chunk->version,
            (unsigned int)chunk->sequence,
            (unsigned int)chunk->total,
            (unsigned int)chunk->data_size
        );
        return;
    }

    if (!mac_equal(chunk->target_bssid, receiver_self_bssid)) {
        transfer_log(
            "chunk ignored target=%06X self=%06X seq=%u",
            mac_tail(chunk->target_bssid),
            mac_tail(receiver_self_bssid),
            (unsigned int)chunk->sequence
        );
        return;
    }

    if (chunk->transfer_id == completed_transfer_id) {
        transfer_log("chunk ignored completed transfer=%u seq=%u", (unsigned int)chunk->transfer_id, (unsigned int)chunk->sequence);
        return;
    }

    if (active_receive_transfer_id != 0 && active_receive_transfer_id != chunk->transfer_id) {
        transfer_log("chunk transfer changed old=%u new=%u reset", (unsigned int)active_receive_transfer_id, (unsigned int)chunk->transfer_id);
        reset_receive_chunks();
    }

    active_receive_transfer_id = chunk->transfer_id;

    if (expected_chunk_count != 0 && expected_chunk_count != chunk->total) {
        transfer_log("chunk total changed old=%d new=%u reset", expected_chunk_count, (unsigned int)chunk->total);
        reset_receive_chunks();
        active_receive_transfer_id = chunk->transfer_id;
    }

    expected_chunk_count = chunk->total;

    if (!received_chunks[chunk->sequence]) {
        size_t offset = (size_t)chunk->sequence * TRANSFER_CHUNK_DATA_SIZE;

        if (offset + chunk->data_size <= sizeof(receive_buffer)) {
            memcpy(receive_buffer + offset, chunk->data, chunk->data_size);
            received_chunks[chunk->sequence] = true;
            received_chunk_count++;
            transfer_log(
                "chunk accepted seq=%u total=%u count=%d size=%u",
                (unsigned int)chunk->sequence,
                (unsigned int)chunk->total,
                received_chunk_count,
                (unsigned int)chunk->data_size
            );
        }
    }

    if (expected_chunk_count > 0 && received_chunk_count >= expected_chunk_count) {
        TransferPacket *incoming = (TransferPacket*)receive_buffer;

        if (packet_is_valid(incoming)) {
            received_puzzle = incoming->custom_puzzle;
            has_received_puzzle = true;
            completed_transfer_id = chunk->transfer_id;
            set_status("Puzzle received");
            transfer_log("packet complete valid id=%u name=%s", (unsigned int)completed_transfer_id, received_puzzle.name);
        } else {
            set_status("Reassembled invalid puzzle");
            transfer_log(
                "packet complete invalid magic=%.6s version=%u group0=%s",
                incoming->magic,
                (unsigned int)incoming->version,
                incoming->custom_puzzle.group_names[0]
            );
        }

        reset_receive_chunks();
    } else {
        char text[96];
        snprintf(text, sizeof(text), "Receiving chunks %d/%d", received_chunk_count, expected_chunk_count);
        set_status(text);
    }
}

static bool advert_is_receiver(const LdnNetworkInfo *network_info) {
    size_t prefix_size = strlen(TRANSFER_RECEIVER_ADVERT_PREFIX);

    return network_info->advertise_data_size > prefix_size &&
           memcmp(network_info->advertise_data, TRANSFER_RECEIVER_ADVERT_PREFIX, prefix_size) == 0;
}

bool platform_transfer_start_receiving(void) {
    Result rc;
    LdnSecurityConfig sec_config = {0};
    LdnUserConfig user_config = {0};
    LdnNetworkConfig network_config = {0};
    char advert[128];

    cleanup_transfer();
    reset_transfer_log("receive");
    clear_debug_lines();
    memset(&received_puzzle, 0, sizeof(received_puzzle));
    has_received_puzzle = false;
    receiver_count = 0;
    reset_receive_chunks();
    reset_receive_debug();

    rc = ldnInitialize(LdnServiceType_User);
    transfer_log("receive ldnInitialize=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnInitialize");
    ldn_initialized = true;

    load_user_name(&user_config);
    init_network_config(&network_config);
    init_security_config(&sec_config);

    rc = ldnOpenAccessPoint();
    transfer_log("receive ldnOpenAccessPoint=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnOpenAccessPoint");

    rc = ldnCreateNetwork(&sec_config, &user_config, &network_config);
    transfer_log("receive ldnCreateNetwork=%08X user=%s", (unsigned int)rc, user_config.user_name);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnCreateNetwork");
    LdnNetworkInfo self_info = {0};
    if (R_SUCCEEDED(ldnGetNetworkInfo(&self_info))) {
        receiver_self_bssid = self_info.common.bssid;
        transfer_log(
            "receive self bssid=%06X channel=%d nodes=%d",
            mac_tail(receiver_self_bssid),
            (int)self_info.common.channel,
            (int)self_info.node_count
        );
    }
    capture_network_debug("receiver", 1);

    snprintf(advert, sizeof(advert), "%s%s", TRANSFER_RECEIVER_ADVERT_PREFIX, user_config.user_name);
    rc = ldnSetAdvertiseData(advert, strlen(advert) + 1);
    transfer_log("receive ldnSetAdvertiseData=%08X advert=%s", (unsigned int)rc, advert);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnSetAdvertiseData");

    rc = ldnSetStationAcceptPolicy(LdnAcceptPolicy_AlwaysAccept);
    transfer_log("receive ldnSetStationAcceptPolicy=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnAcceptPolicy");

    current_state = PLATFORM_TRANSFER_RECEIVING;
    set_status("Open to receive puzzles");
    capture_state_debug("receiver", 0);
    return true;
}

bool platform_transfer_scan_receivers_for_custom_puzzle(const GameSaveCustomPuzzle *custom_puzzle) {
    Result rc;

    if (!custom_puzzle)
        return false;

    cleanup_transfer();
    reset_transfer_log("send");
    clear_debug_lines();
    memset(&packet, 0, sizeof(packet));
    memcpy(packet.magic, "CNXFER", 6);
    packet.version = TRANSFER_VERSION;
    packet.transfer_id = SDL_GetTicks();
    if (packet.transfer_id == 0)
        packet.transfer_id = 1;
    packet.custom_puzzle = *custom_puzzle;
    build_chunks();
    receiver_count = 0;
    last_receiver_seen_at = 0;
    has_received_puzzle = false;
    reset_receive_debug();

    rc = ldnInitialize(LdnServiceType_User);
    transfer_log("send ldnInitialize=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnInitialize");
    ldn_initialized = true;

    rc = ldnOpenStation();
    transfer_log("send ldnOpenStation=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnOpenStation");
    capture_state_debug("sender", 0);

    started_at = SDL_GetTicks();
    last_scan_at = 0;
    current_state = PLATFORM_TRANSFER_SCANNING;
    set_status("Scanning for receivers");
    return true;
}

static void scan_for_receivers(void) {
    Result rc;
    LdnScanFilter filter = {0};
    LdnNetworkInfo networks[0x18];
    s32 total_out = 0;

    init_scan_filter(&filter);

    memset(networks, 0, sizeof(networks));
    rc = ldnScan(0, &filter, networks, 0x18, &total_out);
    last_transfer_result = rc;
    transfer_log("scan rc=%08X total=%d", (unsigned int)rc, (int)total_out);
    if (R_FAILED(rc)) {
        char text[96];
        snprintf(text, sizeof(text), "ldnScan failed: %08X", (unsigned int)rc);
        set_status(text);
        return;
    }

    LdnNetworkInfo found_networks[TRANSFER_MAX_RECEIVERS];
    char found_names[TRANSFER_MAX_RECEIVERS][0x21];
    int found_count = 0;
    memset(found_networks, 0, sizeof(found_networks));
    memset(found_names, 0, sizeof(found_names));

    for (int i = 0; i < total_out && found_count < TRANSFER_MAX_RECEIVERS; i++) {
        if (!advert_is_receiver(&networks[i]))
        {
            continue;
        }

        found_networks[found_count] = networks[i];
        size_t prefix_size = strlen(TRANSFER_RECEIVER_ADVERT_PREFIX);
        const char *name = (const char*)networks[i].advertise_data + prefix_size;
        strncpy(found_names[found_count], name, sizeof(found_names[found_count]));
        found_names[found_count][sizeof(found_names[found_count]) - 1] = '\0';

        if (found_names[found_count][0] == '\0' && networks[i].node_count > 0) {
            strncpy(found_names[found_count], networks[i].nodes[0].user_name, sizeof(found_names[found_count]));
            found_names[found_count][sizeof(found_names[found_count]) - 1] = '\0';
        }

        if (found_names[found_count][0] == '\0')
            snprintf(found_names[found_count], sizeof(found_names[found_count]), "Receiver %d", found_count + 1);

        transfer_log(
            "scan match index=%d receiver=%d name=%s ch=%d nodes=%d bssid=%06X lcid=%ld scene=%d",
            i,
            found_count,
            found_names[found_count],
            (int)networks[i].common.channel,
            (int)networks[i].node_count,
            mac_tail(networks[i].common.bssid),
            networks[i].network_id.intent_id.local_communication_id,
            (int)networks[i].network_id.intent_id.scene_id
        );
        found_count++;
    }

    if (found_count > 0) {
        char text[96];
        receiver_count = found_count;
        memcpy(receiver_networks, found_networks, sizeof(receiver_networks));
        memcpy(receiver_names, found_names, sizeof(receiver_names));
        last_receiver_seen_at = SDL_GetTicks();

        snprintf(
            text,
            sizeof(text),
            "Seen %d, found %d receiver%s",
            total_out,
            receiver_count,
            receiver_count == 1 ? "" : "s"
        );
        set_status(text);
        snprintf(
            text,
            sizeof(text),
            "match ch%d n%d b%06X id%ld sc%d",
            (int)receiver_networks[0].common.channel,
            (int)receiver_networks[0].node_count,
            mac_tail(receiver_networks[0].common.bssid),
            receiver_networks[0].network_id.intent_id.local_communication_id,
            (int)receiver_networks[0].network_id.intent_id.scene_id
        );
        set_debug_line(1, text);
    } else {
        char text[96];
        Uint32 now = SDL_GetTicks();

        if (receiver_count > 0 && now - last_receiver_seen_at < TRANSFER_RECEIVER_STALE_MS) {
            snprintf(text, sizeof(text), "Seen %d, keeping last receiver", total_out);
            set_status(text);
        } else {
            receiver_count = 0;
            snprintf(text, sizeof(text), "Seen %d, no receivers", total_out);
            set_status(text);
            snprintf(text, sizeof(text), "scan total %d rc %08X", total_out, (unsigned int)rc);
            set_debug_line(1, text);
        }
    }
}

static void scan_for_data_chunks(void) {
    Result rc;
    LdnScanFilter filter = {0};
    LdnNetworkInfo networks[0x18];
    s32 total_out = 0;

    init_scan_filter(&filter);
    memset(networks, 0, sizeof(networks));

    rc = ldnScan(0, &filter, networks, 0x18, &total_out);
    last_transfer_result = rc;
    if (R_FAILED(rc)) {
        char text[96];
        snprintf(text, sizeof(text), "data scan rc %08X", (unsigned int)rc);
        set_status(text);
        transfer_log("%s", text);
        return;
    }

    if (total_out > 0)
        transfer_log("data scan rc=%08X total=%d", (unsigned int)rc, (int)total_out);

    for (int i = 0; i < total_out; i++) {
        if (networks[i].advertise_data_size < sizeof(TransferChunk))
            continue;

        TransferChunk chunk;
        memcpy(&chunk, networks[i].advertise_data, sizeof(chunk));
        if (memcmp(chunk.magic, TRANSFER_DATA_ADVERT_MAGIC, 6) != 0)
            continue;

        received_frame_count++;
        transfer_log(
            "data advert frame=%d from=%06X size=%d seq=%u total=%u target=%06X",
            received_frame_count,
            mac_tail(networks[i].common.bssid),
            (int)networks[i].advertise_data_size,
            (unsigned int)chunk.sequence,
            (unsigned int)chunk.total,
            mac_tail(chunk.target_bssid)
        );
        receive_chunk(&chunk);
    }
}

bool platform_transfer_send_to_receiver(int receiver_index) {
    Result rc;
    LdnSecurityConfig sec_config = {0};
    LdnUserConfig user_config = {0};
    LdnNetworkConfig network_config = {0};

    if (current_state != PLATFORM_TRANSFER_SCANNING ||
        receiver_index < 0 ||
        receiver_index >= receiver_count) {
        return false;
    }

    target_bssid = receiver_networks[receiver_index].common.bssid;
    target_destination = receiver_networks[receiver_index].common.bssid;
    if (receiver_networks[receiver_index].node_count > 0)
        target_destination = receiver_networks[receiver_index].nodes[0].mac_addr;

    target_channel = receiver_networks[receiver_index].common.channel;
    if (target_channel == 0)
        return fail_runtime("receiver channel missing");
    set_chunk_targets(target_bssid);

    char text[96];
    snprintf(
        text,
        sizeof(text),
        "target ch%d dst%06X b%06X",
        (int)target_channel,
        mac_tail(target_destination),
        mac_tail(target_bssid)
    );
    set_debug_line(2, text);
    transfer_log("selected receiver=%d %s", receiver_index, text);

    rc = ldnCloseStation();
    transfer_log("send ldnCloseStation=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnCloseStation");

    load_user_name(&user_config);
    init_network_config(&network_config);
    init_security_config(&sec_config);

    rc = ldnOpenAccessPoint();
    transfer_log("send ldnOpenAccessPoint=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnOpenAccessPoint");

    rc = ldnCreateNetwork(&sec_config, &user_config, &network_config);
    transfer_log("send ldnCreateNetwork=%08X", (unsigned int)rc);
    if (R_FAILED(rc))
        return fail_start(rc, "ldnCreateNetwork");

    started_at = SDL_GetTicks();
    last_send_at = 0;
    current_state = PLATFORM_TRANSFER_SENDING;
    set_status("Sending action frame");
    return true;
}

void platform_transfer_update(void) {
    Uint32 now = SDL_GetTicks();

    if (current_state == PLATFORM_TRANSFER_SCANNING) {
        if (last_scan_at == 0 || now - last_scan_at >= TRANSFER_SCAN_INTERVAL_MS) {
            scan_for_receivers();
            last_scan_at = now;
        }

        return;
    }

    if (current_state == PLATFORM_TRANSFER_RECEIVING) {
        if (last_scan_at == 0 || now - last_scan_at >= TRANSFER_SCAN_INTERVAL_MS) {
            scan_for_data_chunks();
            last_scan_at = now;
        }

        return;
    }

    if (current_state != PLATFORM_TRANSFER_SENDING)
        return;

    if (now - started_at >= TRANSFER_SEND_DURATION_MS) {
        cleanup_transfer();
        current_state = PLATFORM_TRANSFER_DONE;
        set_status("Puzzle sent");
        return;
    }

    if (last_send_at == 0 || now - last_send_at >= TRANSFER_SEND_INTERVAL_MS) {
        char text[96];
        Result rc = ldnSetAdvertiseData(
            &chunks[next_chunk_index],
            sizeof(chunks[next_chunk_index])
        );
        last_transfer_result = rc;
        snprintf(text, sizeof(text), "advert rc %08X idx%d/%d", (unsigned int)rc, next_chunk_index + 1, chunk_count);
        set_debug_line(3, text);
        transfer_log(
            "send advertise rc=%08X idx=%d/%d size=%zu target=%06X",
            (unsigned int)rc,
            next_chunk_index + 1,
            chunk_count,
            sizeof(chunks[next_chunk_index]),
            mac_tail(target_bssid)
        );

        if (R_FAILED(rc)) {
            char text[96];
            snprintf(text, sizeof(text), "advertise failed %08X", (unsigned int)rc);
            fail_runtime(text);
            return;
        }

        next_chunk_index = (next_chunk_index + 1) % chunk_count;
        last_send_at = now;
        set_status("Advertising puzzle chunks");
    }
}

void platform_transfer_cancel(void) {
    cleanup_transfer();
    current_state = PLATFORM_TRANSFER_IDLE;
    receiver_count = 0;
    set_status("Idle");
}

PlatformTransferState platform_transfer_state(void) {
    return current_state;
}

const char *platform_transfer_status(void) {
    return status_text;
}

int platform_transfer_seconds_remaining(void) {
    if (current_state != PLATFORM_TRANSFER_SENDING)
        return 0;

    Uint32 now = SDL_GetTicks();
    Uint32 elapsed = now - started_at;

    if (elapsed >= TRANSFER_SEND_DURATION_MS)
        return 0;

    return (int)((TRANSFER_SEND_DURATION_MS - elapsed + 999) / 1000);
}

bool platform_transfer_take_received_custom_puzzle(GameSaveCustomPuzzle *out_custom_puzzle) {
    if (!out_custom_puzzle || !has_received_puzzle)
        return false;

    *out_custom_puzzle = received_puzzle;
    has_received_puzzle = false;
    transfer_log("take received puzzle name=%s", out_custom_puzzle->name);
    return true;
}

int platform_transfer_receiver_count(void) {
    return receiver_count;
}

const char *platform_transfer_receiver_name(int receiver_index) {
    if (receiver_index < 0 || receiver_index >= receiver_count)
        return "";

    return receiver_names[receiver_index];
}
