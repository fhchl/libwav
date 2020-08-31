#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wav.h"

#if defined(__x86_64) || defined(__amd64) || defined(__i386__) || defined(__x86_64__) || defined(__LITTLE_ENDIAN__)
#define WAV_ENDIAN_LITTLE 1
#elif defined(__BIG_ENDIAN__)
#define WAV_ENDIAN_BIG 1
#endif

#if WAV_ENDIAN_LITTLE
#define WAV_RIFF_CHUNK_ID       'FFIR'
#define WAV_FORMAT_CHUNK_ID     ' tmf'
#define WAV_FACT_CHUNK_ID       'tcaf'
#define WAV_DATA_CHUNK_ID       'atad'
#define WAV_WAVE_ID             'EVAW'
#endif

#if WAV_ENDIAN_BIG
#define WAV_RIFF_CHUNK_ID       'RIFF'
#define WAV_FORMAT_CHUNK_ID     'fmt '
#define WAV_FACT_CHUNK_ID       'fact'
#define WAV_DATA_CHUNK_ID       'data'
#define WAV_WAVE_ID             'WAVE'
#endif

static void* wav_default_aligned_alloc(void *context, size_t alignment, size_t size)
{
    (void)context;
    alignment = (alignment + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
#ifdef _WIN32
    void *p = _aligned_malloc(size, alignment);
    assert(p != NULL);
#else
    void *p = NULL;
    int err = posix_memalign(&p, alignment, size);
    assert(err == 0);
#endif
    return p;
}

static void* wav_default_realloc(void *context, void *p, size_t size)
{
    (void)context;
    void *ptr = realloc(p, size);
    assert(ptr != NULL);
    return ptr;
}

static void wav_default_free(void *context, void *p)
{
    (void)context;
    free(p);
}

static WavAllocFuncs g_default_alloc_funcs = {
    &wav_default_aligned_alloc,
    &wav_default_realloc,
    &wav_default_free
};

static void *g_alloc_context = NULL;
static WAV_CONST WavAllocFuncs* g_alloc_funcs = &g_default_alloc_funcs;

void wav_set_allocator(void *context, WAV_CONST WavAllocFuncs *funcs)
{
    g_alloc_context = context;
    g_alloc_funcs = funcs;
}

void* wav_aligned_alloc(size_t alignment, size_t size)
{
    return g_alloc_funcs->aligned_alloc(g_alloc_context, alignment, size);
}

void* wav_realloc(void *p, size_t size)
{
    return g_alloc_funcs->realloc(g_alloc_context, p, size);
}

void wav_free(void *p)
{
    if (p != NULL) {
        g_alloc_funcs->free(g_alloc_context, p);
    }
}

char* wav_strdup(WAV_CONST char *str)
{
    size_t len = strlen(str) + 1;
    void *new = WAV_NEW(char, len);
    if (new == NULL)
        return NULL;

    return memcpy(new, str, len);
}

#pragma pack(push, 1)

#define WAV_RIFF_HEADER_SIZE 8

typedef struct {
    /* RIFF header */
    uint32_t id;
    uint32_t size;

    uint16_t format_tag;
    uint16_t n_channels;
    uint32_t sample_rate;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;

    uint16_t ext_size;
    uint16_t valid_bits_per_sample;
    uint32_t channel_mask;

    uint8_t sub_format[16];
} WavFormatChunk;

typedef struct {
    /* RIFF header */
    uint32_t id;
    uint32_t size;

    uint32_t sample_length;
} WavFactChunk;

typedef struct {
    /* RIFF header */
    uint32_t id;
    uint32_t size;
} WavDataChunk;

typedef struct {
    /* RIFF header */
    uint32_t id;
    uint32_t size;

    uint32_t wave_id;

    WavFormatChunk format_chunk;
    WavFactChunk   fact_chunk;
    WavDataChunk   data_chunk;
} WavMasterChunk;

#pragma pack(pop)

struct _WavFile {
    FILE*               fp;
    char*               filename;
    WAV_CONST char*     mode;
    WavErr              error_code;
    WavMasterChunk      chunk;
    uint8_t*            tmp;
    size_t              tmp_size;
};

static WAV_CONST uint8_t default_sub_format[16] = {
    0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

size_t wav_get_header_size(WAV_CONST WavFile* self)
{
    size_t header_size = WAV_RIFF_HEADER_SIZE + 4 +
                         WAV_RIFF_HEADER_SIZE + self->chunk.format_chunk.size +
                         WAV_RIFF_HEADER_SIZE;

    if (self->chunk.fact_chunk.id == WAV_FACT_CHUNK_ID) {
        header_size += WAV_RIFF_HEADER_SIZE + self->chunk.fact_chunk.size;
    }

    return header_size;
}

void wav_parse_header(WavFile* self)
{
    size_t read_count;

    read_count = fread(&self->chunk, WAV_RIFF_HEADER_SIZE, 1, self->fp);
    if (read_count != 1 || self->chunk.id != WAV_RIFF_CHUNK_ID) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    read_count = fread(&self->chunk.wave_id, 4, 1, self->fp);
    if (read_count != 1 || self->chunk.wave_id != WAV_WAVE_ID) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    read_count = fread(&self->chunk.format_chunk, WAV_RIFF_HEADER_SIZE, 1, self->fp);
    if (read_count != 1 || self->chunk.format_chunk.id != WAV_FORMAT_CHUNK_ID) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    read_count = fread((uint8_t*)(&self->chunk.format_chunk) + WAV_RIFF_HEADER_SIZE,
                       self->chunk.format_chunk.size, 1, self->fp);
    if (read_count != 1) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    if (self->chunk.format_chunk.format_tag == WAV_FORMAT_PCM) {
    } else if (self->chunk.format_chunk.format_tag == WAV_FORMAT_IEEE_FLOAT) {
    } else if (self->chunk.format_chunk.format_tag == WAV_FORMAT_ALAW ||
               self->chunk.format_chunk.format_tag == WAV_FORMAT_MULAW) {
    } else {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    read_count = fread(&self->chunk.fact_chunk, WAV_RIFF_HEADER_SIZE, 1, self->fp);
    if (read_count != 1) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    if (self->chunk.fact_chunk.id == WAV_FACT_CHUNK_ID) {
        read_count = fread((uint8_t*)(&self->chunk.fact_chunk) + WAV_RIFF_HEADER_SIZE,
                           self->chunk.fact_chunk.size, 1, self->fp);
        if (read_count != 1) {
            self->error_code = WAV_ERR_FORMAT;
            return;
        }

        read_count = fread(&self->chunk.data_chunk, WAV_RIFF_HEADER_SIZE, 1, self->fp);
        if (read_count != 1) {
            self->error_code = WAV_ERR_FORMAT;
            return;
        }
    } else if (self->chunk.fact_chunk.id == WAV_DATA_CHUNK_ID) {
        self->chunk.data_chunk.id   = self->chunk.fact_chunk.id;
        self->chunk.data_chunk.size = self->chunk.fact_chunk.size;
        self->chunk.fact_chunk.size = 0;
    }

    self->error_code = WAV_OK;
}

void wav_write_header(WavFile* self)
{
    size_t write_count;

    fseek(self->fp, 0, SEEK_SET);

    self->chunk.size = (4) +
                       (WAV_RIFF_HEADER_SIZE + self->chunk.format_chunk.size) +
                       (WAV_RIFF_HEADER_SIZE + self->chunk.data_chunk.size);

    if (self->chunk.fact_chunk.id == WAV_FACT_CHUNK_ID) {
        /* if there is a fact chunk */
        self->chunk.size += WAV_RIFF_HEADER_SIZE + self->chunk.fact_chunk.size;
    }

    if (self->chunk.size % 2 != 0) {
        self->chunk.size++;
    }

    write_count = fwrite(&self->chunk, WAV_RIFF_HEADER_SIZE + 4, 1, self->fp);
    if (write_count != 1) {
        self->error_code = WAV_ERR_OS;
        return;
    }

    write_count = fwrite(&self->chunk.format_chunk, WAV_RIFF_HEADER_SIZE + self->chunk.format_chunk.size, 1, self->fp);
    if (write_count != 1) {
        self->error_code = WAV_ERR_OS;
        return;
    }

    if (self->chunk.fact_chunk.id == WAV_FACT_CHUNK_ID) {
        /* if there is a fact chunk */
        write_count = fwrite(&self->chunk.fact_chunk, WAV_RIFF_HEADER_SIZE + self->chunk.fact_chunk.size, 1, self->fp);
        if (write_count != 1) {
            self->error_code = WAV_ERR_OS;
            return;
        }
    }

    write_count = fwrite(&self->chunk.data_chunk, WAV_RIFF_HEADER_SIZE, 1, self->fp);
    if (write_count != 1) {
        self->error_code = WAV_ERR_OS;
        return;
    }

    self->error_code = WAV_OK;
}

void wav_init(WavFile* self, WAV_CONST char* filename, WAV_CONST char* mode)
{
    memset(self, 0, sizeof(WavFile));

    if (strncmp(mode, "r", 1) == 0 || strncmp(mode, "rb", 2) == 0) {
        self->mode = "rb";
    } else if (strncmp(mode, "r+", 2) == 0 || strncmp(mode, "rb+", 3) == 0 || strncmp(mode, "r+b", 3) == 0) {
        self->mode = "rb+";
    } else if (strncmp(mode, "w", 1) == 0 || strncmp(mode, "wb", 2) == 0) {
        self->mode = "wb";
    } else if (strncmp(mode, "w+", 2) == 0 || strncmp(mode, "wb+", 3) == 0 || strncmp(mode, "w+b", 3) == 0) {
        self->mode = "wb+";
    } else if (strncmp(mode, "wx", 2) == 0 || strncmp(mode, "wbx", 3) == 0) {
        self->mode = "wbx";
    } else if (strncmp(mode, "w+x", 3) == 0 || strncmp(mode, "wb+x", 4) == 0 || strncmp(mode, "w+bx", 4) == 0) {
        self->mode = "wb+x";
    } else if (strncmp(mode, "a", 1) == 0 || strncmp(mode, "ab", 2) == 0) {
        self->mode = "ab";
    } else if (strncmp(mode, "a+", 2) == 0 || strncmp(mode, "ab+", 3) == 0 || strncmp(mode, "a+b", 3) == 0) {
        self->mode = "ab+";
    } else {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    self->filename = wav_strdup(filename);

    self->fp = fopen(filename, self->mode);
    if (self->fp == NULL) {
        self->error_code = WAV_ERR_OS;
        return;
    }

    if (self->mode[0] == 'r') {
        wav_parse_header(self);
        return;
    }

    if (self->mode[0] == 'a') {
        wav_parse_header(self);
        if (self->error_code == WAV_OK) {
            // If it succeeded in parsing header, return immediately.
            return;
        } else {
            // Header parsing failed. Regard it as a new file.
            rewind(self->fp);
        }
    }

    // reaches here only if (mode[0] == 'w' || mode[0] == 'a')

    self->chunk.id = WAV_RIFF_CHUNK_ID;
    /* self->chunk.size = calculated by wav_write_header */
    self->chunk.wave_id = WAV_WAVE_ID;

    self->chunk.format_chunk.id                = WAV_FORMAT_CHUNK_ID;
    self->chunk.format_chunk.size              = (uint32_t)(&self->chunk.format_chunk.ext_size - &self->chunk.format_chunk.format_tag);
    self->chunk.format_chunk.format_tag        = WAV_FORMAT_PCM;
    self->chunk.format_chunk.n_channels        = 2;
    self->chunk.format_chunk.sample_rate       = 44100;
    self->chunk.format_chunk.avg_bytes_per_sec = 44100 * 2 * 2;
    self->chunk.format_chunk.block_align       = 4;
    self->chunk.format_chunk.bits_per_sample   = 16;

    memcpy(self->chunk.format_chunk.sub_format, default_sub_format, 16);

    self->chunk.fact_chunk.id   = WAV_DATA_CHUNK_ID; /* flag to indicate there is no fact chunk */
    self->chunk.fact_chunk.size = 0;

    self->chunk.data_chunk.id   = WAV_DATA_CHUNK_ID;
    self->chunk.data_chunk.size = 0;

    wav_write_header(self);

    if (self->error_code != WAV_OK) {
        return;
    }

    self->error_code = WAV_OK;
}

void wav_finalize(WavFile* self)
{
    int ret;

    wav_free(self->filename);

    if (self->fp == NULL) {
        return;
    }

    if ((strncmp(self->mode, "wb", 2) == 0 ||
         strncmp(self->mode, "wb+", 3) == 0 ||
         strncmp(self->mode, "wbx", 3) == 0 ||
         strncmp(self->mode, "wb+x", 4) == 0 ||
         strncmp(self->mode, "ab", 2) == 0 ||
         strncmp(self->mode, "ab+", 3) == 0) &&
        self->chunk.data_chunk.size % 2 != 0 &&
        wav_eof(self)) {
        char padding = 0;
        ret          = (int)fwrite(&padding, sizeof(padding), 1, self->fp);
        if (ret != 1) {
            self->error_code = WAV_ERR_OS;
            return;
        }
    }

    ret = fclose(self->fp);
    if (ret != 0) {
        self->error_code = WAV_ERR_OS;
        return;
    }

    self->error_code = WAV_OK;
}

WavFile* wav_open(WAV_CONST char* filename, WAV_CONST char* mode)
{
    WavFile* self = WAV_NEW(WavFile);
    if (self == NULL) {
        return NULL;
    }

    wav_init(self, filename, mode);

    return self;
}

int wav_close(WavFile* self)
{
    wav_finalize(self);

    if (self->error_code != WAV_OK) {
        return EOF;
    }

    if (self->tmp) {
        wav_free(self->tmp);
    }

    wav_free(self);

    return 0;
}

WavFile* wav_reopen(WavFile* self, WAV_CONST char* filename, WAV_CONST char* mode)
{
    wav_finalize(self);
    wav_init(self, filename, mode);
    return self;
}

static WAV_CONST size_t wav_container_sizes[5] = {0, 1, 2, 4, 4};

static inline size_t wav_calc_container_size(size_t sample_size)
{
    return wav_container_sizes[sample_size];
}

size_t wav_read(WavFile* self, void** buffers, size_t count)
{
    size_t   read_count;
    uint16_t n_channels     = wav_get_num_channels(self);
    size_t   sample_size    = wav_get_sample_size(self);
    size_t   container_size = wav_calc_container_size(sample_size);
    size_t   i, j, k;
    size_t   len_remain;

    if (strncmp(self->mode, "wb", 2) == 0 || strncmp(self->mode, "wbx", 3) == 0 || strncmp(self->mode, "ab", 2) == 0) {
        self->error_code = WAV_ERR_MODE;
        return 0;
    }

    if (self->chunk.format_chunk.format_tag == WAV_FORMAT_EXTENSIBLE) {
        self->error_code = WAV_ERR_FORMAT;
        return 0;
    }

    len_remain = wav_get_length(self) - (size_t)wav_tell(self);
    if (self->error_code != WAV_OK) {
        return 0;
    }
    count = (count <= len_remain) ? count : len_remain;

    if (count == 0) {
        self->error_code = WAV_OK;
        return 0;
    }

    if (self->tmp_size < n_channels * count * sample_size || !self->tmp) {
        if (self->tmp) {
            wav_free(self->tmp);
        }
        self->tmp_size = n_channels * count * sample_size;
        self->tmp      = WAV_NEW(uint8_t, self->tmp_size);
        if (self->tmp == NULL) {
            self->tmp_size   = 0;
            self->error_code = WAV_ERR_NOMEM;
            return 0;
        }
    }

    read_count = fread(self->tmp, sample_size, n_channels * count, self->fp);
    if (ferror(self->fp)) {
        self->error_code = WAV_ERR_OS;
        return 0;
    }

    for (i = 0; i < n_channels; ++i) {
        for (j = 0; j < read_count / n_channels; ++j) {
#ifdef WAV_ENDIAN_LITTLE
            for (k = 0; k < sample_size; ++k) {
                ((uint8_t*)buffers[i])[j * container_size + k] = self->tmp[j * n_channels * sample_size + i * sample_size + k];
            }

            /* sign extension */
            if (((uint8_t*)buffers[i])[j * container_size + k - 1] & 0x80) {
                for (; k < container_size; ++k) {
                    ((uint8_t*)buffers[i])[j * container_size + k] = 0xff;
                }
            } else {
                for (; k < container_size; ++k) {
                    ((uint8_t*)buffers[i])[j * container_size + k] = 0x00;
                }
            }
#endif
        }
    }

    self->error_code = WAV_OK;

    return read_count / n_channels;
}

size_t wav_write(WavFile* self, WAV_CONST void* WAV_CONST* buffers, size_t count)
{
    size_t   write_count;
    uint16_t n_channels     = wav_get_num_channels(self);
    size_t   sample_size    = wav_get_sample_size(self);
    size_t   container_size = wav_calc_container_size(sample_size);
    size_t   i, j, k;
    long int save_pos;

    if (strncmp(self->mode, "rb", 2) == 0) {
        self->error_code = WAV_ERR_MODE;
        return 0;
    }

    if (self->chunk.format_chunk.format_tag == WAV_FORMAT_EXTENSIBLE) {
        self->error_code = WAV_ERR_FORMAT;
        return 0;
    }

    if (count == 0) {
        self->error_code = WAV_OK;
        return 0;
    }

    wav_tell(self);
    if (self->error_code != WAV_OK) {
        return 0;
    }

    if (self->tmp_size < n_channels * count * sample_size || !self->tmp) {
        if (self->tmp) {
            wav_free(self->tmp);
        }
        self->tmp_size = n_channels * count * sample_size;
        self->tmp      = WAV_NEW(uint8_t, self->tmp_size);
        if (self->tmp == NULL) {
            self->tmp_size   = 0;
            self->error_code = WAV_ERR_NOMEM;
            return 0;
        }
    }

    for (i = 0; i < n_channels; ++i) {
        for (j = 0; j < count; ++j) {
#ifdef WAV_ENDIAN_LITTLE
            for (k = 0; k < sample_size; ++k) {
                self->tmp[j * n_channels * sample_size + i * sample_size + k] = ((uint8_t*)buffers[i])[j * container_size + k];
            }
#endif
        }
    }

    write_count = fwrite(self->tmp, sample_size, n_channels * count, self->fp);
    if (ferror(self->fp)) {
        self->error_code = WAV_ERR_OS;
        return 0;
    }

    self->chunk.data_chunk.size += write_count * sample_size;

    if (self->chunk.fact_chunk.id == WAV_FACT_CHUNK_ID) {
        self->chunk.fact_chunk.sample_length += write_count / n_channels;
    }

    save_pos = ftell(self->fp);
    if (save_pos == -1L) {
        self->error_code = WAV_ERR_OS;
        return 0;
    }
    wav_write_header(self);
    if (self->error_code != WAV_OK) {
        return 0;
    }
    if (fseek(self->fp, save_pos, SEEK_SET) != 0) {
        self->error_code = WAV_ERR_OS;
        return 0;
    }

    self->error_code = WAV_OK;
    return write_count / n_channels;
}

long int wav_tell(WAV_CONST WavFile* self)
{
    long pos = ftell(self->fp);

    if (pos == -1L) {
        ((WavFile *)self)->error_code = WAV_ERR_OS;
        return -1L;
    } else {
        long header_size = (long)wav_get_header_size(self);

        assert(pos >= header_size);

        ((WavFile *)self)->error_code = WAV_OK;
        return (pos - header_size) / (self->chunk.format_chunk.block_align);
    }
}

int wav_seek(WavFile* self, long int offset, int origin)
{
    size_t length = wav_get_length(self);
    int    ret;

    if (origin == SEEK_CUR) {
        offset += wav_tell(self);
    } else if (origin == SEEK_END) {
        offset += length;
    }

    if (offset >= 0 && (size_t)offset <= length) {
        offset = self->chunk.format_chunk.block_align;
    } else {
        self->error_code = WAV_ERR_PARAM;
        return (int)self->error_code;
    }

    ret = fseek(self->fp, offset, SEEK_SET);

    if (ret != 0) {
        self->error_code = WAV_ERR_OS;
        return (int)self->error_code;
    }

    self->error_code = WAV_OK;
    return (int)self->error_code;
}

void wav_rewind(WavFile* self)
{
    wav_seek(self, 0, SEEK_SET);
}

int wav_eof(WAV_CONST WavFile* self)
{
    return feof(self->fp) ||
           ftell(self->fp) == (long)(wav_get_header_size(self) + self->chunk.data_chunk.size);
}

int wav_error(WAV_CONST WavFile* self)
{
    return self->error_code != WAV_OK || (self->fp != NULL && ferror(self->fp));
}

int wav_flush(WavFile* self)
{
    int ret = fflush(self->fp);

    self->error_code = (ret == 0) ? WAV_OK : WAV_ERR_OS;

    return ret;
}

WavErr wav_errno(WAV_CONST WavFile* self)
{
    return self->error_code;
}

void wav_set_format(WavFile* self, uint16_t format)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    self->chunk.format_chunk.format_tag = format;
    if (format != WAV_FORMAT_PCM && format != WAV_FORMAT_EXTENSIBLE) {
        self->chunk.format_chunk.ext_size = 0;
        self->chunk.format_chunk.size     = (uint32_t)(&self->chunk.format_chunk.valid_bits_per_sample - &self->chunk.format_chunk.format_tag);
    } else if (format == WAV_FORMAT_EXTENSIBLE) {
        self->chunk.format_chunk.ext_size = 22;
        self->chunk.format_chunk.size     = sizeof(WavFormatChunk) - WAV_RIFF_HEADER_SIZE;
    }

    if (format == WAV_FORMAT_ALAW || format == WAV_FORMAT_MULAW) {
        self->chunk.format_chunk.bits_per_sample = 8;
    } else if (format == WAV_FORMAT_IEEE_FLOAT) {
        if (self->chunk.format_chunk.block_align != 4 && self->chunk.format_chunk.block_align != 8) {
            self->chunk.format_chunk.block_align = 4;
        }
        if (self->chunk.format_chunk.bits_per_sample > 8 * self->chunk.format_chunk.block_align) {
            self->chunk.format_chunk.bits_per_sample = 8 * self->chunk.format_chunk.block_align;
        }
    }

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

void wav_set_num_channels(WavFile* self, uint16_t n_channels)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    if (n_channels < 1) {
        self->error_code = WAV_ERR_PARAM;
        return;
    }

    self->chunk.format_chunk.n_channels = n_channels;

    self->chunk.format_chunk.avg_bytes_per_sec =
        self->chunk.format_chunk.block_align *
        self->chunk.format_chunk.sample_rate;

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

void wav_set_sample_rate(WavFile* self, uint32_t sample_rate)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    self->chunk.format_chunk.sample_rate = sample_rate;

    self->chunk.format_chunk.avg_bytes_per_sec =
        self->chunk.format_chunk.block_align *
        self->chunk.format_chunk.sample_rate;

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

void wav_set_valid_bits_per_sample(WavFile* self, uint16_t bits)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    if (bits < 1 || bits > 8 * self->chunk.format_chunk.block_align / self->chunk.format_chunk.n_channels) {
        self->error_code = WAV_ERR_PARAM;
        return;
    }

    if ((self->chunk.format_chunk.format_tag == WAV_FORMAT_ALAW ||
         self->chunk.format_chunk.format_tag == WAV_FORMAT_MULAW) &&
        bits != 8) {
        self->error_code = WAV_ERR_PARAM;
        return;
    }

    if (self->chunk.format_chunk.format_tag != WAV_FORMAT_EXTENSIBLE) {
        self->chunk.format_chunk.bits_per_sample = bits;
    } else {
        self->chunk.format_chunk.bits_per_sample       = 8 * self->chunk.format_chunk.block_align / self->chunk.format_chunk.n_channels;
        self->chunk.format_chunk.valid_bits_per_sample = bits;
    }

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

void wav_set_sample_size(WavFile* self, size_t sample_size)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    if (sample_size < 1) {
        self->error_code = WAV_ERR_PARAM;
        return;
    }

    self->chunk.format_chunk.block_align       = (uint16_t)(sample_size * self->chunk.format_chunk.n_channels);
    self->chunk.format_chunk.avg_bytes_per_sec = self->chunk.format_chunk.block_align * self->chunk.format_chunk.sample_rate;
    self->chunk.format_chunk.bits_per_sample   = (uint16_t)(sample_size * 8);
    if (self->chunk.format_chunk.format_tag == WAV_FORMAT_EXTENSIBLE) {
        self->chunk.format_chunk.valid_bits_per_sample = (uint16_t)(sample_size * 8);
    }

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

void wav_set_channel_mask(WavFile* self, uint32_t channel_mask)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    if (self->chunk.format_chunk.format_tag != WAV_FORMAT_EXTENSIBLE) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    self->chunk.format_chunk.channel_mask = channel_mask;

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

void wav_set_sub_format(WavFile* self, uint16_t sub_format)
{
    if (self->mode[0] == 'r') {
        self->error_code = WAV_ERR_MODE;
        return;
    }

    if (self->chunk.format_chunk.format_tag != WAV_FORMAT_EXTENSIBLE) {
        self->error_code = WAV_ERR_FORMAT;
        return;
    }

    *((uint16_t*)&self->chunk.format_chunk.sub_format) = sub_format;

    wav_write_header(self);
    /* self->error_code is set by wav_write_header */
}

uint16_t wav_get_format(WAV_CONST WavFile* self)
{
    return self->chunk.format_chunk.format_tag;
}

uint16_t wav_get_num_channels(WAV_CONST WavFile* self)
{
    return self->chunk.format_chunk.n_channels;
}

uint32_t wav_get_sample_rate(WAV_CONST WavFile* self)
{
    return self->chunk.format_chunk.sample_rate;
}

uint16_t wav_get_valid_bits_per_sample(WAV_CONST WavFile* self)
{
    if (self->chunk.format_chunk.format_tag != WAV_FORMAT_EXTENSIBLE) {
        return self->chunk.format_chunk.bits_per_sample;
    } else {
        return self->chunk.format_chunk.valid_bits_per_sample;
    }
}

size_t wav_get_sample_size(WAV_CONST WavFile* self)
{
    return self->chunk.format_chunk.block_align / self->chunk.format_chunk.n_channels;
}

size_t wav_get_length(WAV_CONST WavFile* self)
{
    return self->chunk.data_chunk.size / (self->chunk.format_chunk.block_align);
}

uint32_t wav_get_channel_mask(WAV_CONST WavFile* self)
{
    return self->chunk.format_chunk.channel_mask;
}

uint16_t wav_get_sub_format(WAV_CONST WavFile* self)
{
    return *((uint16_t*)&self->chunk.format_chunk.sub_format);
}
