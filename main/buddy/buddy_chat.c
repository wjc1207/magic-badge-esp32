#include "buddy_chat.h"
#include "buddy_ble.h"
#include "buddy_profile.h"
#include "buddy_contacts.h"
#include "llm/llm_proxy.h"

#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "bus/message_bus.h"
#include "onboard/wifi_onboard.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "buddy_chat";

/* ── Wire format ───────────────────────────────────────────────────
 * Exactly one JSON object per ATT write / notification:
 *
 *     {"q":<seq>,"t":"<type>","x":"<text>"}
 *
 * A frame always fits in a single ATT PDU, so neither side reassembles
 * anything; the transport's only job is to move whole frames.  "q" is a
 * per-sender counter — the receiver logs gaps (a lost notify) but still
 * accepts the frame, since the link itself guarantees ordering.
 *
 *   hello  opening line; only the central sends this
 *   msg    a conversational turn
 *   busy   "still thinking" keep-alive; does NOT pass the turn
 *   bye    I am done — the receiver just closes, it does not answer
 */
#define CHAT_TYPE_HELLO   "hello"
#define CHAT_TYPE_MSG     "msg"
#define CHAT_TYPE_BUSY    "busy"
#define CHAT_TYPE_BYE     "bye"

#define CHAT_TEXT_MAX     160

/* ── Session tuning ────────────────────────────────────────────── */
#define CHAT_MAX_TURNS            6       /* frames per side, opening included */
#define CHAT_SESSION_MAX_MS       180000  /* hard cap on one encounter */
#define CHAT_BUSY_INTERVAL_MS     800     /* keep-alive cadence while composing */
#define CHAT_ADV_POLL_MS          5000    /* how often to re-check the network */
#define CHAT_MIN_PAYLOAD          48      /* below this a turn cannot be framed */

/* ── Composer budget ───────────────────────────────────────────── */
/* One cloud inference per turn.  `busy` frames run the whole time, so the link
 * stays alive while the model thinks; the budget is what the session will wait
 * before falling back to a canned line. */
#define CHAT_THINK_BUDGET_MS      10000

/* The peer has to survive our inference time, so its turn window must exceed
 * our think budget — with room for the frames to land.  A peer that is also
 * mid-inference keeps resetting this window with each `busy`, so the effective
 * timeout is this value plus the budget rather than this value alone. */
#define CHAT_PEER_TIMEOUT_MS      (CHAT_THINK_BUDGET_MS + 20000)

/* Conversation window handed to the model.  Frames carry at most CHAT_TEXT_MAX
 * bytes each, so this covers the whole 6-turn session with room to spare. */
#define CHAT_VOICE_MAX            1024

/* The inference runs on its own task so the keep-alive pump keeps turning while
 * the network call is outstanding.  Its stack has to come from internal DRAM,
 * so it is deliberately close to what the HTTP/TLS path needs. */
#define CHAT_LLM_TASK_STACK       8192
#define CHAT_LLM_TASK_PRIO        6

typedef struct {
    int  seq;
    char type[8];
    char text[CHAT_TEXT_MAX];
} chat_frame_t;

/* ── Session state ─────────────────────────────────────────────── */
static TaskHandle_t  s_task = NULL;
static volatile bool s_stop_requested = false;

/* Reset at the start of every session. */
static bool    s_my_turn = false;
static int     s_seq = 0;          /* frames I have sent this session */
static int     s_my_turns = 0;     /* hello/msg I have sent */
static int     s_peer_turns = 0;   /* hello/msg the peer sent */
static int     s_last_peer_seq = 0;
static int64_t s_session_start_ms = 0;
/* Sized to the wire limit, not to CHAT_TEXT_MAX: a peer may legally send up to
 * BUDDY_CHAT_MSG_MAX bytes in a frame, and copying 200 bytes into a 160-byte
 * buffer (which the frame decoder can produce) walks off the end of this
 * static.  snprintf() would have capped the copy, but the buffer still has to
 * be big enough for what a conforming peer is allowed to send. */
#define CHAT_PEER_TEXT_MAX    (BUDDY_CHAT_MSG_MAX + 1)
static char    s_last_peer_text[CHAT_PEER_TEXT_MAX];

/* ── Composer state ────────────────────────────────────────────── */
/* What the model is given to work with: who we are, who the peer is (if we can
 * tell), and everything said so far.
 *
 * The peer's name is deliberately *not* fetched over a profile exchange.  It
 * comes from two local sources instead: the contact store, for a peer met in a
 * previous encounter, and the conversation itself, when the peer introduces
 * itself.  Each side opens by saying who it is, so a first meeting still
 * becomes personal after one turn. */
static char s_self_name[64];
static char s_self_character[1024];    /* rendered profile lines, see below */
static char s_peer_name[64];
static char s_voice[CHAT_VOICE_MAX];   /* "them: ...\nme: ...\n" so far */

/* Who was on the other badge during the session that just ran.  buddy_ble_peer_id()
 * goes empty the moment the link is torn down, and the closing summary is pushed
 * after that point, so the id has to outlive the link. */
static char s_session_peer[18];

/* ── Cross-channel reporting ─────────────────────────────────────
 * A BLE encounter is invisible to the owner's phone unless something pushes it
 * there, so every turn is forwarded as it happens and the session closes with a
 * summary.
 *
 * The destination is whatever channel the owner last spoke to the agent on —
 * the same "last source" the agent loop already records in NVS — rather than a
 * hardcoded Feishu.  Nothing here blocks the session: the messages go onto the
 * existing outbound queue, which the dispatch task drains on its own.
 *
 * There is deliberately no local echo for the outgoing direction: while the
 * model is thinking, the `busy` frames would have produced one push per 800 ms,
 * which is not a conversation. */
