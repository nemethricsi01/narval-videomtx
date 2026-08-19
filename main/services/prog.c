#include "services/prog.h"
#include "services/usb_bridge.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "prog";

#define NVS_NS       "prog"
#define NVS_KEY_MAT  "matrix"
#define NVS_KEY_COL  "col_props"

/* Protocol framing */
#define SOF0  0xBBu
#define SOF1  0xCCu
#define EOF0  0xDDu
#define EOF1  0xEEu

#define CMD_WRITE   0x01u
#define CMD_COMMIT  0x02u
#define CMD_ABORT   0x03u
#define CMD_READ    0x04u

/* -----------------------------------------------------------------------
 * RAM storage — 449 bytes total, no heap.
 * ----------------------------------------------------------------------- */
static uint8_t s_matrix[PROG_MATRIX_LEN];          /* active  */
static uint8_t s_col_props[PROG_COL_PROPS_LEN];    /* active  */
static uint8_t s_stage_matrix[PROG_MATRIX_LEN];    /* staging */
static uint8_t s_stage_props[PROG_COL_PROPS_LEN];  /* staging */
static bool    s_has_staged;

static void (*s_commit_notify)(void) = NULL;

/* TX frame buffer — plain RAM is fine; cdc_write() copies into DMA buffer */
static uint8_t s_tx_frame[2 + 1 + PROG_PAYLOAD_LEN + 2]; /* 229 bytes */

/* -----------------------------------------------------------------------
 * NVS helpers
 * ----------------------------------------------------------------------- */
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = PROG_MATRIX_LEN;
    nvs_get_blob(h, NVS_KEY_MAT, s_matrix, &len);
    len = PROG_COL_PROPS_LEN;
    nvs_get_blob(h, NVS_KEY_COL, s_col_props, &len);
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_blob(h, NVS_KEY_MAT, s_matrix, PROG_MATRIX_LEN);
    nvs_set_blob(h, NVS_KEY_COL, s_col_props, PROG_COL_PROPS_LEN);
    nvs_commit(h);
    nvs_close(h);
}

/* -----------------------------------------------------------------------
 * Frame TX helper
 * ----------------------------------------------------------------------- */
static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t plen)
{
    uint8_t off = 0;
    s_tx_frame[off++] = SOF0;
    s_tx_frame[off++] = SOF1;
    s_tx_frame[off++] = cmd;
    if (payload && plen) {
        memcpy(&s_tx_frame[off], payload, plen);
        off += plen;
    }
    s_tx_frame[off++] = EOF0;
    s_tx_frame[off++] = EOF1;
    cdc_write(s_tx_frame, off);
}

/* -----------------------------------------------------------------------
 * Read-with-abort helper
 * On any timeout, clear staging and return from the caller via goto.
 * ----------------------------------------------------------------------- */
#define READ_OR_ABORT(b, ms)                       \
    do {                                           \
        if (!cdc_read_byte((b), (ms))) {           \
            s_has_staged = false;                  \
            return;                                \
        }                                          \
    } while (0)

/* -----------------------------------------------------------------------
 * Command handlers
 * ----------------------------------------------------------------------- */
static void handle_write(void)
{
    uint8_t payload[PROG_PAYLOAD_LEN];
    for (int i = 0; i < PROG_PAYLOAD_LEN; i++) {
        READ_OR_ABORT(&payload[i], 500);
    }
    uint8_t e0, e1;
    READ_OR_ABORT(&e0, 500);
    READ_OR_ABORT(&e1, 500);
    if (e0 != EOF0 || e1 != EOF1) { s_has_staged = false; return; }

    memcpy(s_stage_matrix, &payload[0],                 PROG_MATRIX_LEN);
    memcpy(s_stage_props,  &payload[PROG_MATRIX_LEN],   PROG_COL_PROPS_LEN);
    s_has_staged = true;

    send_frame(CMD_WRITE, payload, PROG_PAYLOAD_LEN);
    ESP_LOGI(TAG, "WRITE staged");
}

static void handle_commit(void)
{
    uint8_t e0, e1;
    READ_OR_ABORT(&e0, 500);
    READ_OR_ABORT(&e1, 500);
    if (e0 != EOF0 || e1 != EOF1) { s_has_staged = false; return; }

    bool committed = s_has_staged;
    if (committed) {
        memcpy(s_matrix,    s_stage_matrix, PROG_MATRIX_LEN);
        memcpy(s_col_props, s_stage_props,  PROG_COL_PROPS_LEN);
        nvs_save();
        s_has_staged = false;
        ESP_LOGI(TAG, "COMMIT saved to NVS");
    } else {
        ESP_LOGW(TAG, "COMMIT with no staged data");
    }
    send_frame(CMD_COMMIT, NULL, 0);

    // Let the host's COMMIT ack go out first — the notify callback may take
    // a while (can_latest_reconfigure broadcasts to every configured column).
    if (committed && s_commit_notify) s_commit_notify();
}

static void handle_abort(void)
{
    uint8_t e0, e1;
    READ_OR_ABORT(&e0, 500);
    READ_OR_ABORT(&e1, 500);
    if (e0 != EOF0 || e1 != EOF1) { s_has_staged = false; return; }

    s_has_staged = false;
    ESP_LOGI(TAG, "ABORT");
    send_frame(CMD_ABORT, NULL, 0);
}

static void handle_read(void)
{
    uint8_t e0, e1;
    READ_OR_ABORT(&e0, 500);
    READ_OR_ABORT(&e1, 500);
    if (e0 != EOF0 || e1 != EOF1) { s_has_staged = false; return; }

    uint8_t payload[PROG_PAYLOAD_LEN];
    memcpy(&payload[0],               s_matrix,    PROG_MATRIX_LEN);
    memcpy(&payload[PROG_MATRIX_LEN], s_col_props, PROG_COL_PROPS_LEN);
    send_frame(CMD_READ, payload, PROG_PAYLOAD_LEN);
    ESP_LOGI(TAG, "READ");
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void prog_init(void)
{
    memset(s_matrix,   0, sizeof(s_matrix));
    memset(s_col_props, 0, sizeof(s_col_props));
    s_has_staged = false;
    nvs_load();
    ESP_LOGI(TAG, "ready (%u+%u bytes loaded from NVS)",
             PROG_MATRIX_LEN, PROG_COL_PROPS_LEN);
}

void prog_poll(void)
{
    /* 0xBB already consumed by the dispatcher in usb_bridge.c */
    uint8_t b;
    READ_OR_ABORT(&b, 500);
    if (b != SOF1) return;   /* expected 0xCC */

    uint8_t cmd;
    READ_OR_ABORT(&cmd, 500);

    switch (cmd) {
    case CMD_WRITE:  handle_write();  break;
    case CMD_COMMIT: handle_commit(); break;
    case CMD_ABORT:  handle_abort();  break;
    case CMD_READ:   handle_read();   break;
    default:
        s_has_staged = false;
        ESP_LOGW(TAG, "unknown cmd 0x%02x", cmd);
        break;
    }
}

const uint8_t *prog_get_matrix(void)    { return s_matrix; }
const uint8_t *prog_get_col_props(void) { return s_col_props; }

void prog_set_commit_notify(void (*fn)(void))
{
    s_commit_notify = fn;
}
