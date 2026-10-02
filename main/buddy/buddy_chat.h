#pragma once

#include "esp_err.h"

/**
 * Conversation session over the BLE chat characteristic.
 *
 * A session runs on whichever link BLE happened to establish: the central
 * (initiator) speaks first, the peripheral answers, and the turn then
 * alternates strictly.  Everything here is transport and turn-taking — what
 * actually gets *said* comes from buddy_chat.c's composer, which Phase 1
 * stubs out with an echo.
 */

/**
 * Wire up the session module.  Requires buddy_ble_init() to have run.
 */
esp_err_t buddy_chat_init(void);

/**
 * Start the session task.  Requires buddy_ble_start().
 */
esp_err_t buddy_chat_start(void);

/**
 * Ask any live session to wrap up and drop the link.
 */
void buddy_chat_stop(void);
