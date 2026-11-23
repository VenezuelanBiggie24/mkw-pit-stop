// Game-free placeholder and TCP receiver screen shown on PS5 when game code or game files are missing/outdated.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Displays the missing-game-files / build-needed screen, accepts files from the PC Pit Stop builder on port 19913
// strictly under root_dir (e.g. "/app0"), and returns once the user exits after completion or cancellation.
// update: the game files are fine and the player asked for the screen (ps5_update_requested) to send a new build;
// Cross / Circle then also closes it while waiting.
void ps5_placeholder_run(const char *root_dir, const char *port_version, const char *rt_game_id, int has_game_code,
                         const char *reason, int update);

// L1 + R1 held while the title starts: the player wants the receiver screen to update a working install from the PC
// builder (without it a working title starts the game and never listens). Waits for the pad's first reading, at most
// UPDATE_WAIT_MS.
int ps5_update_requested(void);

#ifdef __cplusplus
}
#endif
