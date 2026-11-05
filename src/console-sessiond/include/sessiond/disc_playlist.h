#ifndef SESSIOND_DISC_PLAYLIST_H
#define SESSIOND_DISC_PLAYLIST_H
#include <stdbool.h>
#include <stddef.h>

#define CS_M3U_DISCS 99U
#define CS_M3U_HEADER "# Zero28 multi-disc\n"
#define CS_DISC_STATE_TRAILER 16U
#define CS_PS_PLAYLIST_DISCS 8U
#define CS_DISC_PLAYLIST_POLICY "clove-ordered-playlist-disc-state-v1"

/* Managed playlists name shallow local files below the playlist directory. */
bool cs_m3u_read(int directory, const char *name, char rows[CS_M3U_DISCS][512], size_t *count);

/* Commit to the ordered playlist and every image/descriptor/track byte.
 * Opens each dependency without following symlinks or crossing devices. */
bool cs_disc_playlist_digest(const char *path, char digest[65], unsigned int *discs);
bool cs_disc_ps_playlist_digest(const char *path, char digest[65], unsigned int *discs);
bool cs_disc_playlist_path(const char *path);
void cs_disc_frozen_config(const char config[65], const char options[65], char digest[65]);

/* Playlist checkpoint states append this trailer after the native RASTATE.
 * Single-disc states keep the existing binary format. */
bool cs_disc_state_encode(unsigned char trailer[CS_DISC_STATE_TRAILER], unsigned int discs,
                          unsigned int active);
bool cs_disc_state_decode(const unsigned char trailer[CS_DISC_STATE_TRAILER], unsigned int discs,
                          unsigned int *active);
#endif
