// Threading model
// ---------------
// can_mon_push()    — called from CAN task (priority 3).
// can_mon_snapshot()— called from the LVGL task via can_monitor_refresh().
// notify_trampoline — queued via lv_async_call; runs in the LVGL task.
//
// s_mutex protects the ring buffer AND the s_notify_pending flag, preventing the
// dual-core race where two tasks both pass the "!pending" check simultaneously.
// lv_async_call() is invoked only after s_mutex is released, and is wrapped in
// display_lock/display_unlock because lv_async_call internally calls lv_malloc,
// which is NOT thread-safe — without the lock a concurrent LVGL-task malloc can
// corrupt the TLSF heap on the second core.

#include "services/can_monitor.h"
#include "services/display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "can_mon";

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t     frame_num;   // rolling 0-100 counter
    can_frame_t frame;
    bool        is_tx;
    bool        is_usb;
    bool        is_sep;
    uint8_t     sep_num;
} can_mon_entry_t;

static can_mon_entry_t   s_log[CAN_MON_LOG_SIZE];
static int               s_head          = 0;
static int               s_count         = 0;
static uint8_t           s_frame_counter = 0;   // 0-100, wraps
static SemaphoreHandle_t s_mutex         = NULL;

// ---------------------------------------------------------------------------
// Notification
// ---------------------------------------------------------------------------

static void (*s_notify_fn)(void) = NULL;
static bool  s_notify_pending    = false;  // guarded by s_mutex

static void notify_trampoline(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_notify_pending = false;
    xSemaphoreGive(s_mutex);

    if (s_notify_fn) s_notify_fn();
}

// ---------------------------------------------------------------------------
// Decoder helpers
// ---------------------------------------------------------------------------

static void decode_led_layer(char *buf, size_t len, const char *label, const uint8_t *data)
{
    static const char c[4] = {'0', 'Z', 'P', 'S'};
    char tmp[16];
    int  off = 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) tmp[off++] = c[(data[i] >> (j * 2)) & 0x3];
        if (i != 2) tmp[off++] = '_';
    }
    tmp[off] = '\0';
    snprintf(buf, len, "%s:%s", label, tmp);
}

static void decode_remote_led_layer(char *buf, size_t len, const char *label, const uint8_t *data)
{
    static const char c[4] = {'0', 'Z', 'P', 'S'};
    char tmp[12];
    int  off = 0;
    for (int i = 2; i >= 1; i--) {
        for (int j = 0; j < 4; j++) tmp[off++] = c[(data[i] >> (j * 2)) & 0x3];
        if (i != 1) tmp[off++] = '_';
    }
    tmp[off] = '\0';
    snprintf(buf, len, "%s #%d:%s", label, data[0], tmp);
}

// ---------------------------------------------------------------------------
// Line formatter — full Narval protocol decoder
// ---------------------------------------------------------------------------

