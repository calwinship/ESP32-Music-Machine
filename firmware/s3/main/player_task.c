// Player task: INDEX.BIN -> first tune -> seek to its first marker -> MP3 frames -> esp_audio_codec -> I2S.
// The decisions (index checks, seek maths, which decoded samples to keep, play/pause/end, when to log) are all in
// components/logic (host-tested); this file only moves bytes between the file, the decoder and the driver.
#include "player_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/bsp.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "player";

#define PLAYER_STACK      (8 * 1024)
#define PLAYER_PRIO       5            // above LVGL (2): audio must never wait for the display
#define PLAYER_CORE       1
#define READ_CHUNK        4096
#define MAX_FRAME_SAMPLES 1152
#define PAUSE_SAMPLES     480          // silence written per loop while paused (15 ms at 32 kHz)
#define DRAIN_MS          200          // silence after the last sample so it plays out before the clocks stop
#define TAP_BIT           1u
#define INDEX_PATH        BSP_TUNES_PATH "/INDEX.BIN"

static TaskHandle_t s_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static player_status_t s_status = {.title = "loading...", .state = PLAYER_IDLE};

static int16_t s_pcm[MAX_FRAME_SAMPLES * 2];   // decoder output (up to stereo), downmixed in place to mono

typedef struct {
    FILE *f;
    uint8_t buf[READ_CHUNK];
    size_t len, pos;
    bool eof;
} reader_t;
static reader_t s_reader;

