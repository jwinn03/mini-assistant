#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>
#include <stdbool.h>

/* Thin wrapper around the CubeMX FATFS plumbing (FATFS/App + Target). The
   FATFS object, SDPath, and SD driver are owned by MX_FATFS_Init() in
   FATFS/App/fatfs.c; this module's job is to:
     - mount the volume after init
     - track mount state and surface it to the UI
     - scan the root for REC*.WAV and report the next sequential filename
     - re-mount on hot-insert (polled by the recorder/UI tasks)

   PC13 detect is read inside FATFS via fatfs_platform.c::BSP_PlatformIsDetected.
   No GPIO code lives here. */

#define SD_REC_FILENAME_MAX  13     /* "RECNNN.WAV" + NUL fits in 13 (8.3) */

/* Call once from StartDefaultTask after FATFS init (kernel must be running
   so the sd_diskio osMessageQueue can be created). Attempts mount; if the
   card is absent, leaves the volume unmounted and returns. */
void sd_card_init(void);

/* True if FATFS is currently mounted. UI reads this each frame to decide
   whether to enable REC/PLAY buttons. Cheap (single bool load). */
bool sd_card_mounted(void);

/* Re-checks card presence and attempts mount/unmount transitions. Safe to
   call from a low-priority task — does NOT block the audio path. Returns
   true if the mount state changed (UI uses this to invalidate its file list).
   On removal: actively aborts any in-flight DMA via HAL_SD_Abort_IT, asks the
   recorder/player to stop, takes the FATFS mutex once they're idle, and
   unregisters the volume. UI sees s_mounted = false within one poll tick,
   so its FATFS-touching code paths short-circuit before the slow cleanup
   completes. */
bool sd_card_poll(void);

/* Mutex around all FATFS access. recorder, player, sd_card, and any UI
   refresh path must take this before calling f_open / f_write / f_read /
   f_lseek / f_sync / f_close / f_mount / f_opendir / f_readdir / f_closedir,
   and release it immediately after. The audio task never touches FATFS so it
   never takes this lock — priority inversion is therefore impossible. */
void sd_card_lock(void);
void sd_card_unlock(void);

/* Fills out with the next "RECNNN.WAV" filename (uppercase, 8.3) based on the
   highest existing REC index scanned at mount. Buffer must be >= 13 bytes.
   Returns true on success; false if not mounted or index space exhausted. */
bool sd_card_next_filename(char *out);

/* Snapshot the most recent N recordings (oldest first) for the UI file list.
   max is the slot count in list; *count receives the number actually filled.
   Returns true if mounted (count may still be 0). Re-scans the directory
   on each call — only invoked from the UI task at low rate so the cost is
   acceptable. */
bool sd_card_scan_recordings(char list[][SD_REC_FILENAME_MAX], int max, int *count);

#endif