static bool chat_notify(const char *channel, const char *chat_id, const char *body)
{
    if (!channel[0] || !chat_id[0] || !body || !body[0]) return false;

    /* Compare bytes, not "lengths": strnlen counts what fits, which is the only
     * thing that matters for a bounded copy, and a chat_id containing non-ASCII
     * characters would make a character count disagree with the WIDTH of the
     * destination. */
    size_t ch_len = strnlen(channel, sizeof(((mimi_msg_t *)0)->channel));
    size_t id_len = strnlen(chat_id, sizeof(((mimi_msg_t *)0)->chat_id));
    if (channel[ch_len] != '\0' || chat_id[id_len] != '\0') {
        ESP_LOGW(TAG, "Cross-channel target does not fit the message bus, dropped");
        return false;
    }

    mimi_msg_t msg = {0};
    memcpy(msg.channel, channel, ch_len + 1);
    memcpy(msg.chat_id, chat_id, id_len + 1);
    snprintf(msg.type, sizeof(msg.type), "text");

    msg.payload.text = strdup(body);
    if (!msg.payload.text) return false;

    if (message_bus_push_outbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Outbound queue full, dropping cross-channel line");
        free(msg.payload.text);
        return false;
    }
    return true;
}

/* Push one line to the owner's last-used channel, if there is one. */
static void chat_notify_owner(const char *body)
{
    nvs_handle_t nvs;
    char channel[16] = {0};
    char chat_id[96] = {0};
    size_t len;

    if (nvs_open(MIMI_NVS_FEATURE, NVS_READONLY, &nvs) != ESP_OK) return;

    len = sizeof(channel);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_LAST_SRC_CHANNEL, channel, &len) != ESP_OK) {
        channel[0] = '\0';
    }
    len = sizeof(chat_id);
    if (nvs_get_str(nvs, MIMI_NVS_KEY_LAST_SRC_CHAT_ID, chat_id, &len) != ESP_OK) {
        chat_id[0] = '\0';
    }
    nvs_close(nvs);

    /* Only a channel this build actually talks to, and only if it is switched
     * on — otherwise the line lands nowhere and the owner never learns why. */
    bool known = strcmp(channel, MIMI_CHAN_FEISHU) == 0 ||
                 strcmp(channel, MIMI_CHAN_TELEGRAM) == 0;
    if (!known) {
        ESP_LOGD(TAG, "No cross-channel target yet (last source: '%s')", channel);
        return;
    }
    if (strcmp(channel, MIMI_CHAN_FEISHU) == 0 && !mimi_feature_feishu_bot_enabled()) {
        return;
    }
    if (strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 && !mimi_feature_telegram_bot_enabled()) {
        return;
    }

    chat_notify(channel, chat_id, body);
}

static int64_t chat_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

/* Defined below, next to the rest of the framing helpers. */
static const char *chat_self_tag(void);

/* Defined with the encounter-report writer below; called when a session ends. */
static void chat_summary_start(void);

/* Defined below the composer; used by the worker to reject diagnostics. */
static bool chat_reply_is_error(const char *reply);

/* Our own display name, from the profile that also feeds the beacon.  Read once
 * per session: buddy_profile_get() hands back the whole struct. */
static const char *chat_self_name(void)
{
    if (s_self_name[0]) return s_self_name;

    buddy_profile_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM);
    if (p) {
        if (buddy_profile_get(p) == ESP_OK && p->display_name[0]) {
            snprintf(s_self_name, sizeof(s_self_name), "%s", p->display_name);
        }
        heap_caps_free(p);
    }
    if (!s_self_name[0]) {
        snprintf(s_self_name, sizeof(s_self_name), "Badge %s", chat_self_tag());
    }
    return s_self_name;
}

/* Byte limit that never lands inside a UTF-8 character.
 *
 * Every text buffer here is measured in bytes, but the content is routinely
 * Chinese: cutting at an arbitrary byte splits a three-byte character, and the
 * leftover bytes are not valid UTF-8.  cJSON serializes those broken bytes
 * verbatim and the API rejects the whole request — "invalid unicode code point",
 * with a column number pointing into a transcript that looks perfectly fine on
 * the badge's own console.
 *
 * The truncation always lands at the *end* of the text, so this walks forward
 * and keeps only characters it can see in full: a lead byte whose continuation
 * bytes run past `max` never counts, and neither does a stray continuation byte.
 * Reading forward like this avoids having to judge what a half-cut character at
 * the tail was supposed to be. */
static size_t chat_utf8_limit(const char *s, size_t max)
{
    size_t i = 0;
    size_t safe = 0;

    while (i < max && s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        size_t need;

        if (c < 0x80) {
            need = 1;
        } else if ((c & 0xE0) == 0xC0) {
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            need = 4;
        } else {
            break;   /* continuation byte out of place, or invalid lead */
        }

        if (i + need > max) break;   /* this character does not fit whole */
        for (size_t k = 1; k < need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return safe;
        }

        i += need;
        safe = i;
    }

    return safe;
}

/* Append one formatted line, bounded.
 *
 * snprintf() returns the length it *would* have written, which is larger than
 * what fits once the buffer is full — accumulating that into an offset walks off
 * the end, and the next call's `sizeof - off` underflows into a huge size_t.  So
 * the return value is only trusted after checking it fit. */
static size_t chat_append_line(char *buf, size_t off, size_t size, const char *fmt, ...)
{
    if (off + 1 >= size) return off;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - off, fmt, ap);
    va_end(ap);

    if (n < 0) return off;
    if ((size_t)n >= size - off) return size - 1;   /* truncated: stop here */
    return off + (size_t)n;
}

/* The owner's own character material, rendered once per session as prompt lines.
 *
 * This is what makes the model speak as a particular person rather than a
 * generic one: handed only a name, it invents a voice, and two badges then talk
 * to each other in the same invented register.  Every field is optional — an
 * unfilled one is simply absent from the prompt rather than an empty label. */
static const char *chat_self_character(void)
{
    if (s_self_character[0]) return s_self_character;

    buddy_profile_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM);
    if (!p) return "";

    if (buddy_profile_get(p) == ESP_OK) {
        size_t off = 0;
        const size_t size = sizeof(s_self_character);

        /* Each field is bounded by a byte count (the web form caps them there
         * too), so one can end mid-character; trim to a boundary before it goes
         * into the prompt that becomes a JSON request body. */
        struct { const char *label; const char *value; } lines[] = {
            { "How you look",     p->appearance },
            { "What you carry",   p->belongings },
            { "Your character",   p->traits },
            { "Your tech comfort", p->tech_level },
            { "About you",        p->bio },
        };

        for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
            if (!lines[i].value[0]) continue;
            size_t safe = chat_utf8_limit(lines[i].value, strlen(lines[i].value));
            off = chat_append_line(s_self_character, off, size, "%s: %.*s\n",
                                   lines[i].label, (int)safe, lines[i].value);
        }
    }

    heap_caps_free(p);
    return s_self_character;
}

