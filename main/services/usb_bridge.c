/*
 * CAN-USB CDC bridge service
 *
 * Subscribes to the CAN service RX queue and forwards every received frame to
 * the USB host wrapped in the simple frame protocol:
 *   [0x55][len][data x len bytes][0xAA]
 *
 * Frames sent by the host in the same protocol are parsed and transmitted on
 * the CAN bus via can_service_send().  The CAN ID is fixed to 0x000 (STD);
 * the protocol carries payload bytes only.
 */

#include "services/usb_bridge.h"
#include "services/can.h"
#include "services/can_monitor.h"
#include "services/prog.h"
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usbd_core.h"
#include "usbd_cdc_acm.h"

static const char *TAG = "usb_bridge";

/* -- Frame protocol ----------------------------------------------------- */
#define FRAME_START         0x55u
#define FRAME_END           0xAAu
#define CAN_TX_TIMEOUT_MS   100

/* -- USB CDC endpoints -------------------------------------------------- */
#define CDC_IN_EP   0x81
#define CDC_OUT_EP  0x01
#define CDC_INT_EP  0x83

#define USBD_VID        0x303A// espressif's VID, for funsies.  Not actually registered for this product, but should be fine for development use.  (PID is in the public PID range, so no conflict there.)
#define USBD_PID        0x4001// not actually registered, but should be fine for development use.
#define USBD_MAX_POWER  100

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

#define CDC_FS_MAX_MPS  64
#ifdef CONFIG_USB_HS
#define CDC_HS_MAX_MPS  512
#define CDC_MAX_MPS     CDC_HS_MAX_MPS
#else
#define CDC_MAX_MPS     CDC_FS_MAX_MPS
#endif

#define ALIGN_UP(n, a)  (((n) + ((a) - 1)) & ~((a) - 1))

#define USB_READ_BUF_SIZE  ALIGN_UP(CDC_MAX_MPS, CONFIG_USB_ALIGN_SIZE)
/* Large enough for the biggest programming frame: [BB CC 01 <224 bytes> DD EE] = 229 B */
#define PROG_FRAME_MAX     (2u + 1u + 224u + 2u)
#define USB_WRITE_BUF_SIZE ALIGN_UP(PROG_FRAME_MAX, CONFIG_USB_ALIGN_SIZE)

/* -- Queues -------------------------------------------------------------- */
#define USB_RX_CHUNK_MAX    USB_READ_BUF_SIZE
#define USB_RX_QUEUE_DEPTH  8
#define CAN_RX_QUEUE_DEPTH  32

#define NOTIFY_USB_CONFIGURED_BIT   BIT0

typedef struct {
    uint8_t  data[USB_RX_CHUNK_MAX];
    uint32_t len;
} usb_rx_chunk_t;

/* DMA-safe buffers */
static DRAM_DMA_ALIGNED_ATTR uint8_t s_usb_read_buf[USB_READ_BUF_SIZE];
static DRAM_DMA_ALIGNED_ATTR uint8_t s_usb_write_buf[USB_WRITE_BUF_SIZE];

static QueueHandle_t     s_usb_rx_queue;      /* USB bulk-out → usb_to_can_task  */
static QueueHandle_t     s_can_rx_queue;      /* CAN service  → can_to_usb_task  */
static SemaphoreHandle_t s_usb_tx_sem;        /* binary: given = USB TX free     */
static volatile bool     s_usb_ready = false;
static TaskHandle_t      s_cdc_task_handle = NULL;
static uint32_t          s_mps = CDC_MAX_MPS;

/* ======================================================================= */
/* USB descriptors                                                           */
/* ======================================================================= */

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01,
                               USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor_fs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 2, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP,
                            CDC_IN_EP, CDC_FS_MAX_MPS, 0x02),
};

#ifdef CONFIG_USB_HS
static const uint8_t config_descriptor_hs[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 2, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP,
                            CDC_IN_EP, CDC_HS_MAX_MPS, 0x02),
};
#endif

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },   /* Langid         */
    "ESP32-S3",                      /* Manufacturer   */
    "CAN-USB Bridge",                /* Product        */
    "2026051200",                    /* Serial Number  */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
#ifdef CONFIG_USB_HS
    if (speed >= USB_SPEED_HIGH) {
        s_mps = CDC_HS_MAX_MPS;
        return config_descriptor_hs;
    }
    s_mps = CDC_FS_MAX_MPS;
