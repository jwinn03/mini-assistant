#include "wav.h"
#include <string.h>
#include <stdbool.h>

/* Little-endian writers. STM32F7 is LE, but using memcpy of integers would
   tie the on-disk format to host endianness — explicit byte writes keep the
   header byte-exact regardless. */
static inline void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v       & 0xFF);
    p[1] = (uint8_t)((v >> 8)  & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v       & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline uint32_t get_u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

FRESULT wav_write_header(FIL *fp)
{
    uint8_t hdr[WAV_HEADER_BYTES];

    memcpy(&hdr[0],  "RIFF", 4);
    put_u32_le(&hdr[4], 36u);                   /* placeholder: header-only, patched at finalize */
    memcpy(&hdr[8],  "WAVE", 4);
    memcpy(&hdr[12], "fmt ", 4);
    put_u32_le(&hdr[16], 16u);                  /* fmt chunk size */
    put_u16_le(&hdr[20], 1u);                   /* PCM */
    put_u16_le(&hdr[22], WAV_CHANNELS);
    put_u32_le(&hdr[24], WAV_FS_HZ);
    put_u32_le(&hdr[28], WAV_BYTE_RATE);
    put_u16_le(&hdr[32], WAV_BYTES_PER_FRAME);
    put_u16_le(&hdr[34], WAV_BITS_PER_SAMP);
    memcpy(&hdr[36], "data", 4);
    put_u32_le(&hdr[40], 0u);                   /* placeholder: patched at finalize */

    UINT written = 0;
    FRESULT r = f_write(fp, hdr, WAV_HEADER_BYTES, &written);
    if (r != FR_OK) return r;
    if (written != WAV_HEADER_BYTES) return FR_DISK_ERR;
    return FR_OK;
}

FRESULT wav_finalize(FIL *fp, uint32_t data_bytes)
{
    uint8_t buf[4];
    UINT written = 0;
    FRESULT r;

    /* RIFF size at offset 4 = total file size minus 8 = 36 + data_bytes. */
    r = f_lseek(fp, 4);
    if (r != FR_OK) return r;
    put_u32_le(buf, 36u + data_bytes);
    r = f_write(fp, buf, 4, &written);
    if (r != FR_OK) return r;

    /* data chunk size at offset 40. */
    r = f_lseek(fp, 40);
    if (r != FR_OK) return r;
    put_u32_le(buf, data_bytes);
    r = f_write(fp, buf, 4, &written);
    if (r != FR_OK) return r;

    /* Force the FAT and directory entries to disk so a yanked card still
       presents a valid file. Costly (issues an SD flush) but called once
       at close + periodically during long recordings. */
    return f_sync(fp);
}

/* Parse a WAVE header. We accept only PCM 16-bit stereo 48 kHz. To survive
   chunks between "fmt " and "data" (LIST/INFO, JUNK alignment), we scan
   chunks one by one — many tools emit them. */
FRESULT wav_read_open(FIL *fp, const char *path, wav_info_t *info)
{
    FRESULT r = f_open(fp, path, FA_READ);
    if (r != FR_OK) return r;

    uint8_t hdr[12];
    UINT read = 0;
    r = f_read(fp, hdr, 12, &read);
    if (r != FR_OK || read != 12) { f_close(fp); return FR_INVALID_OBJECT; }

    if (memcmp(&hdr[0], "RIFF", 4) != 0 ||
        memcmp(&hdr[8], "WAVE", 4) != 0) {
        f_close(fp);
        return FR_INVALID_OBJECT;
    }

    bool got_fmt  = false;
    bool got_data = false;
    info->sample_rate     = 0;
    info->channels        = 0;
    info->bits_per_sample = 0;
    info->data_bytes      = 0;
    info->data_offset     = 0;

    /* Walk subchunks until we find both "fmt " and "data". */
    while (!got_data) {
        uint8_t ch[8];
        r = f_read(fp, ch, 8, &read);
        if (r != FR_OK || read != 8) { f_close(fp); return FR_INVALID_OBJECT; }

        uint32_t ck_size = get_u32_le(&ch[4]);

        if (memcmp(ch, "fmt ", 4) == 0) {
            if (ck_size < 16) { f_close(fp); return FR_INVALID_OBJECT; }
            uint8_t fmt[16];
            r = f_read(fp, fmt, 16, &read);
            if (r != FR_OK || read != 16) { f_close(fp); return FR_INVALID_OBJECT; }

            uint16_t format     = get_u16_le(&fmt[0]);
            info->channels      = get_u16_le(&fmt[2]);
            info->sample_rate   = get_u32_le(&fmt[4]);
            info->bits_per_sample = get_u16_le(&fmt[14]);

            if (format != 1 ||
                info->channels      != WAV_CHANNELS ||
                info->sample_rate   != WAV_FS_HZ ||
                info->bits_per_sample != WAV_BITS_PER_SAMP) {
                f_close(fp);
                return FR_INVALID_OBJECT;
            }
            /* Skip any fmt extension bytes past the 16 we consumed. */
            if (ck_size > 16) {
                r = f_lseek(fp, f_tell(fp) + (ck_size - 16));
                if (r != FR_OK) { f_close(fp); return FR_INVALID_OBJECT; }
            }
            got_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!got_fmt) { f_close(fp); return FR_INVALID_OBJECT; }
            info->data_bytes  = ck_size;
            info->data_offset = f_tell(fp);
            got_data = true;
        } else {
            /* Skip unknown chunk (LIST, JUNK, etc.) — chunks are word-aligned
               so size 1 means 2 bytes on disk. */
            uint32_t skip = (ck_size + 1u) & ~1u;
            r = f_lseek(fp, f_tell(fp) + skip);
            if (r != FR_OK) { f_close(fp); return FR_INVALID_OBJECT; }
        }
    }

    return FR_OK;
}