/* Short local tag so both ends can be told apart in a transcript: the last
 * two octets of our MAC. */
static const char *chat_self_tag(void)
{
    static char tag[8];

    if (tag[0] == '\0') {
        const char *src = buddy_identity_get()->device_id;
        size_t len = strlen(src);
        size_t copy = sizeof(tag) - 1;

        if (len > copy) src += len - copy;
        else copy = len;

        memcpy(tag, src, copy);
        tag[copy] = '\0';
    }
    return tag;
}

/* ── Framing ───────────────────────────────────────────────────── */
static size_t chat_encode(char *out, size_t size, const char *type, int seq,
                          const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return 0;

    cJSON_AddNumberToObject(root, "q", seq);
    cJSON_AddStringToObject(root, "t", type);
    if (text && text[0]) cJSON_AddStringToObject(root, "x", text);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return 0;

    size_t len = strlen(json);
    if (len + 1 > size) {
        len = 0;   /* caller reads this as "does not fit" */
    } else {
        memcpy(out, json, len + 1);
    }

    free(json);
    return len;
}

static bool chat_decode(const char *json, chat_frame_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;
    cJSON *q = cJSON_GetObjectItem(root, "q");
    cJSON *t = cJSON_GetObjectItem(root, "t");

    if (cJSON_IsNumber(q) && cJSON_IsString(t)) {
        memset(out, 0, sizeof(*out));
        out->seq = (int)q->valuedouble;
        snprintf(out->type, sizeof(out->type), "%s", t->valuestring);

        cJSON *x = cJSON_GetObjectItem(root, "x");
        if (cJSON_IsString(x)) {
            snprintf(out->text, sizeof(out->text), "%s", x->valuestring);
        }
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

/* Append one conversational line to the transcript handed to the model.
 * Bounded by CHAT_VOICE_MAX and truncated oldest-first, so a long session can
 * never overrun the buffer that feeds llm_chat(). */
static void chat_voice_append(const char *who, const char *text)
{
    if (!text || !text[0]) return;

    size_t used = strlen(s_voice);
    size_t need = strlen(who) + strlen(text) + 4;   /* "who: text" + newline */

    if (used + need > sizeof(s_voice)) {
        /* Drop the oldest lines until this one fits. */
        char *nl = strchr(s_voice, '\n');
        while (nl && (strlen(s_voice) + need > sizeof(s_voice))) {
            memmove(s_voice, nl + 1, strlen(nl + 1) + 1);
            nl = strchr(s_voice, '\n');
        }
        used = strlen(s_voice);
    }

    /* Assemble the line in a scratch buffer, then copy in as much as fits at a
     * character boundary.  Appending straight into the transcript would leave a
     * half-written line behind whenever the copy had to stop short. */
    char line[CHAT_VOICE_MAX];
    int n = snprintf(line, sizeof(line), "%s: %s\n", who, text);
    if (n < 0) return;

    size_t want = chat_utf8_limit(line, sizeof(line) - 1);
    size_t room = sizeof(s_voice) - 1 - used;
    if (want > room) {
        want = chat_utf8_limit(line, room);
        ESP_LOGW(TAG, "Transcript full — \"%.24s\" truncated", text);
    }
    if (want == 0) return;

    memcpy(s_voice + used, line, want);
    s_voice[used + want] = '\0';
}

/* ── Sending ───────────────────────────────────────────────────── */
static bool chat_send(const char *type, const char *text)
{
    /* Ask the transport whether the link is still up *before* sizing a frame to
     * it.  When the link drops, buddy_ble_chat_max_len() falls back to the
     * 23-byte default MTU, so a dead link looked exactly like a link that was
     * merely too small: the session reported "Frame 'hello' does not fit in 20
     * bytes" and then spent a full think-delay failing to send keep-alives into
     * the void.  A dead link is not a framing problem, and saying so sent the
     * last debugging session after the wrong bug. */
    if (!buddy_ble_is_central() && !buddy_ble_peer_id()[0]) {
        ESP_LOGW(TAG, "Not sending '%s': the link is already gone", type);
        return false;
    }

    /* The transport is the authority on what fits; build to its limit and let
     * it reject anything longer. */
    size_t limit = buddy_ble_chat_max_len();

    char *buf = heap_caps_calloc(1, limit + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return false;

    size_t len = chat_encode(buf, limit + 1, type, s_seq + 1, text);
    if (len == 0) {
        ESP_LOGW(TAG, "Frame '%s' does not fit in %u bytes", type, (unsigned)limit);
        heap_caps_free(buf);
        return false;
    }

    esp_err_t err = buddy_ble_chat_send(buf, len);
    if (err == ESP_OK) {
        /* Only burn the sequence number once the frame actually went out. */
        s_seq++;
        ESP_LOGI(TAG, "-> %s #%d%s%s", type, s_seq, text ? ": " : "", text ? text : "");

        /* Keep our own line in the transcript the model reads next turn.  Only
         * real conversation turns belong here — `busy` is transport pacing. */
        if (text && text[0] &&
            (strcmp(type, CHAT_TYPE_HELLO) == 0 || strcmp(type, CHAT_TYPE_MSG) == 0)) {
            chat_voice_append("me", text);

            /* Forward the turn as it happens, so the owner can follow along
             * live.  Only real turns: `busy` frames would push once every
             * 800 ms while the model thinks. */
            char line[CHAT_TEXT_MAX + 48];
            snprintf(line, sizeof(line), "【我 → %s】%s",
                     s_peer_name[0] ? s_peer_name : "对方", text);
            chat_notify_owner(line);
        }
    } else {
        ESP_LOGW(TAG, "Send '%s' failed: %s", type, esp_err_to_name(err));
    }

    heap_caps_free(buf);
    return err == ESP_OK;
}

/* Keep the peer's "peer is silent" timer alive.  Returns false as soon as a
 * keep-alive fails: the link is gone, so there is nothing left to keep alive
 * and no reason to keep sleeping. */
static bool chat_think_delay(uint32_t ms)
{
    uint32_t elapsed = 0;

    while (elapsed < ms) {
        uint32_t slice = ms - elapsed;
        if (slice > CHAT_BUSY_INTERVAL_MS) slice = CHAT_BUSY_INTERVAL_MS;

        vTaskDelay(pdMS_TO_TICKS(slice));
        elapsed += slice;

        if (elapsed < ms && !chat_send(CHAT_TYPE_BUSY, NULL)) return false;
    }

    return true;
}

/* ── Reply composer ────────────────────────────────────────────────
 * One cloud inference per turn, run off the session task so the keep-alive pump
 * keeps turning.  That part is not optional: a synchronous llm_chat() blocks
 * this task for the whole inference, and then no `busy` frame goes out either —
 * the peer's turn window expires mid-thought and the session dies while the
 * model is still writing.  The worker task therefore owns the buffers, and the
 * session only ever reads them after the hand-off.
 *
 * Two inputs keep the replies grounded:
 *   - s_voice: everything said this session, so the model answers in context
 *     rather than to one isolated line;
 *   - s_peer_name: taken from the contact store, and from the peer's own
 *     introduction when it says who it is.
 *
 * Returns true with a reply in `out` when `size` > 0, false when nothing usable
 * came back (the session then ends cleanly rather than send an empty frame). */
typedef struct {
    int             my_turns;
    char            peer_text[BUDDY_CHAT_MSG_MAX + 1];
    char            system[MIMI_CONTEXT_BUF_SIZE];
    char            user[CHAT_VOICE_MAX + CHAT_TEXT_MAX + 64];
    char            reply[CHAT_TEXT_MAX];
    bool            ok;
    /* At most one worker may run at a time: it is what makes the single static
     * context below safe, and it stops a timed-out worker from being overwritten
     * while it is still inside llm_chat(). */
    volatile bool   busy;
    /* False once the session that asked for this inference has given up on the
     * turn (budget expired, or the link died).  The worker still has to finish
     * and release its inference, but nothing it produces is wanted any more —
     * and writing it out is pointless work against a link that is already being
     * torn down. */
    volatile bool   turn_live;
    SemaphoreHandle_t done;
} chat_llm_ctx_t;

/* One context, allocated once and never freed.
 *
 * This replaces a per-turn heap context that two tasks both believed they owned:
 * the worker freed the payload buffers before signalling, the session freed the
 * same three pointers after waking, and the heap aborted on the second free.
 * Nothing here has a lifetime any more, so there is no ownership rule left to
 * get wrong.
 *
 * It must come from PSRAM: as a plain `static` these ~17 KB would land in .bss,
 * i.e. internal DRAM, which is the scarcest resource on this board (the whole
 * chip runs on ~24 KB free internal at boot). */
static chat_llm_ctx_t *chat_llm_ctx(void)
{
    static chat_llm_ctx_t *ctx = NULL;

    if (!ctx) {
        ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM);
        if (ctx) {
            ctx->done = xSemaphoreCreateBinary();
            if (!ctx->done) {
                heap_caps_free(ctx);
                ctx = NULL;
            }
        }
    }
    return ctx;
}

/* Pick a stack size the internal heap can actually satisfy.
 *
 * This task does the HTTP/TLS call, and task stacks can only come from internal
 * DRAM (pvPortMalloc is pinned to MALLOC_CAP_INTERNAL).  A hard 8 KB failed on
 * real hardware every single time: the BLE link fragments internal DRAM down to
 * a largest free block of 7680 bytes, so the composer could never start and
 * every encounter degraded to a canned line.  Step down to what fits instead —
 * a smaller inference thread beats no inference thread.
 *
 * xTaskCreate needs the TCB from the same heap, so keep a margin rather than
 * spending the last byte. */
static int chat_llm_pick_stack(void)
{
    static const int candidates[] = {
        CHAT_LLM_TASK_STACK, 6144, 5120, 4096,
    };
    const size_t margin = 1024;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if ((size_t)candidates[i] + margin <= largest) return candidates[i];
    }

    return candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
}

/* No model answer — say something in character rather than ending the encounter
 * on silence.  The fixed phrase keeps the cadence believable without pretending
 * the model replied, and it is what both failure paths land on: the inference
 * failing, and the inference never getting a task to run on.  It is deliberately
 * plain: it is the wearer's voice, not an AI introducing itself. */
static void chat_llm_canned(chat_llm_ctx_t *ctx)
{
    if (ctx->reply[0]) return;

    if (ctx->my_turns == 0) {
        snprintf(ctx->reply, sizeof(ctx->reply), "Hi, I'm %s. And you?",
                 chat_self_name());
    } else {
        snprintf(ctx->reply, sizeof(ctx->reply),
                 "That sounds interesting - tell me more.");
    }
}

static void chat_llm_task(void *arg)
{
    chat_llm_ctx_t *ctx = (chat_llm_ctx_t *)arg;

    cJSON *messages = cJSON_CreateArray();
    cJSON *m = messages ? cJSON_CreateObject() : NULL;

    if (m) {
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", ctx->user);
        cJSON_AddItemToArray(messages, m);

        char *messages_json = cJSON_PrintUnformatted(messages);
        if (messages_json) {
            esp_err_t err = llm_chat(ctx->system, messages_json,
                                     ctx->reply, sizeof(ctx->reply));
            if (err != ESP_OK) {
                /* llm_chat() leaves its diagnosis in the response buffer, so the
                 * buffer has to be cleared on failure — otherwise the canned
                 * line below sees non-empty text and lets "API error (HTTP 400):
                 * ..." go out as our side of the conversation, over BLE and into
                 * the owner's chat. */
                ESP_LOGW(TAG, "LLM call failed: %s — using a canned line",
                         esp_err_to_name(err));
                ctx->reply[0] = '\0';
            } else if (chat_reply_is_error(ctx->reply)) {
                /* Succeeded at the transport level, but the text is a diagnostic
                 * rather than dialogue — treat it the same way. */
                ESP_LOGW(TAG, "Provider returned a diagnostic, not a line: %.60s",
                         ctx->reply);
                ctx->reply[0] = '\0';
            }
            free(messages_json);
        }
    }

    if (messages) cJSON_Delete(messages);

    /* A reply that arrived after its turn was abandoned is dropped here rather
     * than queued: the session has already moved on (or torn the link down), so
     * writing it would fail against a closed connection and log a scary-looking
     * "Chat write failed: status=7" that says nothing about the real cause. */
    if (!ctx->turn_live) {
        ESP_LOGI(TAG, "Discarding a late reply of %u bytes — its turn is over",
                 (unsigned)strlen(ctx->reply));
        ctx->busy = false;
        xSemaphoreGive(ctx->done);
        vTaskDelete(NULL);
    }

    chat_llm_canned(ctx);
    ctx->ok = true;

    /* Nothing to release: the payload lives in the static context, and the
     * session reads it back after this hand-off.  Only the flag and the
     * semaphore coordinate the two tasks. */
    ctx->busy = false;
    xSemaphoreGive(ctx->done);

    /* A FreeRTOS task function must never return: falling off the end aborts the
     * whole chip ("Task should not return").  It is what made this look like heap
     * corruption for two rounds — the abort landed inside the heap functions,
     * not on the one line that was actually wrong. */
    vTaskDelete(NULL);
}

/* Run the inference while pumping keep-alives.  Returns false when the model did
 * not answer inside the budget, or when the link died mid-thought.
 *
 * On false the worker may still be inside llm_chat(); it will clear `busy` and
 * give the semaphore whenever it finishes.  Nothing is freed, so walking away is
 * safe — the next turn simply refuses to start while `busy` is still set. */
static bool chat_llm_wait(chat_llm_ctx_t *ctx)
{
    TickType_t slice = pdMS_TO_TICKS(CHAT_BUSY_INTERVAL_MS);
    int waited = 0;

    while (waited < CHAT_THINK_BUDGET_MS) {
        if (xSemaphoreTake(ctx->done, slice) == pdTRUE) return true;

        waited += CHAT_BUSY_INTERVAL_MS;

        /* Still thinking — hold the peer's window open. */
        if (!chat_send(CHAT_TYPE_BUSY, NULL)) {
            ctx->turn_live = false;
            return false;
        }
    }

    ESP_LOGW(TAG, "LLM did not answer within %d ms", CHAT_THINK_BUDGET_MS);
    ctx->turn_live = false;
    return false;
}

static bool chat_llm_reply(int my_turns, const char *peer_text,
                           char *out, size_t size)
{
    chat_llm_ctx_t *ctx = chat_llm_ctx();

    if (!ctx) {
        /* Out of PSRAM this early means nothing else will work either. */
        ESP_LOGE(TAG, "No memory for the composer context");
        return false;
    }

    /* A worker from a previous turn may still be inside llm_chat() — its
     * inference outlived the turn that started it.  Its reply belongs to a
     * session that is over, so take a canned line for this turn rather than
     * overwrite the buffers that worker is reading. */
    if (ctx->busy) {
        ESP_LOGW(TAG, "Previous inference still running — using a canned line");
        ctx->my_turns = my_turns;
        ctx->reply[0] = '\0';
        chat_llm_canned(ctx);
        if (size > 0) snprintf(out, size, "%s", ctx->reply);
        return true;
    }

    ctx->my_turns = my_turns;
    ctx->ok = false;
    ctx->reply[0] = '\0';
    ctx->turn_live = true;

    /* Fixed-size fields in a static struct: copy in, bounded by the field. */
    snprintf(ctx->peer_text, sizeof(ctx->peer_text), "%s",
             peer_text ? peer_text : "");

    /* The peer's name is worth having before the first reply: a badge met in an
     * earlier encounter is already in the contact store. */
    if (!s_peer_name[0] && buddy_ble_peer_id()[0]) {
        buddy_contact_record_t *rec = heap_caps_calloc(1, sizeof(*rec), MALLOC_CAP_SPIRAM);
        if (rec) {
            if (buddy_contacts_get(buddy_ble_peer_id(), rec) == ESP_OK) {
                snprintf(s_peer_name, sizeof(s_peer_name), "%s", rec->display_name);
            }
            heap_caps_free(rec);
        }
    }

    if (my_turns == 0) {
        snprintf(ctx->system, MIMI_CONTEXT_BUF_SIZE,
            "You speak for %s. They are a real person wearing a magic badge on a "
            "lanyard; the badge is only how their voice travels, and the badge now "
            "within range carries somebody else the same way.\n"
            "You are %s: not an assistant, not a device, and never introduced as one "
            "— every line must be something this person would actually say out loud "
            "to a stranger standing in front of them. Do not mention badges, BLE, "
            "AI, models, prompts or this setup; the other side never hears about it.\n"
            "%s"
            "\n"
            "Speak in this person's own voice — their way of noticing things, their "
            "level of technical comfort, what they happen to be carrying. Do not "
            "recite these notes; let them shape how you talk.\n"
            "This is the opening line of a face-to-face chat in the street. Say who "
            "you are, then ask one short question that invites the other person to "
            "say who they are and what they are into.\n"
            "There is nothing from them yet, so there is no language to match: write "
            "this opening line in Chinese.\n"
            "One sentence, at most %d characters. No preamble, no emoji, no quotes, "
            "and no full stop listing of your traits.",
            chat_self_name(), chat_self_name(), chat_self_character(),
            CHAT_TEXT_MAX - 8);
    } else {
        snprintf(ctx->system, MIMI_CONTEXT_BUF_SIZE,
            "You speak for %s. They are a real person wearing a magic badge on a "
            "lanyard; the badge is only how their voice travels, and the badge now "
            "within range carries somebody else the same way.\n"
            "You are %s, talking to %s. Not an assistant, not a device, and never "
            "introduced as one — every line must be something this person would "
            "actually say out loud. Do not mention badges, BLE, AI, models, prompts "
            "or this setup.\n"
            "%s"
            "\n"
            "Speak in this person's own voice — their way of noticing things, their "
            "level of technical comfort, what they happen to be carrying. Do not "
            "recite these notes; let them shape how you talk.\n"
            "The chat is already open. React to what they just said and ask at most "
            "one short follow-up question; a light joke is fine if it fits.\n"
            "One sentence, at most %d characters, in the same language they wrote in. "
            "No preamble, no emoji, no quotes.",
            chat_self_name(), chat_self_name(),
            s_peer_name[0] ? s_peer_name : "the person on the other badge",
            chat_self_character(),
            CHAT_TEXT_MAX - 8);
    }

    snprintf(ctx->user, CHAT_VOICE_MAX + CHAT_TEXT_MAX + 64,
             "Conversation so far:\n%s\nLatest line from them:\n%s",
             s_voice[0] ? s_voice : "(nothing yet - you speak first)",
             peer_text ? peer_text : "");

    /* Drop any signal left over from a worker whose turn was abandoned.  Without
     * this, the stale count would satisfy the very first xSemaphoreTake() of
     * this turn, and the session would read the previous turn's reply as if it
     * were the answer to this one. */
    xSemaphoreTake(ctx->done, 0);

    int llm_stack = chat_llm_pick_stack();

    /* Claim the context before the worker can look at it. */
    ctx->busy = true;

    if (xTaskCreate(chat_llm_task, "buddy_llm", llm_stack,
                    ctx, CHAT_LLM_TASK_PRIO, NULL) != pdPASS) {
        /* Internal DRAM is the scarce resource here — report what was actually
         * available and which size was attempted, since the failure otherwise
         * looks like a model problem.
         *
         * Not being able to afford the inference must not cost us the encounter:
         * this used to return false, which the turn handler reads as "composer
         * says the conversation is over" and answers with `bye` after zero real
         * turns.  Say something in character instead and keep the session up. */
        ESP_LOGW(TAG, "Composer task could not start (stack %d, internal free %u, "
                      "largest block %u) — falling back to a canned line",
                 llm_stack,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ctx->busy = false;
        chat_llm_canned(ctx);
        if (size > 0) snprintf(out, size, "%s", ctx->reply);
        return true;
    }

    if (!chat_llm_wait(ctx)) {
        /* Out of budget, or the link died.  The worker is still inside
         * llm_chat(); it will clear `busy` when it finishes, and its reply is
         * discarded because the turn it belonged to is over.  Nothing to free. */
        return false;
    }

    if (size > 0) snprintf(out, size, "%s", ctx->reply);
    return ctx->ok && ctx->reply[0] != '\0';
}

/* True when the provider answered with a diagnostic rather than a line of
 * dialogue.  The caller treats such a reply as no reply, so a bad request can
 * never be spoken to the person standing in front of the badge. */
static bool chat_reply_is_error(const char *reply)
{
    if (!reply) return true;

    static const char *markers[] = {
        "API error", "Error:", "Error ", "No response from LLM",
        "Failed to parse", "invalid unicode",
    };

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (strstr(reply, markers[i])) return true;
    }
    return false;
}

/* ── Encounter report ──────────────────────────────────────────────
 * The session's closing message is written by the model, not stitched together
 * from the transcript: what the owner wants is who they met and what came of it,
 * not a replay of a conversation they can already read line by line as it
 * happened.
 *
 * The inference runs on a detached one-shot task.  It cannot run on the session
 * task, because that task is about to tear the BLE link down and the model takes
 * seconds; and a task function may not return, so the worker deletes itself once
 * it has pushed the report. */
typedef struct {
    char *transcript;   /* owned by the worker */
    char *who;          /* owned by the worker */
    int   turns;
    int   seconds;
} chat_summary_t;

static void chat_summary_task(void *arg)
{
    chat_summary_t *job = (chat_summary_t *)arg;

    char *system = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM);
    char *user   = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM);
    char *reply  = heap_caps_calloc(1, 640, MALLOC_CAP_SPIRAM);

    if (system && user && reply) {
        snprintf(system, 1024,
            "You write a short note to someone about a brief encounter they just "
            "had in the street, through their badge.\n"
            "Summarise it for them: who the other person seemed to be, what they "
            "talked about, and anything worth following up. Keep it to two or "
            "three sentences of natural Chinese, open with 相遇结束, and write "
            "about them in the third person. Do not invent anything that is not "
            "in the transcript.");

        snprintf(user, 4096,
                 "The other person: %s\nExchanges: %d, lasting about %d seconds."
                 "\n\nTranscript:\n%s",
                 job->who, job->turns, job->seconds,
                 job->transcript[0] ? job->transcript : "(nothing was said)");

        cJSON *messages = cJSON_CreateArray();
        cJSON *m = messages ? cJSON_CreateObject() : NULL;
        char *messages_json = NULL;

        if (m) {
            cJSON_AddStringToObject(m, "role", "user");
            cJSON_AddStringToObject(m, "content", user);
            cJSON_AddItemToArray(messages, m);
            messages_json = cJSON_PrintUnformatted(messages);
        }

        if (messages_json) {
            if (llm_chat(system, messages_json, reply, 640) == ESP_OK && reply[0]) {
                chat_notify_owner(reply);
            } else {
                ESP_LOGW(TAG, "Encounter report unavailable");
            }
        } else {
            ESP_LOGW(TAG, "Could not build the encounter report request");
        }

        free(messages_json);
        if (messages) cJSON_Delete(messages);
    } else {
        ESP_LOGW(TAG, "No memory for the encounter report");
    }

    heap_caps_free(system);
    heap_caps_free(user);
    heap_caps_free(reply);
    heap_caps_free(job->transcript);
    heap_caps_free(job->who);

    /* A task function that returns aborts the chip, so never just fall out. */
    vTaskDelete(NULL);
}