static void format_line(const can_mon_entry_t *e, char *buf, size_t len)
{
    if (e->is_sep) {
        snprintf(buf, len, "--------------------[%2d]--------------------\n", e->sep_num);
        return;
    }

    char prefix[16];
    if (e->is_usb && e->is_tx)
        snprintf(prefix, sizeof(prefix), "[%3u] USB TX", e->frame_num);
    else if (e->is_usb)
        snprintf(prefix, sizeof(prefix), "[%3u] USB", e->frame_num);
    else if (e->is_tx)
        snprintf(prefix, sizeof(prefix), "[%3u] TX", e->frame_num);
    else
        snprintf(prefix, sizeof(prefix), "[%3u]", e->frame_num);

    const uint8_t *d   = e->frame.data;
    int            dlc = (int)(e->frame.header.dlc < 8 ? e->frame.header.dlc : 8);

    // Narval decoder (RX and TX)
    char decoded[80];
    bool known = true;

    switch (dlc) {

    case 3:
        switch (d[2]) {
        case  0: snprintf(decoded, sizeof(decoded), "ERR [%02x_%02x_%02x]",  d[0], d[1], d[2]); break;
        case  1: snprintf(decoded, sizeof(decoded), "CALL %d->%d",           d[0]+1, d[1]+1); break;
        case  2: snprintf(decoded, sizeof(decoded), "CALL_END %d->%d",       d[0]+1, d[1]+1); break;
        case  4: snprintf(decoded, sizeof(decoded), "RING_START %d->%d",     d[0]+1, d[1]+1); break;
        case  8: snprintf(decoded, sizeof(decoded), "RING_END %d->%d",       d[0]+1, d[1]+1); break;
        case 16: snprintf(decoded, sizeof(decoded), "CALLB_REQ %d->%d",      d[0]+1, d[1]+1); break;
        case 32: snprintf(decoded, sizeof(decoded), "CALLB_TERM %d->%d",     d[0]+1, d[1]+1); break;
        default: known = false; break;
        }
        break;

    case 4: {
        int node = d[0] + 1;
        switch (d[3]) {

        case 0x00:
            snprintf(decoded, sizeof(decoded), "4B_ERR [%02x_%02x_%02x_%02x]", d[0], d[1], d[2], d[3]);
            break;

        case 0x01:
            snprintf(decoded, sizeof(decoded), "CLK@%d %02d:%02d:%02d",
                (d[0]>>5)&0x7, d[0]&0x1F, d[1]&0x3F, d[2]&0x3F);
            break;

        case 0x02:
            snprintf(decoded, sizeof(decoded), "DATE@%d 20%02d/%02d/%02d",
                (d[0]>>5)&0x7, d[0]&0x7F, d[1]&0x0F, d[2]&0x1F);
            break;

        case 0x03:
            snprintf(decoded, sizeof(decoded), "DOTINFO [%02x_%02x_%02x]", d[0], d[1], d[2]);
            break;

        case 0x04:
            switch (d[2]) {
            case 0x01: snprintf(decoded, sizeof(decoded), "MP3@%d START tr:%d",  node, d[1]+1); break;
            case 0x02: snprintf(decoded, sizeof(decoded), "MP3@%d STOP_ALL",     node); break;
            case 0x03: snprintf(decoded, sizeof(decoded), "MP3@%d STOP_ACT",     node); break;
            case 0x04: snprintf(decoded, sizeof(decoded), "MP3@%d DEL_PLAYLIST", node); break;
            case 0x05: snprintf(decoded, sizeof(decoded), "MP3@%d PAUSE",        node); break;
            default:   snprintf(decoded, sizeof(decoded), "MP3@%d cmd:0x%02x",   node, d[2]); break;
            }
            break;

        case 0x08:
            switch (d[2]) {
            case 0x00: snprintf(decoded, sizeof(decoded), "INTBSY@%d->%d busy",   node, d[1]+1); break;
            case 0x01: snprintf(decoded, sizeof(decoded), "INTBSY@%d->%d mtxbsy", node, d[1]+1); break;
            default:   snprintf(decoded, sizeof(decoded), "INTBSY@%d 0x%02x",     node, d[2]);   break;
            }
            break;

        case 0x10: decode_led_layer(decoded, sizeof(decoded), "L1-12",    d); break;
        case 0x11: decode_led_layer(decoded, sizeof(decoded), "L13-24",   d); break;
        case 0x12: decode_led_layer(decoded, sizeof(decoded), "L25-36",   d); break;
        case 0x13: decode_led_layer(decoded, sizeof(decoded), "L37-49",   d); break;
        case 0x14: decode_led_layer(decoded, sizeof(decoded), "L50-61",   d); break;
        case 0x15: decode_led_layer(decoded, sizeof(decoded), "L62-73",   d); break;
        case 0x16: decode_led_layer(decoded, sizeof(decoded), "L74-85",   d); break;
        case 0x17: decode_led_layer(decoded, sizeof(decoded), "L86-97",   d); break;

        case 0x18:
            switch (d[1]) {
            case 0x01: snprintf(decoded, sizeof(decoded), "BTN@%d press:%d",    node, d[2]+1); break;
            case 0x03: snprintf(decoded, sizeof(decoded), "BTN@%d grplearn:%d", node, d[2]+1); break;
            case 0x04: snprintf(decoded, sizeof(decoded), "BTN@%d led_qry:%d",  node, d[2]+1); break;
            case 0x08: snprintf(decoded, sizeof(decoded), "BTN@%d release:%d",  node, d[2]+1); break;
            default:   snprintf(decoded, sizeof(decoded), "BTN@%d 0x%02x",      node, d[1]);   break;
            }
            break;

        case 0x19: decode_led_layer(decoded, sizeof(decoded), "L98-109",  d); break;
        case 0x1A: decode_led_layer(decoded, sizeof(decoded), "L110-121", d); break;
        case 0x1B: decode_led_layer(decoded, sizeof(decoded), "L122-133", d); break;
        case 0x1C: decode_led_layer(decoded, sizeof(decoded), "L134-145", d); break;
        case 0x1D: decode_led_layer(decoded, sizeof(decoded), "L146-157", d); break;
        case 0x1E: decode_led_layer(decoded, sizeof(decoded), "L158-169", d); break;
        case 0x1F: decode_led_layer(decoded, sizeof(decoded), "L170-181", d); break;

        case 0x20: {
            char side = (d[0] & 0x80) ? 'k' : 't';
            int  src  = (d[0] & 0x7F) + 1;
            if (d[1] == 0x00) {
                static const char *const qnames[16] = {
                    "?", "?vol", "?ch", "?muvind", "?", "?bass", "?treb", "?hdmi",
                    "?", "?bright", "?", "?", "?", "?", "?", "?onoff"
                };
                const char *qn = (d[2] < 16) ? qnames[d[2]] : "?unk";
                snprintf(decoded, sizeof(decoded), "REM@%d%c QRY %s", src, side, qn);
            } else {
                static const char *const anames[16] = {
                    "?", "vol", "ch", "muvind", "?", "bass", "treb", "hdmi",
                    "?", "bright", "?", "?", "?", "?", "?", "onoff"
                };
                const char *an = (d[1] < 16) ? anames[d[1]] : "unk";
                snprintf(decoded, sizeof(decoded), "REM@%d%c %s=%d", src, side, an, d[2]);
            }
            break;
        }

        case 0x28: {
            static const char dtmf[12] = {'0','1','2','3','4','5','6','7','8','9','*','#'};
            switch (d[1]) {
            case 0x00: snprintf(decoded, sizeof(decoded), "PHN@%d callend->mod",    node); break;
            case 0x01: snprintf(decoded, sizeof(decoded), "PHN@%d pickup->mod",     node); break;
            case 0x02: snprintf(decoded, sizeof(decoded), "PHN@%d dtmf->mod:%c %dms",
                           node, (d[2]&0xF)<12?dtmf[d[2]&0xF]:'?', (d[2]>>4)*25); break;
            case 0x03: snprintf(decoded, sizeof(decoded), "PHN@%d ring->mod",       node); break;
            case 0x04: snprintf(decoded, sizeof(decoded), "PHN@%d ring_stop->mod",  node); break;
            case 0x08: snprintf(decoded, sizeof(decoded), "PHN@%d callend->pho",    node); break;
            case 0x09: snprintf(decoded, sizeof(decoded), "PHN@%d pickup->pho",     node); break;
            case 0x0A: snprintf(decoded, sizeof(decoded), "PHN@%d dtmf->pho:%c %dms",
                           node, (d[2]&0xF)<12?dtmf[d[2]&0xF]:'?', (d[2]>>4)*25); break;
            case 0x0B: snprintf(decoded, sizeof(decoded), "PHN@%d ring->pho",       node); break;
            case 0x0C: snprintf(decoded, sizeof(decoded), "PHN@%d ring_stop->pho",  node); break;
            default:   snprintf(decoded, sizeof(decoded), "PHN@%d 0x%02x",          node, d[1]); break;
            }
            break;
        }

        case 0x30: decode_remote_led_layer(decoded, sizeof(decoded), "AL129-136", d); break;
        case 0x31: decode_remote_led_layer(decoded, sizeof(decoded), "AL137-144", d); break;
        case 0x32: decode_remote_led_layer(decoded, sizeof(decoded), "AL145-152", d); break;
        case 0x33: decode_remote_led_layer(decoded, sizeof(decoded), "AL153-160", d); break;
        case 0x34: decode_remote_led_layer(decoded, sizeof(decoded), "AL161-168", d); break;
        case 0x35: decode_remote_led_layer(decoded, sizeof(decoded), "AL169-176", d); break;
        case 0x36: decode_remote_led_layer(decoded, sizeof(decoded), "AL177-184", d); break;
        case 0x37: decode_remote_led_layer(decoded, sizeof(decoded), "AL185-192", d); break;
        case 0x38: decode_remote_led_layer(decoded, sizeof(decoded), "AL193-200", d); break;
        case 0x39: decode_remote_led_layer(decoded, sizeof(decoded), "AL201-208", d); break;
        case 0x3A: decode_remote_led_layer(decoded, sizeof(decoded), "AL209-216", d); break;
        case 0x3B: decode_remote_led_layer(decoded, sizeof(decoded), "AL217-224", d); break;
        case 0x3C: decode_remote_led_layer(decoded, sizeof(decoded), "AL225-232", d); break;
        case 0x3D: decode_remote_led_layer(decoded, sizeof(decoded), "AL233-240", d); break;
        case 0x3E: decode_remote_led_layer(decoded, sizeof(decoded), "AL241-248", d); break;
        case 0x3F: decode_remote_led_layer(decoded, sizeof(decoded), "AL249-256", d); break;


        case 0xFF:
            snprintf(decoded, sizeof(decoded), "RESET node:%d", d[1]);
            break;

        default:
            known = false;
            break;
        }
        break;
    }

    default:
        known = false;
        break;
    }

    if (known) {
        snprintf(buf, len, "%s %s\n", prefix, decoded);
    } else {
        snprintf(buf, len, "%s 0x%03lx %dB: %02x %02x %02x %02x\n",
            prefix, (unsigned long)e->frame.header.id, dlc, d[0], d[1], d[2], d[3]);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void can_mon_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    assert(s_mutex);
    ESP_LOGI(TAG, "ready (log size %d)", CAN_MON_LOG_SIZE);
}

void can_mon_push(const can_frame_t *f, bool is_tx, bool is_usb)
{
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_log[s_head].frame_num = s_frame_counter;
    s_log[s_head].frame     = *f;
    s_log[s_head].is_tx     = is_tx;
    s_log[s_head].is_usb    = is_usb;
    s_log[s_head].is_sep    = false;
    s_frame_counter = (s_frame_counter >= 100) ? 0 : s_frame_counter + 1;
    s_head  = (s_head + 1) % CAN_MON_LOG_SIZE;
    if (s_count < CAN_MON_LOG_SIZE) s_count++;

    bool do_notify = (s_notify_fn && !s_notify_pending);
    if (do_notify) s_notify_pending = true;
    xSemaphoreGive(s_mutex);

    if (do_notify) {
        display_lock();
        lv_async_call(notify_trampoline, NULL);
        display_unlock();
    }
}

static int s_sep_counter = 0;

void can_mon_push_separator(void)
{
    if (!s_mutex) return;

    s_sep_counter = (s_sep_counter % 10) + 1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_log[s_head], 0, sizeof(s_log[s_head]));
    s_log[s_head].is_sep  = true;
    s_log[s_head].sep_num = (uint8_t)s_sep_counter;
    s_head  = (s_head + 1) % CAN_MON_LOG_SIZE;
    if (s_count < CAN_MON_LOG_SIZE) s_count++;

    bool do_notify = (s_notify_fn && !s_notify_pending);
    if (do_notify) s_notify_pending = true;
    xSemaphoreGive(s_mutex);

    if (do_notify) {
        display_lock();
        lv_async_call(notify_trampoline, NULL);
        display_unlock();
    }
}

