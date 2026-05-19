#include "services/can_latest.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include "videomtx.h"
#include "services/column_properties.h"
#include "services/prog.h"
#include "services/matrix_config.h"
#include "services/can.h"
#include "services/can_monitor.h"
#include "esp_log.h"
#include "esp_err.h"

#define TAG "CAN_LATEST"

#define MAX_SEQ_LEN 16

typedef enum
{
    BUTTON_TYPE_HH = 1,
    BUTTON_TYPE_CSEND = 8,
} button_type_t;

typedef struct
{
    uint8_t button_id;
    button_type_t type;
    uint8_t dev_addr;
} button_event_t;

// LED sequence: buf holds the ordered LED indices for one mode,
// ptr cycles through them as button presses arrive.
typedef struct
{
    uint8_t buf[MAX_SEQ_LEN];  // LED indices to cycle through
    uint8_t leds[MAX_SEQ_LEN]; //
    uint8_t len;               // valid entries: 4=Normal, 6=6Step, up to 16=Radio
    uint8_t ptr;               // current position in buf
    uint8_t addrToSendTo[4];   // device addresses to send updates to for this column
    uint8_t mode;              // MODE_NORMAL / MODE_6STEP / MODE_RADIO_GREEN / MODE_RADIO_RED
    uint8_t base_addr;         // first LED layer address (PROP_LED_BASE_ADDR)
    uint8_t rows[MAX_SEQ_LEN]; // matrix row index for each buf entry
} led_seq_t;

static led_seq_t s_cols[16];             // one per column
static uint16_t s_addr_to_col_mask[256]; // bit N = column N has this address

uint8_t LedBuff[4] = {0}; // for sending led updates, holds the 4 bytes of the CAN data field
can_frame_t tx = {0};

// Stores the single most-recently-received CAN frame.
// Protected by a mutex so it can be read safely from any task (e.g. the UI).
static can_frame_t s_frame;
static bool s_valid = false; // false until the first frame arrives
static SemaphoreHandle_t s_mutex = NULL;

// Called once at startup (before can_service_init).
void can_latest_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    assert(s_mutex);
}

// Called once after prog_init() — reads the committed prog settings to build
// local state (ledOrder, per-column config) used during runtime frame handling.
void can_latest_configure(void)
{
    const uint8_t *matrix = prog_get_matrix();
    const uint8_t *col_props = prog_get_col_props();

    for (int col = 0; col < 16; col++)
    {
        uint8_t mode = column_props_get(col_props, col, PROP_MODE);
        s_cols[col].addrToSendTo[0] = column_props_get(col_props, col, PROP_DEV_ADDR_1);
        s_cols[col].addrToSendTo[1] = column_props_get(col_props, col, PROP_DEV_ADDR_2);
        s_cols[col].addrToSendTo[2] = column_props_get(col_props, col, PROP_DEV_ADDR_3);
        s_cols[col].addrToSendTo[3] = column_props_get(col_props, col, PROP_DEV_ADDR_4);
        s_cols[col].mode = mode;
        s_cols[col].base_addr = column_props_get(col_props, col, PROP_LED_BASE_ADDR);
        s_cols[col].len = 0;
        s_cols[col].ptr = 0;
        if (mode == MODE_NORMAL)
        {
            for (uint8_t row = 0; row < 16 && s_cols[col].len < 4; row++) // loop rows until we have 4 states for this column
            {
                uint8_t color = matrix_get_cell(matrix, row, col);
                if (color > 0)
                {
                    s_cols[col].rows[s_cols[col].len] = row;
                    s_cols[col].buf[s_cols[col].len++] = color - 1;
                }
            }
        }
        else if (mode == MODE_6STEP)
        {
            for (uint8_t row = 0; row < 16 && s_cols[col].len < 6; row++) // loop rows until we have 6 states for this column
            {
                uint8_t color = matrix_get_cell(matrix, row, col);
                if (color > 0)
                {
                    s_cols[col].rows[s_cols[col].len] = row;
                    s_cols[col].buf[s_cols[col].len++] = color - 1;
                }
            }
        }
        else if (mode == MODE_RADIO_GREEN || mode == MODE_RADIO_RED)
        {
            for (uint8_t row = 0; row < 16 && s_cols[col].len < MAX_SEQ_LEN; row++) // loop rows until we fill the buf for this column
            {
                uint8_t color = matrix_get_cell(matrix, row, col);
                if (color > 0)
                {
                    s_cols[col].rows[s_cols[col].len] = row;
                    s_cols[col].buf[s_cols[col].len++] = color - 1;
                }
            }
        }
    }

    memset(s_addr_to_col_mask, 0, sizeof(s_addr_to_col_mask));
    for (uint8_t col = 0; col < 16; col++)
    {
        for (uint8_t slot = 0; slot < 4; slot++)
        {
            uint8_t addr = s_cols[col].addrToSendTo[slot];
            if (addr != 0xFF)
            {
                s_addr_to_col_mask[addr] |= (1u << col);
            }
        }
    }
}


