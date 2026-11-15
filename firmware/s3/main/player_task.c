// Player task: INDEX.BIN -> tune -> exact seek -> MP3 frames -> esp_audio_codec -> I2S, driven by a command queue
// (tap, knob seek, tune change). The decisions (index checks, seek maths, which decoded samples to keep, player
// state, fades) are in components/logic (host-tested); this file only moves bytes between file, decoder and driver.
// The loop never stops: while paused or after the end it writes silence, which keeps the DAC clocked and means a
// stall in this task is always visible as an underrun.
#include "player_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/bsp.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logic/controls.h"

static const char *TAG = "player";

#define PLAYER_STACK      (8 * 1024)
#define PLAYER_PRIO       5            // above LVGL (2): audio must never wait for the display
#define PLAYER_CORE       1
#define READ_CHUNK        4096
#define MAX_FRAME_SAMPLES 1152
#define SILENCE_SAMPLES   480          // silence written per loop while paused/ended (15 ms at 32 kHz)
#define CMD_QUEUE_LEN     16
#define INDEX_PATH        BSP_TUNES_PATH "/INDEX.BIN"

typedef enum { CMD_TAP, CMD_RESTART_SPEED, CMD_SPEED, CMD_SEEK, CMD_SELECT } cmd_type_t;
typedef struct {
    cmd_type_t type;
    int32_t arg;
} cmd_t;

typedef struct {
    FILE *f;
    uint8_t buf[READ_CHUNK];
    size_t len, pos;
    bool eof;
} reader_t;

static TaskHandle_t s_task;
static QueueHandle_t s_cmds;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static player_status_t s_status = {.title = "loading...", .state = PLAYER_IDLE};

// Player state: touched only by the player task.
static int16_t s_pcm[MAX_FRAME_SAMPLES * 2];   // decoder output (up to stereo), downmixed in place to mono
static int16_t s_ramp[FADE_SAMPLES];
static reader_t s_reader;
static tune_index_t s_ix;
static tune_info_t s_tune;
static uint16_t s_tune_i;
static void *s_dec;
static player_t s_pl;
static frame_gate_t s_gate;
static int16_t s_last_out;      // last sample written (before gain); the fade-out ramp starts here
static uint32_t s_fade_done = FADE_SAMPLES;
static unsigned s_decode_errors, s_preroll_errors, s_frames;
static uint32_t s_rate;         // I2S rate (all tunes must share it)
static bool s_resume;
static uint32_t s_resume_id, s_resume_ms;

// ---- commands and status ------------------------------------------------------------------------------------
static void send(cmd_type_t type, int32_t arg)
{
    const cmd_t c = {type, arg};
    if (s_cmds && xQueueSend(s_cmds, &c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full: dropped");
    }
}

void player_task_tap(void) { send(CMD_TAP, 0); }
void player_task_restart_speed(void) { send(CMD_RESTART_SPEED, 0); }
void player_task_change_speed(int32_t delta) { send(CMD_SPEED, delta); }
void player_task_seek(int32_t clicks) { send(CMD_SEEK, clicks); }
void player_task_select_tune(uint16_t tune) { send(CMD_SELECT, tune); }

static volatile bool s_index_ready;

bool player_tune_info(uint16_t tune, char *title, size_t n, uint32_t *colour)
{
    tune_info_t t;   // tune_index_get only reads the (now constant) index buffer: safe from other tasks
    if (!s_index_ready || !tune_index_get(&s_ix, tune, &t)) {
        return false;
    }
    strlcpy(title, t.title, n);
    *colour = t.colour;
    return true;
}

void player_status_get(player_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

static void status_update(bool title)
{
    speed_t sp;
    speed_at(&s_tune, s_pl.position, &sp);
    uint16_t permille = speed_permille(&sp, s_pl.position);
    uint32_t total_ms = s_tune.sample_rate ? (uint32_t)((uint64_t)s_tune.total_samples * 1000u / s_tune.sample_rate)
                                           : 0;
    taskENTER_CRITICAL(&s_lock);
    if (title) {
        strlcpy(s_status.title, s_tune.title, sizeof(s_status.title));
    }
    s_status.tune_id = s_tune.tune_id;
    s_status.icon = s_tune.icon;
    s_status.colour = s_tune.colour;
    s_status.elapsed_ms = player_elapsed_ms(&s_pl);
    s_status.total_ms = total_ms;
    s_status.state = s_pl.state;
    s_status.tune = s_tune_i;
    s_status.tune_count = s_ix.count;
    s_status.speed = sp.index;
    s_status.speed_count = sp.count;
    s_status.speed_permille = permille;
    taskEXIT_CRITICAL(&s_lock);
}

static const char *speed_label(uint8_t index, uint8_t count, char *buf, size_t n)
{
    const char *name = speed_name(index, count);
    if (name && *name) {
        return name;
    }
    snprintf(buf, n, "%u/%u", (unsigned)index + 1u, (unsigned)count);
    return buf;
}

static void status_error(const char *msg)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_status.title, msg, sizeof(s_status.title));
    s_status.state = PLAYER_ERROR;
    taskEXIT_CRITICAL(&s_lock);
}

