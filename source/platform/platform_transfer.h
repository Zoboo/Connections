#pragma once

#include <stdbool.h>

#include "platform_save.h"

typedef enum {
    PLATFORM_TRANSFER_IDLE,
    PLATFORM_TRANSFER_SCANNING,
    PLATFORM_TRANSFER_SENDING,
    PLATFORM_TRANSFER_RECEIVING,
    PLATFORM_TRANSFER_DONE,
    PLATFORM_TRANSFER_ERROR
} PlatformTransferState;

bool platform_transfer_scan_receivers_for_custom_puzzle(const GameSaveCustomPuzzle *custom_puzzle);
bool platform_transfer_send_to_receiver(int receiver_index);
bool platform_transfer_start_receiving(void);
void platform_transfer_update(void);
void platform_transfer_cancel(void);

PlatformTransferState platform_transfer_state(void);
const char *platform_transfer_status(void);
int platform_transfer_seconds_remaining(void);
bool platform_transfer_take_received_custom_puzzle(GameSaveCustomPuzzle *out_custom_puzzle);
int platform_transfer_receiver_count(void);
const char *platform_transfer_receiver_name(int receiver_index);
