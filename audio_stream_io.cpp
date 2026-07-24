/**
 * @file audio_stream_io.cpp
 * @brief Stream I/O implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_check.h"

#include "audio_stream_io.h"

static const char *TAG = "audio_stream_io";

typedef struct audio_stream_io {
    audio_stream_io_ops_t ops;
    void *ctx;
} audio_stream_io_t;

/* ================= File stream implementation ================= */

typedef struct {
    FILE *fp;
    bool should_close;
} file_stream_ctx_t;

static size_t file_stream_read(void *ctx, void *buf, size_t size) {
    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(ctx);
    if (!fctx || !fctx->fp) return 0;
    return fread(buf, 1, size, fctx->fp);
}

static int file_stream_seek(void *ctx, long offset, int whence) {
    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(ctx);
    if (!fctx || !fctx->fp) return -1;

    int file_whence;
    switch (whence) {
        case AUDIO_STREAM_SEEK_SET:
            file_whence = SEEK_SET;
            break;
        case AUDIO_STREAM_SEEK_CUR:
            file_whence = SEEK_CUR;
            break;
        case AUDIO_STREAM_SEEK_END:
            file_whence = SEEK_END;
            break;
        default:
            return -1;
    }

    return fseek(fctx->fp, offset, file_whence);
}

static long file_stream_tell(void *ctx) {
    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(ctx);
    if (!fctx || !fctx->fp) return -1;
    return ftell(fctx->fp);
}

static int file_stream_eof(void *ctx) {
    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(ctx);
    if (!fctx || !fctx->fp) return 1;
    return feof(fctx->fp);
}

static void file_stream_close(void *ctx) {
    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(ctx);
    if (!fctx) return;
    if (fctx->fp && fctx->should_close) {
        fclose(fctx->fp);
    }
    free(fctx);
}

static const audio_stream_io_ops_t file_stream_ops = {
    .read = file_stream_read,
    .seek = file_stream_seek,
    .tell = file_stream_tell,
    .eof = file_stream_eof,
    .close = file_stream_close
};

/* ================= Memory stream implementation ================= */

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    bool owns_buffer;
} mem_stream_ctx_t;

static size_t mem_stream_read(void *ctx, void *buf, size_t size) {
    mem_stream_ctx_t *mctx = static_cast<mem_stream_ctx_t*>(ctx);
    if (!mctx || !mctx->buf) return 0;

    size_t available = mctx->size - mctx->pos;
    size_t to_read = (size < available) ? size : available;

    if (to_read > 0) {
        memcpy(buf, mctx->buf + mctx->pos, to_read);
        mctx->pos += to_read;
    }

    return to_read;
}

static int mem_stream_seek(void *ctx, long offset, int whence) {
    mem_stream_ctx_t *mctx = static_cast<mem_stream_ctx_t*>(ctx);
    if (!mctx) return -1;

    size_t new_pos;

    switch (whence) {
        case AUDIO_STREAM_SEEK_SET:
            new_pos = offset;
            break;
        case AUDIO_STREAM_SEEK_CUR:
            new_pos = mctx->pos + offset;
            break;
        case AUDIO_STREAM_SEEK_END:
            new_pos = mctx->size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos > mctx->size) {
        return -1;
    }

    mctx->pos = new_pos;
    return 0;
}

static long mem_stream_tell(void *ctx) {
    const mem_stream_ctx_t *mctx = static_cast<mem_stream_ctx_t*>(ctx);
    if (!mctx) return -1;
    return static_cast<long>(mctx->pos);
}

static int mem_stream_eof(void *ctx) {
    const mem_stream_ctx_t *mctx = static_cast<mem_stream_ctx_t*>(ctx);
    if (!mctx) return 1;
    return (mctx->pos >= mctx->size) ? 1 : 0;
}

static void mem_stream_close(void *ctx) {
    mem_stream_ctx_t *mctx = static_cast<mem_stream_ctx_t*>(ctx);
    if (!mctx) return;
    if (mctx->owns_buffer && mctx->buf) {
        free(const_cast<uint8_t*>(mctx->buf));
    }
    free(mctx);
}

static const audio_stream_io_ops_t mem_stream_ops = {
    .read = mem_stream_read,
    .seek = mem_stream_seek,
    .tell = mem_stream_tell,
    .eof = mem_stream_eof,
    .close = mem_stream_close
};

/* ================= Public API ================= */