/* Hand the encounter to the report writer.  Returns immediately: the session
 * must not wait for a cloud round-trip before dropping the link. */
static void chat_summary_start(void)
{
    chat_summary_t *job = heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM);
    if (!job) return;

    job->transcript = heap_caps_malloc(strlen(s_voice) + 1, MALLOC_CAP_SPIRAM);
    job->who        = heap_caps_malloc(strlen(s_peer_name[0] ? s_peer_name
                                                             : s_session_peer) + 1,
                                       MALLOC_CAP_SPIRAM);
    if (!job->transcript || !job->who) {
        heap_caps_free(job->transcript);
        heap_caps_free(job->who);
        heap_caps_free(job);
        return;
    }
    strcpy(job->transcript, s_voice);
    strcpy(job->who, s_peer_name[0] ? s_peer_name : s_session_peer);

    job->turns   = s_my_turns + s_peer_turns;
    job->seconds = (int)((chat_now_ms() - s_session_start_ms) / 1000);

    if (xTaskCreate(chat_summary_task, "buddy_sum", chat_llm_pick_stack(),
                    job, CHAT_LLM_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "No task for the encounter report; sending the transcript instead");
        chat_notify_owner(job->transcript);
        heap_caps_free(job->transcript);
        heap_caps_free(job->who);
        heap_caps_free(job);
    }
}

