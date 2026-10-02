#pragma once

#include "buddy.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── BLE timing ────────────────────────────────────────────────── */
/* Advertising and scanning share one radio, so the two are budgeted together.
 *
 * The scan used to run an 800 ms window every 1000 ms — 80% of the radio —
 * while advertising at 900-1100 ms.  That leaves ~200 ms out of every second
 * for the three advertising events and, more to the point, for the receive
 * window after each one in which a CONNECT_IND can land.  A central that does
 * catch an advertisement frequently finds the peripheral not listening when it
 * answers, so the connection simply never establishes — which is why a phone
 * can see the badge in a scan list but not connect to it, and why the links
 * that do come up report LL control-procedure collisions (`547`/`554`) as the
 * two procedures fight for the same radio.
 *
 * Advertising fast is what buys the scan duty back: at 100-150 ms a central
 * finds us within a fraction of a second and has many chances to connect, so
 * the scan no longer needs to be near-continuous to discover peers.  The badge
 * is USB-powered, so the usual reason to advertise slowly does not apply. */
#define BUDDY_BLE_ADV_PERIOD_MS      125
#define BUDDY_BLE_ADV_JITTER_MS      25
#define BUDDY_BLE_SCAN_INTERVAL_MS   1000
#define BUDDY_BLE_SCAN_WINDOW_MS     300   /* 30% duty, was 80% */

/* ── GATT service / characteristic UUIDs (128-bit) ─────────────── */
#define BUDDY_SVC_UUID \
    0x4A,0x7B,0x80,0x01,0x9C,0x3D,0x4E,0x5F, \
    0xA1,0xB2,0xC3,0xD4,0xE5,0xF6,0xA7,0xB8

#define BUDDY_CHR_PROFILE_UUID \
    0x4A,0x7B,0x80,0x02,0x9C,0x3D,0x4E,0x5F, \
    0xA1,0xB2,0xC3,0xD4,0xE5,0xF6,0xA7,0xB8

#define BUDDY_CHR_PROFILE_WRITE_UUID \
    0x4A,0x7B,0x80,0x03,0x9C,0x3D,0x4E,0x5F, \
    0xA1,0xB2,0xC3,0xD4,0xE5,0xF6,0xA7,0xB8

/* Chat characteristic.
 * GATT is asymmetric per link: the central may only WRITE, the peripheral
 * may only NOTIFY back.  One characteristic carries both directions —
 * flags WRITE (peer -> us) plus NOTIFY (us -> peer). */
#define BUDDY_CHR_CHAT_UUID \
    0x4A,0x7B,0x80,0x04,0x9C,0x3D,0x4E,0x5F, \
    0xA1,0xB2,0xC3,0xD4,0xE5,0xF6,0xA7,0xB8

/* ── Manufacturer data identifiers ─────────────────────────────── */
#define BUDDY_MFG_COMPANY_ID  0x02E5   /* Espressif */
/* 2 = chat session protocol.  Peers running v1 are invisible to us and
 * vice versa, so the beacon parser must match this exactly. */
#define BUDDY_PROTO_VERSION   2

/* ── Advertising flag bits (mfg_buf[17]) ───────────────────────── */
#define BUDDY_FLAG_ACCEPTING     0x01  /* willing to accept a connection */
#define BUDDY_FLAG_PRIVACY       0x04  /* do not approach */
#define BUDDY_FLAG_CHAT_CAPABLE  0x08  /* firmware speaks the chat protocol (static) */
#define BUDDY_FLAG_NET_OK        0x10  /* has network right now, can participate (dynamic) */

/* ── Chat payload limits ───────────────────────────────────────── */
/* Hard ceiling on one chat frame.  The real limit is the negotiated
 * ATT MTU minus the 3-byte ATT header, whichever is smaller. */
#define BUDDY_CHAT_MSG_MAX       200
#define BUDDY_ATT_HEADER_LEN     3

/* ── Connection parameters for a chat session ──────────────────── */
/* The badge is USB-powered and chat latency matters more than power,
 * so ask for a short interval and a 4 s supervision timeout — the latter
 * doubles as a fast "owner walked away" detector. */
#define BUDDY_CONN_ITVL_MS       30
#define BUDDY_CONN_LATENCY       0
#define BUDDY_CONN_TIMEOUT_MS    4000

/* ── Chat transport events ─────────────────────────────────────── */
typedef enum {
    BUDDY_CHAT_RX_DATA = 0,     /* a frame arrived from the peer */
    BUDDY_CHAT_RX_LINK_READY,   /* chat path usable in both directions */
    BUDDY_CHAT_RX_LINK_LOST,    /* connection gone */
} buddy_chat_rx_kind_t;

typedef struct {
    buddy_chat_rx_kind_t kind;
    char  *text;      /* NUL-terminated, PSRAM.  Consumer frees. NULL for link events. */
    size_t len;
    int8_t rssi;
} buddy_chat_rx_t;

/**
 * Initialize BLE transport: NimBLE stack, GATT services, event queue.
 */
esp_err_t buddy_ble_init(void);

/**
 * Start advertising + scanning.
 */
esp_err_t buddy_ble_start(void);

/**
 * Stop advertising + scanning, disconnect any active connection.
 */
esp_err_t buddy_ble_stop(void);

/**
 * Get the event queue for contact processing.
 */
QueueHandle_t buddy_ble_get_event_queue(void);

/* ── Chat transport API ────────────────────────────────────────── */

/**
 * Queue carrying chat frames and link events.  Separate from the contact
 * event queue on purpose: the contact task makes LLM calls (and can block
 * for tens of seconds), so chat traffic must not queue behind it.
 */
QueueHandle_t buddy_ble_get_chat_queue(void);

/**
 * Largest payload one chat frame may carry on the current link.
 * Never zero (falls back to the 23-byte default ATT MTU).
 */
uint16_t buddy_ble_chat_max_len(void);

/**
 * Send one chat frame to the peer.
 * The central writes; the peripheral notifies.
 * @return ESP_OK, ESP_ERR_INVALID_STATE if the link or notify path is not ready,
 *         ESP_ERR_INVALID_ARG if len exceeds buddy_ble_chat_max_len().
 */
esp_err_t buddy_ble_chat_send(const char *data, size_t len);

/**
 * True when we are the central (initiator) of the active link, i.e. the side
 * that speaks first.
 */
bool buddy_ble_is_central(void);

/**
 * Peer device id of the active link ("aa:bb:.."), or "" when idle.
 */
const char *buddy_ble_peer_id(void);

/**
 * Tear down the active connection.  The chat session calls this when done;
 * the disconnect handler then posts BUDDY_CHAT_RX_LINK_LOST.
 */
void buddy_ble_terminate_link(void);

/**
 * Rebuild and re-publish the advertising fields.  Must be called when the
 * network state changes, otherwise BUDDY_FLAG_NET_OK goes stale.
 */
void buddy_ble_refresh_adv_flags(void);

/**
 * Current RSSI of the active link, or 0 when idle.
 */
int8_t buddy_ble_link_rssi(void);