#endif
    return config_descriptor_fs;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    if (index > 3) return NULL;
    return string_descriptors[index];
}

static const struct usb_descriptor cdc_descriptor = {
    .device_descriptor_callback         = device_descriptor_callback,
    .config_descriptor_callback         = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback         = string_descriptor_callback,
};

/* ======================================================================= */
/* USB CDC event callbacks  (ISR context)                                   */
/* ======================================================================= */

void usbd_event_handler(uint8_t busid, uint8_t event)
{
    BaseType_t woken = pdFALSE;
    switch (event) {
    case USBD_EVENT_CONFIGURED:
        s_usb_ready = true;
        xTaskNotifyFromISR(s_cdc_task_handle,
                           NOTIFY_USB_CONFIGURED_BIT, eSetBits, &woken);
        break;
    case USBD_EVENT_DISCONNECTED:
        s_usb_ready = false;
        /* Unblock any TX task waiting for bulk_in */
        xSemaphoreGiveFromISR(s_usb_tx_sem, &woken);
        break;
    default:
        break;
    }
    if (woken) portYIELD_FROM_ISR();
}

/* Host → device data (OUT endpoint). */
static void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (nbytes > 0) {
        usb_rx_chunk_t chunk;
        uint32_t len = (nbytes <= USB_RX_CHUNK_MAX) ? nbytes : USB_RX_CHUNK_MAX;
        memcpy(chunk.data, s_usb_read_buf, len);
        chunk.len = len;
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_usb_rx_queue, &chunk, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
    usbd_ep_start_read(0, ep, s_usb_read_buf, s_mps);
}

/* Write to host complete (IN endpoint). */
static void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if ((nbytes % s_mps) == 0 && nbytes) {
        /* Send ZLP to close the transfer */
        usbd_ep_start_write(0, ep, NULL, 0);
    } else {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(s_usb_tx_sem, &woken);
        if (woken) portYIELD_FROM_ISR();
    }
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr) {}
void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts) {}

static struct usbd_endpoint cdc_out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = usbd_cdc_acm_bulk_out };
static struct usbd_endpoint cdc_in_ep  = { .ep_addr = CDC_IN_EP,  .ep_cb = usbd_cdc_acm_bulk_in  };
static struct usbd_interface intf0;
static struct usbd_interface intf1;

/* ======================================================================= */
/* HAL — byte-level read/write used by usb_to_can_task and prog.c           */
/* ======================================================================= */

/* Byte-level iterator over the current USB RX chunk.
 * Only accessed from usb_to_can_task — no locking needed. */
static usb_rx_chunk_t s_cur_chunk = {{0}, 0};
static uint32_t       s_cur_pos   = 0;

int cdc_read_byte(uint8_t *byte, unsigned int timeout_ms)
{
    while (s_cur_pos >= s_cur_chunk.len) {
        TickType_t ticks = (timeout_ms == CDC_WAIT_FOREVER)
                           ? portMAX_DELAY
                           : pdMS_TO_TICKS(timeout_ms);
        if (xQueueReceive(s_usb_rx_queue, &s_cur_chunk, ticks) != pdTRUE) {
            s_cur_chunk.len = 0;
            s_cur_pos = 0;
            return 0;
        }
        s_cur_pos = 0;
    }
    *byte = s_cur_chunk.data[s_cur_pos++];
    return 1;
}

