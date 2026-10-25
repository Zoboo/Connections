#ifndef CONNECTIONS_MULTIPLAYER_H
#define CONNECTIONS_MULTIPLAYER_H
#include <stdbool.h>
#include <stdint.h>
#include "game/connections_puzzle.h"
#define MP_MAX_PLAYERS 8
#define MP_MAX_PUZZLES 20000
#define MP_NAME_LENGTH 32
#define MP_ICON_SIZE 32
#define MP_ICON_BYTES (MP_ICON_SIZE * MP_ICON_SIZE * 4)
typedef enum { MP_INACTIVE, MP_DISCOVERING, MP_CONNECTING, MP_LOBBY, MP_PREPARING, MP_STARTING, MP_PLAYING, MP_FINALIZING, MP_RESULTS, MP_ERROR } MpState;
typedef enum { MP_TIMED, MP_TARGET_SCORE } MpMode;
typedef struct { MpMode mode; uint32_t duration_seconds, target_score; } MpRules;
typedef struct { uint64_t id; char name[MP_NAME_LENGTH + 1]; uint32_t score, puzzle_count, solved_count; uint64_t elapsed_us; bool connected, ready; uint8_t color_id; uint32_t profile_color, icon_revision; uint8_t icon_rgba[MP_ICON_BYTES]; } MpPlayer;
typedef struct { MpState state; bool is_host; uint64_t own_id, match_id, start_ticks; MpRules rules; unsigned player_count, winner_mask; MpPlayer players[MP_MAX_PLAYERS]; char error[96]; } MpSnapshot;
bool mp_init(void);
/* Shared socket-driver ownership for background update and multiplayer workers. */
bool mp_socket_acquire(void);
void mp_socket_release(void);
void mp_shutdown(void);
bool mp_begin_local(const char *name);
bool mp_begin_wifi(bool host, const char *ip, uint16_t port, const char *name);
void mp_stop(void); /* Queued cancellation: never joins on the game thread. */
bool mp_set_rules(MpRules rules);
bool mp_set_puzzle_count(unsigned count);
/* Main-thread-only borrowed library; retain until unregistering with NULL/0/0.
   Only its usable count is queued to the networking worker. */
bool mp_set_library(const ConnectionsPuzzle *puzzles, unsigned count, unsigned entries);
/* Prepares the local sequence during PREPARING, then returns cached puzzles.
   No file or network access; call on the main thread before sending READY. */
bool mp_race_puzzle(uint64_t match_id, unsigned index, ConnectionsPuzzle *out);
bool mp_return_lobby(void);
bool mp_set_solved_count(uint32_t count);
/* Cached 32x32 RGBA thumbnail, sent once per change through bounded TCP chunks. */
bool mp_set_appearance(uint8_t color_id, uint32_t profile_color, const uint8_t rgba[MP_ICON_BYTES]);
bool mp_prepare_match(void);
bool mp_ready(bool usable_puzzles);
bool mp_complete(uint32_t score, uint64_t elapsed_us);
/* Atomic revision gate; takes a short snapshot lock only when state changes. */
bool mp_read_snapshot(MpSnapshot *snapshot);
#endif