// "m:ss.cc" for logs
static const char *log_time(char *buf, size_t n, uint32_t ms)
{
    char mmss[16];
    text_mmss(mmss, sizeof(mmss), ms);
    snprintf(buf, n, "%s.%02u", mmss, (unsigned)(ms % 1000u / 10u));
    return buf;
}

static uint32_t tune_ms(uint32_t samples)
{
    return (uint32_t)((uint64_t)samples * 1000u / s_tune.sample_rate);
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
        *bad = r->len != r->pos;
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

static bool reader_seek(reader_t *r, uint32_t offset)
{
    r->len = r->pos = 0;
    r->eof = false;
    return r->f && fseek(r->f, (long)offset, SEEK_SET) == 0;
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

// ---- output -------------------------------------------------------------------------------------------------
static void out_write(int16_t *pcm, uint32_t n)
{
    if (!n) {
        return;
    }
    fade_in(pcm, n, &s_fade_done);
    if (bsp_audio_write_mono(pcm, n) != ESP_OK) {
        player_fail(&s_pl);
    }
    s_last_out = pcm[n - 1];
}

// Before audio stops or jumps: ramp from the last sample to silence, and fade in whatever comes next.
static void out_break(void)
{
    if (s_last_out) {
        fade_ramp_to_zero(s_last_out, s_ramp, FADE_SAMPLES);
        bsp_audio_write_mono(s_ramp, FADE_SAMPLES);
        s_last_out = 0;
    }
    s_fade_done = 0;
}

// ---- seeking and tunes --------------------------------------------------------------------------------------
// Jump to tune sample `target`: reposition the file, reset the decoder, re-arm the gate. Keeps the play state.
static bool jump_to(uint32_t target)
{
    seek_plan_t plan;
    if (!seek_plan(&s_tune, target, &plan) || !reader_seek(&s_reader, plan.byte_offset)) {
        ESP_LOGE(TAG, "seek to sample %u failed", (unsigned)target);
        player_fail(&s_pl);
        return false;
    }
    esp_mp3_dec_reset(s_dec);
    frame_gate_init(&s_gate, &plan);
    player_seek(&s_pl, plan.target_sample);
    return true;
}

static uint32_t first_marker_sample(void)
{
    return seek_ms_to_sample(s_tune.marker_count ? s_tune.markers_ms[0] : 0, s_tune.sample_rate);
}

// Open tune i at tune sample `start` (UINT32_MAX = its first marker), playing or paused.
static bool open_tune_at(uint16_t i, uint32_t start, bool paused)
{
    char path[48], a[24];
    if (s_reader.f) {
        fclose(s_reader.f);
        s_reader.f = NULL;
    }
    if (!tune_index_get(&s_ix, i, &s_tune)) {
        return false;
    }
    s_tune_i = i;
    if (s_tune.sample_rate != s_rate) {
        ESP_LOGE(TAG, "%s is %u Hz but the output runs at %u Hz", s_tune.file_name, (unsigned)s_tune.sample_rate,
                 (unsigned)s_rate);
        player_fail(&s_pl);
        status_update(true);
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", BSP_TUNES_PATH, s_tune.file_name);
    s_reader.f = fopen(path, "rb");
    if (!s_reader.f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        player_fail(&s_pl);
        status_update(true);
        return false;
    }
    player_start(&s_pl, s_tune.sample_rate, s_tune.total_samples, 0);
    s_decode_errors = s_preroll_errors = s_frames = 0;
    if (!jump_to(start == UINT32_MAX ? first_marker_sample() : start)) {
        status_update(true);
        return false;
    }
    if (paused) {
        player_toggle(&s_pl);
    }
    char total[16];
    text_mmss(total, sizeof(total), tune_ms(s_tune.total_samples));
    ESP_LOGI(TAG, "tune %u/%u: %s \"%s\" (id %08x, %s, %u speeds) from %s, %s", (unsigned)i + 1,
             (unsigned)s_ix.count, s_tune.file_name, s_tune.title, (unsigned)s_tune.tune_id, total,
             (unsigned)(s_tune.marker_count ? s_tune.marker_count : 1), log_time(a, sizeof(a), tune_ms(s_pl.position)),
             player_state_name(s_pl.state));
    status_update(true);
    return true;
}

static void handle(const cmd_t *c)
{
    char a[24], b[24];
    switch (c->type) {
    case CMD_TAP:
        if (s_pl.state == PLAYER_ENDED) {
            jump_to(first_marker_sample());   // -> paused at the first marker
            player_toggle(&s_pl);             // -> playing
            s_fade_done = 0;
            ESP_LOGI(TAG, "restart from %s (tap)", log_time(a, sizeof(a), tune_ms(s_pl.position)));
        } else if (s_pl.state == PLAYER_PLAYING || s_pl.state == PLAYER_PAUSED) {
            if (s_pl.state == PLAYER_PLAYING) {
                out_break();
            } else {
                s_fade_done = 0;
            }
            player_toggle(&s_pl);
            ESP_LOGI(TAG, "%s at %s (tap)", s_pl.state == PLAYER_PAUSED ? "paused" : "resumed",
                     log_time(a, sizeof(a), tune_ms(s_pl.position)));
        }
        break;
    case CMD_SEEK: {
        if (s_pl.state != PLAYER_PLAYING && s_pl.state != PLAYER_PAUSED && s_pl.state != PLAYER_ENDED) {
            break;
        }
        uint32_t from = s_pl.position;
        uint32_t to = seek_target(from, c->arg, s_tune.sample_rate, s_tune.total_samples);
        if (s_pl.state == PLAYER_PLAYING) {
            out_break();
        }
        if (jump_to(to)) {
            ESP_LOGI(TAG, "seek %+d click(s): %s -> %s (sample %u, %s)", (int)c->arg, log_time(a, sizeof(a), tune_ms(from)),
                     log_time(b, sizeof(b), tune_ms(s_pl.position)), (unsigned)s_pl.position,
                     player_state_name(s_pl.state));
        }
        break;
    }
    case CMD_RESTART_SPEED:
    case CMD_SPEED: {
        if (s_pl.state != PLAYER_PLAYING && s_pl.state != PLAYER_PAUSED && s_pl.state != PLAYER_ENDED) {
            break;
        }
        speed_t sp;
        speed_at(&s_tune, s_pl.position, &sp);
        int32_t want = c->type == CMD_SPEED ? (int32_t)sp.index + c->arg : sp.index;
        want = want < 0 ? 0 : want >= sp.count ? sp.count - 1 : want;
        uint32_t from = s_pl.position;
        if (s_pl.state == PLAYER_PLAYING) {
            out_break();
        }
        if (!jump_to(speed_start(&s_tune, (uint8_t)want))) {
            break;
        }
        if (c->type == CMD_RESTART_SPEED && s_pl.state != PLAYER_PLAYING) {
            player_toggle(&s_pl);   // paused (or ended -> paused by the jump) -> playing
            s_fade_done = 0;
        }
        char n1[12], n2[12];
        ESP_LOGI(TAG, "%s: %s %s -> %s %s (%s)", c->type == CMD_SPEED ? "speed" : "restart speed",
                 speed_label(sp.index, sp.count, n1, sizeof(n1)), log_time(a, sizeof(a), tune_ms(from)),
                 speed_label((uint8_t)want, sp.count, n2, sizeof(n2)), log_time(b, sizeof(b), tune_ms(s_pl.position)),
                 player_state_name(s_pl.state));
        break;
    }
    case CMD_SELECT: {
        if (c->arg < 0 || c->arg >= s_ix.count) {
            break;
        }
        if (s_pl.state == PLAYER_PLAYING) {
            out_break();
        }
        ESP_LOGI(TAG, "tune chosen from the list: %d", (int)c->arg + 1);
        open_tune_at((uint16_t)c->arg, UINT32_MAX, false);
        s_fade_done = 0;
        break;
    }
    }
}

// One decoded frame (or silence when not playing).
static void step(void)
{
    char a[24];
    if (s_pl.state != PLAYER_PLAYING) {
        bsp_audio_write_silence(SILENCE_SAMPLES);
        return;
    }
    uint32_t flen;
    bool bad;
    const uint8_t *frame = reader_next_frame(&s_reader, &flen, &bad);
    if (!frame) {
        ESP_LOGE(TAG, "%s at %s, %u samples short of the end", bad ? "bad MP3 data" : "file ended",
                 log_time(a, sizeof(a), tune_ms(s_pl.position)), (unsigned)(s_pl.total_samples - s_pl.position));
        player_fail(&s_pl);
        return;
    }
    bool preroll = frame_gate_in_preroll(&s_gate);
    esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)frame, .len = flen};
    esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)s_pcm, .len = sizeof(s_pcm)};
    esp_audio_dec_info_t info = {0};
    esp_audio_err_t err = esp_mp3_dec_decode(s_dec, &raw, &out, &info);
    s_frames++;
    uint32_t samples = 0;
    if (err == ESP_AUDIO_ERR_OK) {
        uint32_t ch = info.channel ? info.channel : 1;
        samples = out.decoded_size / (2u * ch);
        if (ch == 2) {
            for (uint32_t i = 0; i < samples; i++) {
                s_pcm[i] = (int16_t)(((int32_t)s_pcm[2 * i] + s_pcm[2 * i + 1]) / 2);
            }
        }
    } else if (preroll) {
        s_preroll_errors++;   // expected: the bit reservoir before the seek point is missing
    } else if (s_decode_errors++ < 10) {
        ESP_LOGE(TAG, "decode error %d at %s", (int)err, log_time(a, sizeof(a), tune_ms(s_pl.position)));
    }
    uint32_t first;
    uint32_t keep = player_accept(&s_pl, frame_gate_next(&s_gate, samples, &first));
    out_write(s_pcm + first, keep);
    if (s_pl.state == PLAYER_ENDED) {
        out_break();
        char b[24];
        ESP_LOGI(TAG, "ended at %s: %u frames, underruns %u, decode errors %u (pre-roll %u). Tap to play again",
                 log_time(b, sizeof(b), tune_ms(s_pl.position)), s_frames, (unsigned)s_pl.underruns_seen,
                 s_decode_errors, s_preroll_errors);
    }
}