int cdc_write(const uint8_t *buf, unsigned int len)
{
    if (!s_usb_ready || len == 0 || len > USB_WRITE_BUF_SIZE) return 0;
    if (xSemaphoreTake(s_usb_tx_sem, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
    if (!s_usb_ready) {
        xSemaphoreGive(s_usb_tx_sem);
        return 0;
    }
    memcpy(s_usb_write_buf, buf, len);
    usbd_ep_start_write(0, CDC_IN_EP, s_usb_write_buf, len);
    /* Semaphore released by usbd_cdc_acm_bulk_in when transfer completes */
    return 1;
}

/* ======================================================================= */
/* Tasks                                                                     */
/* ======================================================================= */

/* CAN → USB: wrap each received CAN frame and send to the host. */
static void can_to_usb_task(void *arg)
{
    can_frame_t frame;
    uint8_t     buf[CAN_DATA_MAX_LEN + 3];

    while (1) {
        if (xQueueReceive(s_can_rx_queue, &frame, portMAX_DELAY) != pdTRUE)
            continue;
        if (!s_usb_ready) continue;

        uint8_t dlc = (frame.header.dlc <= CAN_DATA_MAX_LEN)
                      ? frame.header.dlc : CAN_DATA_MAX_LEN;
        buf[0] = FRAME_START;
        buf[1] = dlc;
        memcpy(&buf[2], frame.data, dlc);
        buf[2 + dlc] = FRAME_END;
        if (!cdc_write(buf, 3u + dlc)) {
            ESP_LOGW(TAG, "USB TX timeout, CAN frame dropped");
        }
    }
}

/* USB → CAN / programming dispatcher.
 * Reads the first byte of every frame and routes:
 *   0x55 → CAN bridge frame  [55 len data... AA]
 *   0xBB → programming frame [BB CC cmd ...  DD EE]  (prog.c handles the rest)
 */
static void usb_to_can_task(void *arg)
{
    uint8_t b;
    while (1) {
        if (!cdc_read_byte(&b, CDC_WAIT_FOREVER)) continue;

        if (b == FRAME_START) {
            /* CAN bridge: read len, data bytes, end byte */
            uint8_t len;
            if (!cdc_read_byte(&len, 500)) continue;
            if (len == 0 || len > CAN_DATA_MAX_LEN) continue;

            uint8_t data[CAN_DATA_MAX_LEN];
            bool ok = true;
            for (int i = 0; i < len; i++) {
                if (!cdc_read_byte(&data[i], 500)) { ok = false; break; }
            }
            if (!ok) continue;

            uint8_t tail;
            if (!cdc_read_byte(&tail, 500) || tail != FRAME_END) continue;

            can_frame_t tx = {0};
            tx.header.dlc = len;
            memcpy(tx.data, data, len);
            if (can_service_send(&tx, CAN_TX_TIMEOUT_MS) == ESP_OK) {
                can_mon_push(&tx, false, true);
            } else {
                ESP_LOGW(TAG, "CAN TX failed");
            }

        } else if (b == 0xBB) {
            /* Programming protocol — 0xBB already consumed */
            prog_poll();
        }
        /* else: ignore unknown framing byte */
    }
}

/* USB CDC task: init the stack, then handle configured/disconnect events. */
static void cdc_acm_task(void *arg)
{
    usbd_desc_register(0, &cdc_descriptor);
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf0));
    usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &intf1));
    usbd_add_endpoint(0, &cdc_out_ep);
    usbd_add_endpoint(0, &cdc_in_ep);

    usbd_initialize(0, ESP_USB_FS0_BASE, usbd_event_handler);

    ESP_LOGI(TAG, "USB CDC init done - waiting for host");

    uint32_t notify_value;
    while (1) {
        xTaskNotifyWait(0, 0xFFFFFFFF, &notify_value, portMAX_DELAY);
        if (notify_value & NOTIFY_USB_CONFIGURED_BIT) {
            /* Drain any leftover give, then mark semaphore free */
            xSemaphoreTake(s_usb_tx_sem, 0);
            xSemaphoreGive(s_usb_tx_sem);
            /* Kick off the first read */
            usbd_ep_start_read(0, CDC_OUT_EP, s_usb_read_buf, s_mps);
            ESP_LOGI(TAG, "USB configured - bridge active");
        }
    }
}

/* ======================================================================= */
/* Public API                                                                */
/* ======================================================================= */

esp_err_t usb_bridge_init(void)
{
    s_can_rx_queue = xQueueCreate(CAN_RX_QUEUE_DEPTH, sizeof(can_frame_t));
    if (!s_can_rx_queue) return ESP_ERR_NO_MEM;

    s_usb_rx_queue = xQueueCreate(USB_RX_QUEUE_DEPTH, sizeof(usb_rx_chunk_t));
    if (!s_usb_rx_queue) return ESP_ERR_NO_MEM;

    s_usb_tx_sem = xSemaphoreCreateBinary();
    if (!s_usb_tx_sem) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_usb_tx_sem);   /* start as free */

    can_service_add_rx_observer(s_can_rx_queue);

    /* CDC task must be pinned to core 0 (CherryUSB requirement) */
    xTaskCreatePinnedToCore(cdc_acm_task,    "cdc_acm",  4096, NULL, 10, &s_cdc_task_handle, 0);
    xTaskCreate(can_to_usb_task, "can2usb",  4096, NULL,  5,  NULL);
    xTaskCreate(usb_to_can_task, "usb2can",  4096, NULL,  5,  NULL);

    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}
