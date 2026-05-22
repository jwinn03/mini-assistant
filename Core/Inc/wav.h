#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include "ff.h"

/* Minimal WAV (RIFF/WAVE) reader/writer for 16-bit PCM stereo at 48 kHz —
   the only format this project records or plays. A full WAV decoder would
   handle floats, mu-law, multi-channel, etc.; we deliberately don't.

   Header layout (44 bytes):
     0:  "RIFF"
     4:  uint32 LE  riff_size  = file_size - 8
     8:  "WAVE"
     12: "fmt "
     16: uint32 LE  fmt_size   = 16
     20: uint16 LE  format     = 1 (PCM)
     22: uint16 LE  channels   = 2
     24: uint32 LE  sample_rate
     28: uint32 LE  byte_rate  = sample_rate * channels * bits/8
     32: uint16 LE  block_align= channels * bits/8
     34: uint16 LE  bits/sample= 16
     36: "data"
     40: uint32 LE  data_size  = data_bytes
     44: <audio samples...>
*/

#define WAV_HEADER_BYTES   44u
#define WAV_FS_HZ          48000u
#define WAV_CHANNELS       2u
#define WAV_BITS_PER_SAMP  16u
#define WAV_BYTES_PER_FRAME (WAV_CHANNELS * (WAV_BITS_PER_SAMP / 8u))   /* 4 */
#define WAV_BYTE_RATE      (WAV_FS_HZ * WAV_BYTES_PER_FRAME)             /* 192000 */

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_bytes;       /* size of the PCM payload */
    uint32_t data_offset;      /* file offset where samples begin */
} wav_info_t;

/* Write a 44-byte placeholder header with data_size=0 and riff_size=36.
   Call wav_finalize() before closing to patch the size fields. Returns FR_OK
   on success. The file must be opened with FA_WRITE. */
FRESULT wav_write_header(FIL *fp);

/* Patch the riff_size and data_size fields. data_bytes is the total payload
   written since the header. Leaves the cursor at end-of-data so further
   writes are not needed. Returns FR_OK on success. Cost: ~1 ms (two
   f_lseek + two short f_write + one f_sync). */
FRESULT wav_finalize(FIL *fp, uint32_t data_bytes);

/* Open file for read and parse + validate the WAVE header against our
   single supported format. On success, fp is positioned at data_offset
   and info is populated. Returns FR_OK on success, FR_INVALID_OBJECT on
   format mismatch. */
FRESULT wav_read_open(FIL *fp, const char *path, wav_info_t *info);

#endif