// ---- status shared with the UI ------------------------------------------------------------------------------
void player_status_get(player_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_set(const player_t *p, const char *title)
{
    taskENTER_CRITICAL(&s_lock);
    if (title) {
        text_fold_ascii(s_status.title, sizeof(s_status.title), title);
    }
    if (p) {
        s_status.elapsed_ms = player_elapsed_ms(p);
        s_status.state = p->state;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void status_error(const char *msg)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_status.title, msg, sizeof(s_status.title));
    s_status.state = PLAYER_ERROR;
    taskEXIT_CRITICAL(&s_lock);
}

void player_task_tap(void)
{
    if (s_task) {
        xTaskNotify(s_task, TAP_BIT, eSetBits);
    }
}

// ---- file reading -------------------------------------------------------------------------------------------
static bool reader_fill(reader_t *r, size_t need)
{
    if (r->len - r->pos >= need) {
        return true;
    }
    memmove(r->buf, r->buf + r->pos, r->len - r->pos);
    r->len -= r->pos;
    r->pos = 0;
    while (!r->eof && r->len < need) {
        size_t n = fread(r->buf + r->len, 1, sizeof(r->buf) - r->len, r->f);
        if (n == 0) {
            r->eof = true;
        }
        r->len += n;
    }
    return r->len >= need;
}

// Next whole frame (valid until the next call), or NULL: *bad is false at a clean end of file.
static const uint8_t *reader_next_frame(reader_t *r, uint32_t *flen, bool *bad)
{
    *bad = false;
    if (!reader_fill(r, 4)) {
        *bad = r->len != r->pos;   // leftover bytes that are not a frame
        return NULL;
    }
    uint32_t n = mp3_frame_len(r->buf + r->pos);
    if (!n || !reader_fill(r, n)) {
        *bad = true;
        return NULL;
    }
    const uint8_t *p = r->buf + r->pos;
    r->pos += n;
    *flen = n;
    return p;
}

static uint8_t *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    uint8_t *buf = NULL;
    long n = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (n > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = heap_caps_malloc((size_t)n, MALLOC_CAP_8BIT);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

// "m:ss.cc" for logs
static const char *log_time(char *buf, size_t n, uint32_t ms)
{
    char mmss[16];
    text_mmss(mmss, sizeof(mmss), ms);
    snprintf(buf, n, "%s.%02u", mmss, (unsigned)(ms % 1000u / 10u));
    return buf;
}

// ---- one tune -----------------------------------------------------------------------------------------------
static void play(const tune_info_t *t)
{
    char a[24], b[24], path[48];
    snprintf(path, sizeof(path), "%s/%s", BSP_TUNES_PATH, t->file_name);
    uint32_t start_ms = t->marker_count ? t->markers_ms[0] : 0;
    seek_plan_t plan;
    if (!seek_plan(t, seek_ms_to_sample(start_ms, t->sample_rate), &plan)) {
        ESP_LOGE(TAG, "first marker %u ms is outside the tune", (unsigned)start_ms);
        status_error("bad marker");
        return;
    }
    reader_t *r = &s_reader;
    memset(r, 0, sizeof(*r));
    r->f = fopen(path, "rb");
    if (!r->f || fseek(r->f, (long)plan.byte_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "cannot open %s at byte %u", path, (unsigned)plan.byte_offset);
        status_error("cannot open tune");
        if (r->f) {
            fclose(r->f);
        }
        return;
    }
    void *dec = NULL;
    esp_audio_err_t derr = esp_mp3_dec_open(NULL, 0, &dec);
    if (derr != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_mp3_dec_open: %d", (int)derr);
        status_error("decoder failed");
        fclose(r->f);
        return;
    }
    if (bsp_audio_init(t->sample_rate) != ESP_OK) {
        status_error("audio init failed");
        esp_mp3_dec_close(dec);
        fclose(r->f);
        return;
    }

    player_t pl;
    player_init(&pl);
    player_start(&pl, t->sample_rate, t->total_samples, plan.target_sample);
    frame_gate_t gate;
    frame_gate_init(&gate, &plan);
    status_set(&pl, t->title);
    ESP_LOGI(TAG, "playing %s \"%s\" (id %08x) from %s: read from byte %u (frame %u), target frame %u + %u samples",
             t->file_name, t->title, (unsigned)t->tune_id, log_time(a, sizeof(a), start_ms),
             (unsigned)plan.byte_offset, (unsigned)plan.start_frame, (unsigned)plan.target_frame,
             (unsigned)plan.skip_in_target);

    unsigned preroll_errors = 0, decode_errors = 0, frames = 0;
    bool reported_first = false;
    char total[16];
    text_mmss(total, sizeof(total), (uint32_t)((uint64_t)t->total_samples * 1000u / t->sample_rate));
    bsp_audio_start();

    while (pl.state == PLAYER_PLAYING || pl.state == PLAYER_PAUSED) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, 0) == pdTRUE && (bits & TAP_BIT) && player_toggle(&pl)) {
            ESP_LOGI(TAG, "%s at %s (tap)", pl.state == PLAYER_PAUSED ? "paused" : "resumed",
                     log_time(a, sizeof(a), player_elapsed_ms(&pl)));
        }

        if (pl.state == PLAYER_PAUSED) {
            bsp_audio_write_silence(PAUSE_SAMPLES);
        } else {
            uint32_t flen;
            bool bad;
            const uint8_t *frame = reader_next_frame(r, &flen, &bad);
            if (!frame) {
                ESP_LOGE(TAG, "%s at %s, %u samples short of the end", bad ? "bad MP3 data" : "file ended",
                         log_time(a, sizeof(a), player_elapsed_ms(&pl)), (unsigned)(pl.total_samples - pl.position));
                player_fail(&pl);
                break;
            }
            bool preroll = frame_gate_in_preroll(&gate);
            esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)frame, .len = flen};
            esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)s_pcm, .len = sizeof(s_pcm)};
            esp_audio_dec_info_t info = {0};
            derr = esp_mp3_dec_decode(dec, &raw, &out, &info);
            frames++;
            uint32_t samples = 0;
            if (derr == ESP_AUDIO_ERR_OK) {
                uint32_t ch = info.channel ? info.channel : 1;
                samples = out.decoded_size / (2u * ch);
                if (ch == 2) {
                    for (uint32_t i = 0; i < samples; i++) {
                        s_pcm[i] = (int16_t)(((int32_t)s_pcm[2 * i] + s_pcm[2 * i + 1]) / 2);
                    }
                }
                if (!reported_first) {
                    reported_first = true;
                    ESP_LOGI(TAG, "decoder: first good frame #%u: consumed %u of %u bytes, %u samples, %u Hz, %u ch, "
                             "%u kbps (%u pre-roll frames failed before it)", frames, (unsigned)raw.consumed,
                             (unsigned)flen, (unsigned)samples, (unsigned)info.sample_rate, (unsigned)info.channel,
                             (unsigned)(info.bitrate / 1000u), preroll_errors);
                    if (info.sample_rate != t->sample_rate) {
                        ESP_LOGE(TAG, "decoder rate %u Hz != index %u Hz", (unsigned)info.sample_rate,
                                 (unsigned)t->sample_rate);
                        player_fail(&pl);
                        break;
                    }
                }
            } else if (preroll) {
                preroll_errors++;   // expected: the bit reservoir before the seek point is missing
            } else if (decode_errors++ < 10) {
                ESP_LOGE(TAG, "decode error %d at %s (frame %u bytes)", (int)derr,
                         log_time(a, sizeof(a), player_elapsed_ms(&pl)), (unsigned)flen);
            }
            uint32_t first;
            uint32_t keep = frame_gate_next(&gate, samples, &first);
            keep = player_accept(&pl, keep);
            if (keep && bsp_audio_write_mono(s_pcm + first, keep) != ESP_OK) {
                player_fail(&pl);
                break;
            }
        }

        for (uint32_t n = player_new_underruns(&pl, bsp_audio_underruns()); n; n--) {
            ESP_LOGW(TAG, "UNDERRUN #%u at %s", (unsigned)(pl.underruns_seen - n + 1),
                     log_time(a, sizeof(a), player_elapsed_ms(&pl)));
        }
        if (pl.state == PLAYER_PLAYING && player_log_due(&pl)) {
            char now[16];
            text_mmss(now, sizeof(now), player_elapsed_ms(&pl));
            ESP_LOGI(TAG, "pos %s / %s underruns %u", now, total, (unsigned)pl.underruns_seen);
        }
        status_set(&pl, NULL);
    }

    if (pl.state == PLAYER_ENDED) {
        bsp_audio_write_silence(t->sample_rate * DRAIN_MS / 1000u);
        player_new_underruns(&pl, bsp_audio_underruns());
    }
    bsp_audio_stop();
    status_set(&pl, NULL);
    ESP_LOGI(TAG, "%s at %s / %s: %u frames, underruns %u, decode errors %u (pre-roll %u), self-test underruns %u",
             player_state_name(pl.state), log_time(a, sizeof(a), player_elapsed_ms(&pl)),
             log_time(b, sizeof(b), (uint32_t)((uint64_t)t->total_samples * 1000u / t->sample_rate)), frames,
             (unsigned)pl.underruns_seen, decode_errors, preroll_errors, (unsigned)bsp_audio_selftest_underruns());
    esp_mp3_dec_close(dec);
    fclose(r->f);
}