uint8_t baseAddressToLedLayer(uint8_t base_addr)
{
    if(base_addr == 0xFF || base_addr < 128)
        return 0xFF; // invalid base address
    base_addr -= 128;
    base_addr = 48 + (base_addr / 8); // convert from LED index to layer index
    return base_addr;
}

// Resolve which column index handles dev_addr.
// prefer_red=true  → pick the RADIO_RED column (CSEND)
// prefer_red=false → pick the non-RADIO_RED column (HH)
// Returns 0xFF on failure.
static uint8_t resolve_col(uint8_t dev_addr, bool prefer_red)
{
    uint16_t mask = s_addr_to_col_mask[dev_addr];
    uint8_t count = __builtin_popcount(mask);
    if (count == 0)
    {
        ESP_LOGE(TAG, "unknown addr 0x%02x", dev_addr);
        return 0xFF;
    }
    if (count > 2)
    {
        ESP_LOGE(TAG, "addr 0x%02x maps to %d cols", dev_addr, count);
        return 0xFF;
    }
    uint8_t col_a = __builtin_ctz(mask);
    if (count == 1)
        return col_a;
    uint8_t col_b = __builtin_ctz(mask & ~(1u << col_a));
    return ((s_cols[col_a].mode == MODE_RADIO_RED) == prefer_red) ? col_a : col_b;
}

// Send LedBuff to all device addresses registered for column col.
// Sends one frame (layer 56) for ≤8 LEDs, two frames for >8.
static void send_to_col_addrs(uint8_t col)
{
    tx.header.id = 0x000;
    tx.header.dlc = 4;
    for (uint8_t slot = 0; slot < 4; slot++)
    {
        uint8_t addr = s_cols[col].addrToSendTo[slot];
        if (addr == 0xFF)
            continue;
        tx.data[0] = addr - 1;
        tx.data[1] = LedBuff[1];
        tx.data[2] = LedBuff[0];
        tx.data[3] = baseAddressToLedLayer(s_cols[col].base_addr);
        esp_err_t ret = can_service_send(&tx, 100);
        if (ret == ESP_OK)
            can_mon_push(&tx, true, false);
        else
            ESP_LOGE(TAG, "TX failed: %s", esp_err_to_name(ret));
        if (s_cols[col].len > 8)
        {
            tx.data[1] = LedBuff[3];
            tx.data[2] = LedBuff[2];
            tx.data[3] = baseAddressToLedLayer(s_cols[col].base_addr) + 1;
            ret = can_service_send(&tx, 100);
            if (ret == ESP_OK)
                can_mon_push(&tx, true, false);
            else
                ESP_LOGE(TAG, "TX failed: %s", esp_err_to_name(ret));
        }
    }
}