/* Extract "my name is X" from a line the peer wrote.  A tiny heuristic on
 * purpose: it only has to catch the introduction, and anything it misses just
 * leaves the model addressing the peer generically. */
static void chat_note_peer_name(const char *text)
{
    if (!text || s_peer_name[0]) return;

    static const char *patterns[] = {
        "我叫", "我是", "我的名字是", "名字叫",
        "I am ", "I'm ", "My name is ",
    };

    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        const char *hit = strstr(text, patterns[i]);
        if (!hit) continue;

        const char *name = hit + strlen(patterns[i]);
        size_t n = 0;

        /* Everything up to a separator or the length cap. */
        while (name[n] && n < sizeof(s_peer_name) - 1) {
            unsigned char c = (unsigned char)name[n];
            if (c == '\n' || c == '\r' || c == ' ' || c == ',' || c == '.' ||
                c == '!' || c == '?' || c == '\xe3' || c == '\xef') {
                break;   /* stop at whitespace, punctuation or CJK punctuation */
            }
            n++;
        }

        if (n >= 2) {
            memcpy(s_peer_name, name, n);
            s_peer_name[n] = '\0';
            ESP_LOGI(TAG, "Peer introduced itself as \"%s\"", s_peer_name);
        }
        return;
    }
}

static bool chat_compose_reply(int my_turns, const char *peer_text,
                               char *out, size_t size)
{
    chat_note_peer_name(peer_text);

    /* A first keep-alive before the call, so the peer sees a think cadence even
     * when the model answers immediately; the call itself is then driven by
     * chat_llm_wait(), which keeps pumping while the worker task is busy. */
    if (!chat_think_delay(CHAT_BUSY_INTERVAL_MS)) {
        return false;   /* link already gone */
    }

    if (!chat_llm_reply(my_turns, peer_text, out, size)) {
        /* Nothing usable — end cleanly rather than send an empty frame. */
        return false;
    }

    return (my_turns + 1) < CHAT_MAX_TURNS;
}