audio_stream_io_handle_t audio_stream_io_create(const audio_stream_io_ops_t *ops, void *ctx) {
    ESP_RETURN_ON_FALSE(ops != NULL, NULL, TAG, "ops is NULL");
    ESP_RETURN_ON_FALSE(ops->read != NULL, NULL, TAG, "read operation is required");

    audio_stream_io_t *h = static_cast<audio_stream_io_t*>(calloc(1, sizeof(audio_stream_io_t)));
    ESP_RETURN_ON_FALSE(h != NULL, NULL, TAG, "allocation failed");

    h->ops = *ops;
    h->ctx = ctx;

    return h;
}

audio_stream_io_handle_t audio_stream_io_from_file(FILE *fp) {
    ESP_RETURN_ON_FALSE(fp != NULL, NULL, TAG, "fp is NULL");

    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(calloc(1, sizeof(file_stream_ctx_t)));
    ESP_RETURN_ON_FALSE(fctx != NULL, NULL, TAG, "allocation failed");

    fctx->fp = fp;
    fctx->should_close = true;

    audio_stream_io_handle_t h = audio_stream_io_create(&file_stream_ops, fctx);
    if (!h) {
        free(fctx);
    }
    return h;
}

audio_stream_io_handle_t audio_stream_io_from_file_no_close(FILE *fp) {
    ESP_RETURN_ON_FALSE(fp != NULL, NULL, TAG, "fp is NULL");

    file_stream_ctx_t *fctx = static_cast<file_stream_ctx_t*>(calloc(1, sizeof(file_stream_ctx_t)));
    ESP_RETURN_ON_FALSE(fctx != NULL, NULL, TAG, "allocation failed");

    fctx->fp = fp;
    fctx->should_close = false;

    audio_stream_io_handle_t h = audio_stream_io_create(&file_stream_ops, fctx);
    if (!h) {
        free(fctx);
    }
    return h;
}

audio_stream_io_handle_t audio_stream_io_from_memory(const void *buf, size_t size, bool copy) {
    ESP_RETURN_ON_FALSE(buf != NULL, NULL, TAG, "buf is NULL");
    ESP_RETURN_ON_FALSE(size > 0, NULL, TAG, "size is 0");

    mem_stream_ctx_t *mctx = static_cast<mem_stream_ctx_t*>(calloc(1, sizeof(mem_stream_ctx_t)));
    ESP_RETURN_ON_FALSE(mctx != NULL, NULL, TAG, "allocation failed");

    if (copy) {
        mctx->buf = static_cast<uint8_t*>(malloc(size));
        if (!mctx->buf) {
            free(mctx);
            ESP_LOGE(TAG, "buffer allocation failed");
            return NULL;
        }
        memcpy(const_cast<uint8_t*>(mctx->buf), buf, size);
        mctx->owns_buffer = true;
    } else {
        mctx->buf = static_cast<const uint8_t*>(buf);
        mctx->owns_buffer = false;
    }

    mctx->size = size;
    mctx->pos = 0;

    audio_stream_io_handle_t h = audio_stream_io_create(&mem_stream_ops, mctx);
    if (!h) {
        mem_stream_close(mctx);
    }
    return h;
}

size_t audio_stream_io_read(audio_stream_io_handle_t h, void *buf, size_t size) {
    if (!h || !buf) return 0;
    return h->ops.read(h->ctx, buf, size);
}

esp_err_t audio_stream_io_seek(audio_stream_io_handle_t h, long offset, int whence) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    if (!h->ops.seek) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (h->ops.seek(h->ctx, offset, whence) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t audio_stream_io_tell(audio_stream_io_handle_t h, long *pos) {
    ESP_RETURN_ON_FALSE(h != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(pos != NULL, ESP_ERR_INVALID_ARG, TAG, "pos is NULL");

    if (!h->ops.tell) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    long p = h->ops.tell(h->ctx);
    if (p < 0) {
        return ESP_FAIL;
    }

    *pos = p;
    return ESP_OK;
}

bool audio_stream_io_eof(audio_stream_io_handle_t h) {
    if (!h) return true;
    if (h->ops.eof) {
        return h->ops.eof(h->ctx) != 0;
    }
    return false;
}

void audio_stream_io_close(audio_stream_io_handle_t h) {
    if (!h) return;

    if (h->ops.close) {
        h->ops.close(h->ctx);
    }

    free(h);
}

void* audio_stream_io_get_context(audio_stream_io_handle_t h) {
    if (!h) return NULL;
    return h->ctx;
}
