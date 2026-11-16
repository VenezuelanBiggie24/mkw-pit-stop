// Game-free placeholder and TCP receiver screen shown on PS5 when game code or game files are missing/outdated.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Displays the missing-game-files / build-needed screen, accepts files from the PC Pit Stop builder on port 19913
// strictly under root_dir (e.g. "/app0"), and returns once the user exits after completion or cancellation.
void ps5_placeholder_run(const char *root_dir, const char *port_version, const char *rt_game_id, int has_game_code,
                         const char *reason);

#ifdef __cplusplus
}
#endif
