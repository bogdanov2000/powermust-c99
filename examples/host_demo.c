/* SPDX-License-Identifier: Apache-2.0
 * Запускаемый пример БЕЗ реального ИБП. Показывает интерфейс callback.
 * Никакие команды управления/выключения здесь не отправляются. */
#include "powermust.h"
#include <stdio.h>
#include <string.h>

typedef struct { const char *reply; size_t offset; } demo_io;

static int read_byte(void *user, uint8_t *byte)
{
    demo_io *io = user;
    if (io->reply == NULL || io->reply[io->offset] == '\0') return 0;
    *byte = (uint8_t)io->reply[io->offset++];
    return 1;
}

static bool write_frame(void *user, const uint8_t *data, size_t size)
{
    demo_io *io = user;
    printf("TX: %.*s\\r\n", (int)(size - 1u), (const char *)data);
    io->offset = 0u;
    io->reply = size == 3u && memcmp(data, "Q1\r", 3u) == 0 ?
        "(232.9 232.9 232.9 003 49.9 13.4 25.0 00001000\r\n" :
        "#220.0 003 12.00 50.0\r\n";
    return true;
}

static void event(void *user, const pm_context *ctx, pm_event type, pm_result result)
{
    (void)user;
    if (type == PM_EVENT_STATUS) {
        printf("Q1: input=%u.%02u V, battery=%u.%02u V, flags=0x%02x\n",
            (unsigned int)(ctx->status.input_cV / 100u),
            (unsigned int)(ctx->status.input_cV % 100u),
            (unsigned int)(ctx->status.battery_cV / 100u),
            (unsigned int)(ctx->status.battery_cV % 100u),
            (unsigned int)ctx->status.flags);
    } else if (type == PM_EVENT_RATING) puts("F: nominal values updated");
    else printf("event=%d result=%d\n", (int)type, (int)result);
}

int main(void)
{
    demo_io io = { NULL, 0u };
    pm_context ctx;
    pm_config config = pm_default_config(read_byte, write_frame, &io);
    uint32_t now;
    config.event = event;
    if (pm_init(&ctx, &config, 0u) != PM_OK) return 1;
    for (now = 0u; now < 5000u; now += 10u) pm_service(&ctx, now);
    printf("Host context: %zu bytes; Q1 fresh: %s\n", sizeof(ctx),
           pm_status_fresh(&ctx, now, 30000u) ? "yes" : "no");
    return 0;
}