static void player_task(void *arg)
{
    (void)arg;
    size_t len = 0;
    uint8_t *data = load_file(INDEX_PATH, &len);
    tune_index_err_t err = data ? tune_index_open(&s_ix, data, len) : TUNE_INDEX_ERR_SIZE;
    if (!data || err != TUNE_INDEX_OK || s_ix.count == 0 || !tune_index_get(&s_ix, 0, &s_tune)) {
        ESP_LOGE(TAG, "%s: %s", INDEX_PATH, data ? (err ? tune_index_err_str(err) : "no tunes") : "cannot read");
        status_error("No tunes: run ./sync-tunes");
        vTaskSuspend(NULL);
    }
    ESP_LOGI(TAG, "%s: %u tunes", INDEX_PATH, s_ix.count);
    if (esp_mp3_dec_open(NULL, 0, &s_dec) != ESP_AUDIO_ERR_OK) {
        status_error("decoder failed");
        vTaskSuspend(NULL);
    }
    s_rate = s_tune.sample_rate;
    if (bsp_audio_init(s_rate) != ESP_OK || bsp_audio_start() != ESP_OK) {
        status_error("audio init failed");
        vTaskSuspend(NULL);
    }
    player_init(&s_pl);
    s_index_ready = true;
    uint16_t tune;
    uint32_t sample;
    if (s_resume && resume_find(&s_ix, s_resume_id, s_resume_ms, &tune, &sample)) {
        ESP_LOGI(TAG, "resuming tune id %08x at %u ms (paused)", (unsigned)s_resume_id, (unsigned)s_resume_ms);
        open_tune_at(tune, sample, true);
    } else {
        if (s_resume) {
            ESP_LOGW(TAG, "saved tune id %08x at %u ms is not in the index any more: first tune", (unsigned)s_resume_id,
                     (unsigned)s_resume_ms);
        }
        open_tune_at(0, UINT32_MAX, false);
    }

    for (;;) {
        cmd_t c;
        while (xQueueReceive(s_cmds, &c, 0) == pdTRUE) {
            handle(&c);
        }
        step();
        for (uint32_t n = player_new_underruns(&s_pl, bsp_audio_underruns()); n; n--) {
            char a[24];
            ESP_LOGW(TAG, "UNDERRUN #%u at %s", (unsigned)(s_pl.underruns_seen - n + 1),
                     log_time(a, sizeof(a), tune_ms(s_pl.position)));
        }
        if (s_pl.state == PLAYER_PLAYING && player_log_due(&s_pl)) {
            char now[16], total[16];
            text_mmss(now, sizeof(now), tune_ms(s_pl.position));
            text_mmss(total, sizeof(total), tune_ms(s_tune.total_samples));
            ESP_LOGI(TAG, "pos %s / %s underruns %u", now, total, (unsigned)s_pl.underruns_seen);
        }
        status_update(false);
    }
}

void player_task_start(bool resume, uint32_t resume_tune_id, uint32_t resume_ms)
{
    s_resume = resume;
    s_resume_id = resume_tune_id;
    s_resume_ms = resume_ms;
    s_cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    xTaskCreatePinnedToCore(player_task, "player", PLAYER_STACK, NULL, PLAYER_PRIO, &s_task, PLAYER_CORE);
}