// Called only from the LVGL task (ui_encoder_event). Identical ring-buffer push
// but omits display_lock() because the LVGL task already holds it — calling the
// regular variant from there would deadlock. The caller refreshes the UI directly.
void can_mon_push_separator_from_ui(void)
{
    if (!s_mutex) return;

    s_sep_counter = (s_sep_counter % 10) + 1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_log[s_head], 0, sizeof(s_log[s_head]));
    s_log[s_head].is_sep  = true;
    s_log[s_head].sep_num = (uint8_t)s_sep_counter;
    s_head  = (s_head + 1) % CAN_MON_LOG_SIZE;
    if (s_count < CAN_MON_LOG_SIZE) s_count++;
    xSemaphoreGive(s_mutex);
    // no lv_async_call — caller (ui_encoder_event) invokes can_monitor_refresh() directly
}

void can_mon_set_notify(void (*fn)(void))
{
    s_notify_fn = fn;
}

int can_mon_snapshot(char *out, size_t out_len)
{
    if (!s_mutex || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int    count = s_count;
    int    start = (s_head - count + CAN_MON_LOG_SIZE) % CAN_MON_LOG_SIZE;
    size_t pos   = 0;

    for (int i = 0; i < count && pos < out_len - 1; i++) {
        int  idx = (start + i) % CAN_MON_LOG_SIZE;
        char line[CAN_MON_LINE_MAX];
        format_line(&s_log[idx], line, sizeof(line));
        size_t ll = strlen(line);
        if (pos + ll >= out_len - 1) break;
        memcpy(out + pos, line, ll);
        pos += ll;
    }
    out[pos] = '\0';

    xSemaphoreGive(s_mutex);
    return (int)pos;
}