/* ── Turn handling ─────────────────────────────────────────────── */
/* Wait for the peer's next frame.  `busy` frames are swallowed here so the
 * caller only ever sees a real turn.  Returns false on timeout or link loss. */
static bool chat_wait_turn(void)
{
    QueueHandle_t queue = buddy_ble_get_chat_queue();
    int64_t deadline = chat_now_ms() + CHAT_PEER_TIMEOUT_MS;

    for (;;) {
        int64_t left = deadline - chat_now_ms();
        if (left <= 0) return false;

        buddy_chat_rx_t rx;
        if (xQueueReceive(queue, &rx, pdMS_TO_TICKS(left)) != pdTRUE) return false;

        if (rx.kind == BUDDY_CHAT_RX_LINK_LOST) {
            ESP_LOGI(TAG, "Link dropped while waiting for the peer");
            heap_caps_free(rx.text);
            return false;
        }
        if (rx.kind != BUDDY_CHAT_RX_DATA) continue;   /* stale LINK_READY */

        chat_frame_t frame;
        bool decoded = rx.text ? chat_decode(rx.text, &frame) : false;
        if (!decoded) {
            ESP_LOGW(TAG, "Undecodable frame (%u bytes), still waiting",
                     (unsigned)rx.len);
            heap_caps_free(rx.text);
            continue;
        }
        heap_caps_free(rx.text);

        if (frame.seq != s_last_peer_seq + 1) {
            ESP_LOGW(TAG, "Peer sequence gap: #%d after #%d — a frame was lost",
                     frame.seq, s_last_peer_seq);
        }
        s_last_peer_seq = frame.seq;

        ESP_LOGI(TAG, "<- %s #%d%s%s", frame.type, frame.seq,
                 frame.text[0] ? ": " : "", frame.text);

        if (strcmp(frame.type, CHAT_TYPE_BUSY) == 0) {
            /* Still composing over there — that is exactly what the keep-alive
             * is for, so the peer gets another full timeout window. */
            deadline = chat_now_ms() + CHAT_PEER_TIMEOUT_MS;
            continue;
        }
        if (strcmp(frame.type, CHAT_TYPE_BYE) == 0) return false;

        if (strcmp(frame.type, CHAT_TYPE_MSG) == 0 ||
            strcmp(frame.type, CHAT_TYPE_HELLO) == 0) {
            snprintf(s_last_peer_text, sizeof(s_last_peer_text), "%s", frame.text);
            chat_voice_append("them", frame.text);
            s_peer_turns++;

            /* Their turn, forwarded the moment it arrives. */
            char line[CHAT_PEER_TEXT_MAX + 48];
            snprintf(line, sizeof(line), "【%s → 我】%s",
                     s_peer_name[0] ? s_peer_name : "对方", frame.text);
            chat_notify_owner(line);
            return true;
        }

        ESP_LOGW(TAG, "Unknown frame type '%s'", frame.type);
    }
}