// Called by can_task for every received CAN frame.
// Runs in the CAN task — safe to call videomtx_set() here (not the LVGL task).
void can_latest_update(const can_frame_t *frame)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_frame = *frame;
    s_valid = true;
    xSemaphoreGive(s_mutex);

    uint8_t sel_idx = 0xFF;

    if (frame->header.dlc != 4 || frame->data[3] != 0x18)
        return;

    button_event_t event = {
        .button_id = frame->data[2] + 1,
        .type = frame->data[1],
        .dev_addr = frame->data[0] + 1,
    };

    if (event.type != BUTTON_TYPE_HH && event.type != BUTTON_TYPE_CSEND)
        return;

    bool prefer_red = (event.type == BUTTON_TYPE_CSEND);
    uint8_t col = resolve_col(event.dev_addr, prefer_red);
    if (col == 0xFF)
        return;

    if (!prefer_red && s_cols[col].mode == MODE_RADIO_RED)
    {
        ESP_LOGE(TAG, "HH addr 0x%02x col %d is RADIO_RED", event.dev_addr, col);
        return;
    }
    if (prefer_red && s_cols[col].mode != MODE_RADIO_RED)
    {
        ESP_LOGE(TAG, "CSEND addr 0x%02x col %d not RADIO_RED", event.dev_addr, col);
        return;
    }

    int16_t whichLed = event.button_id - s_cols[col].base_addr;
    if (whichLed < 0 || (uint8_t)whichLed >= s_cols[col].len || (s_cols[col].mode == MODE_6STEP && whichLed >= 2)||(s_cols[col].mode == MODE_NORMAL && whichLed >= 1))
    {
        ESP_LOGE(TAG, "button_id %d OOR col %d (base=%d len=%d)",
                 event.button_id, col, s_cols[col].base_addr, s_cols[col].len);
        return;
    }


    if (s_cols[col].mode == MODE_RADIO_GREEN || s_cols[col].mode == MODE_RADIO_RED)
    {
        bool is_green = (s_cols[col].mode == MODE_RADIO_GREEN);
        sel_idx = (uint8_t)whichLed;
        for (uint8_t led = 0; led < s_cols[col].len; led++)
        {
            uint8_t byte_idx = led / 4;
            uint8_t bit = is_green ? (led % 4) * 2 : (led % 4) * 2 + 1;
            if (led == (uint8_t)whichLed)
                LedBuff[byte_idx] |= (1u << bit);
            else
                LedBuff[byte_idx] &= ~(1u << bit);
        }
    }
    else if (s_cols[col].mode == MODE_NORMAL)
    {
        sel_idx = s_cols[col].ptr;
        s_cols[col].leds[0] = s_cols[col].buf[s_cols[col].ptr++];
        if (s_cols[col].ptr >= s_cols[col].len)
            s_cols[col].ptr = 0;
        LedBuff[0] = s_cols[col].leds[0];
    }
    else if (s_cols[col].mode == MODE_6STEP)
    {
        uint8_t half = s_cols[col].len / 2;
        if (whichLed == 0)
        {
            if (s_cols[col].ptr >= half)
                s_cols[col].ptr = 0;
            sel_idx = s_cols[col].ptr;
            s_cols[col].leds[0] = s_cols[col].buf[s_cols[col].ptr++];
            if (s_cols[col].ptr >= half)
                s_cols[col].ptr = 0;
        }
        else
        {
            if (s_cols[col].ptr < half)
                s_cols[col].ptr = half;
            sel_idx = s_cols[col].ptr;
            s_cols[col].leds[0] = s_cols[col].buf[s_cols[col].ptr++];
            if (s_cols[col].ptr >= s_cols[col].len)
                s_cols[col].ptr = half;
        }
        LedBuff[0] = s_cols[col].leds[0] << (whichLed * 2);
    }

    ESP_LOG_BUFFER_HEX(TAG, LedBuff, sizeof(LedBuff));
    if (sel_idx != 0xFF)
    videomtx_set(col, s_cols[col].rows[sel_idx]);
    send_to_col_addrs(col);
}

// Returns the latest received frame into *out.
// Returns false (and leaves *out untouched) if no frame has arrived yet.
// Safe to call from any task.
bool can_latest_get(can_frame_t *out)
{
    if (!s_mutex || !out)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_valid;
    if (v)
        *out = s_frame;
    xSemaphoreGive(s_mutex);
    return v;
}