static void player_task(void *arg)
{
    (void)arg;
    size_t len = 0;
    uint8_t *data = load_file(INDEX_PATH, &len);
    tune_index_t ix;
    tune_index_err_t err = data ? tune_index_open(&ix, data, len) : TUNE_INDEX_ERR_SIZE;
    if (!data || err != TUNE_INDEX_OK || ix.count == 0) {
        ESP_LOGE(TAG, "%s: %s", INDEX_PATH, data ? (err ? tune_index_err_str(err) : "no tunes") : "cannot read");
        status_error("No tunes: run ./sync-tunes");
    } else {
        ESP_LOGI(TAG, "%s: %u tunes", INDEX_PATH, ix.count);
        static tune_info_t t;
        for (uint16_t i = 0; i < ix.count; i++) {
            tune_index_get(&ix, i, &t);
            char d[16], m[16];
            text_mmss(d, sizeof(d), (uint32_t)((uint64_t)t.total_samples * 1000u / t.sample_rate));
            snprintf(m, sizeof(m), "%u.%u", (unsigned)(t.marker_count ? t.markers_ms[0] / 1000u : 0),
                     (unsigned)(t.marker_count ? t.markers_ms[0] % 1000u / 100u : 0));
            ESP_LOGI(TAG, "  %s %08x %5s starts %u (first %ss) %s", t.file_name, (unsigned)t.tune_id, d,
                     t.marker_count, m, t.title);
        }
        tune_index_get(&ix, 0, &t);
        play(&t);
    }
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void player_task_start(void)
{
    xTaskCreatePinnedToCore(player_task, "player", PLAYER_STACK, NULL, PLAYER_PRIO, &s_task, PLAYER_CORE);
}