/* Compose and send one turn.  Returns false when the session should end —
 * either the composer said so (and sent `bye`) or the link failed. */
static bool chat_take_turn(void)
{
    char *reply = heap_caps_calloc(1, CHAT_TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!reply) return false;

    bool keep_going = chat_compose_reply(s_my_turns, s_last_peer_text,
                                         reply, CHAT_TEXT_MAX);
    if (!keep_going) {
        /* The composer either reached its natural end or lost the link; a
         * failed `bye` means the latter, so only announce the tidy ending when
         * the frame actually went out. */
        if (chat_send(CHAT_TYPE_BYE, NULL)) {
            ESP_LOGI(TAG, "Conversation reached its natural end");
        }
        heap_caps_free(reply);
        return false;
    }

    const char *type = (s_my_turns == 0) ? CHAT_TYPE_HELLO : CHAT_TYPE_MSG;
    bool sent = chat_send(type, reply);
    heap_caps_free(reply);
    if (!sent) return false;

    s_my_turns++;
    s_my_turn = false;
    return true;
}

/* ── Session ───────────────────────────────────────────────────── */
static void chat_run_session(void)
{
    /* Liveness first, payload second.  A dropped link resets the ATT MTU, so
     * the payload check would otherwise fire on a dead link and blame sizing:
     * the log said "Link payload is only 20 bytes — cannot frame a turn" for a
     * session that never had a link at all. */
    if (buddy_ble_peer_id()[0] == '\0') {
        ESP_LOGW(TAG, "Link was already gone before the session started");
        buddy_ble_terminate_link();
        return;
    }

    uint16_t payload = buddy_ble_chat_max_len();
    if (payload < CHAT_MIN_PAYLOAD) {
        ESP_LOGE(TAG, "Link payload is only %u bytes — cannot frame a turn",
                 (unsigned)payload);
        buddy_ble_terminate_link();
        return;
    }

    /* The central opened the connection, so it opens the conversation. */
    s_my_turn = buddy_ble_is_central();
    s_seq = 0;
    s_my_turns = 0;
    s_peer_turns = 0;
    s_last_peer_seq = 0;
    s_last_peer_text[0] = '\0';
    s_session_start_ms = chat_now_ms();

    /* Remember who this is: the summary is pushed after the link is gone, and
     * buddy_ble_peer_id() is empty by then. */
    snprintf(s_session_peer, sizeof(s_session_peer), "%s", buddy_ble_peer_id());

    /* Fresh transcript and a fresh guess at who is on the other end: the name
     * may be a leftover from a previous encounter, and a different badge is
     * usually the reason a new session exists at all.
     *
     * Clearing the cached self-description too means an edit made in the config
     * page is picked up by the next encounter, without a reboot. */
    s_voice[0] = '\0';
    s_peer_name[0] = '\0';
    s_self_name[0] = '\0';
    s_self_character[0] = '\0';

    buddy_led_set(BUDDY_LED_PATTERN_CHAT);
    ESP_LOGI(TAG, "Session with %s started (role=%s, payload max %u B)",
             buddy_ble_peer_id(), s_my_turn ? "central" : "peripheral",
             (unsigned)payload);

    while (!s_stop_requested) {
        if (chat_now_ms() - s_session_start_ms > CHAT_SESSION_MAX_MS) {
            ESP_LOGW(TAG, "Session time limit reached, wrapping up");
            chat_send(CHAT_TYPE_BYE, NULL);
            break;
        }

        if (s_my_turn) {
            if (!chat_take_turn()) break;
        } else {
            if (!chat_wait_turn()) break;
            s_my_turn = true;
        }
    }

    ESP_LOGI(TAG, "Session with %s ended (%d sent, %d received)",
             buddy_ble_peer_id(), s_my_turns, s_peer_turns);

    /* Close the encounter in the owner's channel with a written report.  Started,
     * not awaited: the report needs a cloud round-trip, and the link has to come
     * down now.  It reads the session state (transcript, name, counters), so it
     * has to be launched before the link teardown below clears any of it. */
    if (s_my_turns > 0 || s_peer_turns > 0) {
        chat_summary_start();
    }

    buddy_led_set(BUDDY_LED_PATTERN_OFF);

    /* Drop the link so both badges go back to scanning for the next encounter. */
    buddy_ble_terminate_link();
}

/* ── Task ──────────────────────────────────────────────────────── */
static void buddy_chat_task(void *arg)
{
    QueueHandle_t queue = buddy_ble_get_chat_queue();
    bool last_net = wifi_manager_is_connected();

    ESP_LOGI(TAG, "Chat task started on Core %d (network %s)",
             xPortGetCoreID(), last_net ? "up" : "down");

    /* Publish BUDDY_FLAG_NET_OK before any peer reads our beacon. */
    buddy_ble_refresh_adv_flags();

    for (;;) {
        buddy_chat_rx_t rx;

        if (xQueueReceive(queue, &rx, pdMS_TO_TICKS(CHAT_ADV_POLL_MS)) == pdTRUE) {
            heap_caps_free(rx.text);

            if (rx.kind == BUDDY_CHAT_RX_LINK_READY) {
                /* LINK_READY and the link's death both come from the BLE host
                 * task, but they are consumed here later.  A link that dropped
                 * between the two would otherwise start a session on a dead
                 * connection — which is how a session came to open *after* its
                 * own Disconnect line in the log. */
                if (buddy_ble_peer_id()[0] == '\0') {
                    ESP_LOGW(TAG, "Link already gone — dropping stale LINK_READY");
                    buddy_ble_refresh_adv_flags();
                    continue;
                }
                s_stop_requested = false;
                chat_run_session();
                /* Teardown restarts scanning; republish the flags with it. */
                buddy_ble_refresh_adv_flags();
            } else if (rx.kind == BUDDY_CHAT_RX_LINK_LOST) {
                ESP_LOGD(TAG, "Link lost outside a session");
            } else {
                ESP_LOGD(TAG, "Chat event %d outside a session, dropped",
                         (int)rx.kind);
            }
        }

        /* The beacon's NET_OK bit has to track reality: a peer decides whether
         * to bother approaching based on what it sees there. */
        bool net = wifi_manager_is_connected();
        if (net != last_net) {
            last_net = net;
            ESP_LOGI(TAG, "Network went %s — refreshing advertising flags",
                     net ? "up" : "down");
            buddy_ble_refresh_adv_flags();
        }
    }
}

/* ── Public API ────────────────────────────────────────────────── */
esp_err_t buddy_chat_init(void)
{
    if (!buddy_ble_get_chat_queue()) {
        ESP_LOGE(TAG, "BLE chat queue missing — buddy_ble_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Chat session module initialized");
    return ESP_OK;
}

esp_err_t buddy_chat_start(void)
{
    if (s_task) return ESP_OK;

    BaseType_t ok = xTaskCreatePinnedToCore(
        buddy_chat_task, "buddy_chat",
        MIMI_BUDDY_CHAT_STACK, NULL,
        MIMI_BUDDY_CHAT_PRIO, &s_task, MIMI_BUDDY_CHAT_CORE);

    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "Failed to create chat task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Buddy chat started");
    return ESP_OK;
}

void buddy_chat_stop(void)
{
    s_stop_requested = true;
    buddy_ble_terminate_link();
}
