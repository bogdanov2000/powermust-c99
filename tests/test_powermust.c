/* SPDX-License-Identifier: Apache-2.0
 * Тесты работают без ИБП: искусственный UART задаёт байты и время.
 * Проверяем не только числа, но и опасные ошибки состояния/связи. */
#include "powermust.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static const char q1[] = "(232.9 232.9 232.9 003 49.9 13.4 25.0 00001000";
static const char f[] = "#220.0 003 12.00 50.0";

typedef struct {
    uint8_t rx[4096];
    size_t rx_head, rx_tail;
    char tx[256][PM_COMMAND_CAPACITY + 1u];
    unsigned int tx_count, events;
    pm_event last_event;
    pm_result last_result;
    bool tx_fail, rx_fail;
} fake_io;

static int fake_read(void *user, uint8_t *byte)
{
    fake_io *io = user;
    if (io->rx_fail) { io->rx_fail = false; return -1; }
    if (io->rx_head == io->rx_tail) return 0;
    *byte = io->rx[io->rx_tail++];
    return 1;
}

static bool fake_write(void *user, const uint8_t *data, size_t size)
{
    fake_io *io = user;
    CHECK(size <= PM_COMMAND_CAPACITY);
    CHECK(io->tx_count < 256u);
    memcpy(io->tx[io->tx_count], data, size);
    io->tx[io->tx_count++][size] = '\0';
    return !io->tx_fail;
}

static void fake_event(void *user, const pm_context *ctx, pm_event event, pm_result result)
{
    fake_io *io = user;
    CHECK(ctx->state == PM_RECOVER); /* callback видит завершённое состояние. */
    ++io->events;
    io->last_event = event;
    io->last_result = result;
}

static void setup(pm_context *ctx, fake_io *io, uint32_t now, bool automatic)
{
    pm_config config = pm_default_config(fake_read, fake_write, io);
    memset(io, 0, sizeof(*io));
    config.timeout_ms = 100u;
    config.recovery_quiet_ms = 20u;
    config.allow_control = true;
    config.event = fake_event;
    if (!automatic) config.q1_interval_ms = config.f_interval_ms = 0u;
    CHECK(pm_init(ctx, &config, now) == PM_OK);
}

static void feed(fake_io *io, const void *data, size_t size)
{
    /* Ничего не имитируем через C-строки: разрешены NUL и любой мусор. */
    if (io->rx_head == io->rx_tail) io->rx_head = io->rx_tail = 0u;
    CHECK(io->rx_head + size <= sizeof(io->rx));
    memcpy(io->rx + io->rx_head, data, size);
    io->rx_head += size;
}

static void reply(pm_context *ctx, fake_io *io, const char *text, uint32_t now)
{
    feed(io, text, strlen(text));
    feed(io, "\r", 1u);
    pm_service(ctx, now);
}

static void test_parser(void)
{
    pm_status s, before;
    pm_rating rating;
    size_t i;
    CHECK(pm_parse_q1(q1, sizeof(q1) - 1u, &s) == PM_OK);
    CHECK(s.input_cV == 23290u && s.output_cV == 23290u);
    CHECK(s.load_percent == 3u && s.frequency_cHz == 4990u);
    CHECK(s.battery_cV == 1340u && s.temperature_cC == 2500);
    CHECK(s.flags == PM_FLAG_STANDBY && s.valid_fields == 255u);
    CHECK(pm_parse_f(f, sizeof(f) - 1u, &rating) == PM_OK);
    CHECK(rating.voltage_cV == 22000u && rating.current_cA == 300u);
    CHECK(rating.battery_cV == 1200u && rating.frequency_cHz == 5000u);
    CHECK(rating.valid_fields == 15u);
    {
        const char online[] = "(208.4 140.0 208.4 034 59.9 2.05 35.0 00110000";
        CHECK(pm_parse_q1(online, sizeof(online) - 1u, &s) == PM_OK);
        CHECK(s.battery_cV == 205u && s.load_percent == 34u);
        CHECK(s.flags == (PM_FLAG_BYPASS_AVR | PM_FLAG_UPS_FAILED));
    }
    {
        const char placeholders[] = "(@@@.@ 000.0 230.0 @@@ 50.0 @.@@ -30.5 10001000";
        CHECK(pm_parse_q1(placeholders, sizeof(placeholders) - 1u, &s) == PM_OK);
        CHECK(s.temperature_cC == -3050 && s.flags == 0x88u);
        CHECK(s.valid_fields == (uint8_t)(255u & ~(1u | 8u | 32u)));
        CHECK(s.input_cV == 0u && s.battery_cV == 0u);
    }
    {
        const char fractions[] = "#230.00 3.50 48.12 50.01";
        CHECK(pm_parse_f(fractions, sizeof(fractions) - 1u, &rating) == PM_OK);
        CHECK(rating.current_cA == 350u && rating.battery_cV == 4812u);
        const char all_unknown[] = "#@@@.@ @@@ @@.@@ @@.@";
        CHECK(pm_parse_f(all_unknown, sizeof(all_unknown) - 1u, &rating) == PM_OK);
        CHECK(rating.valid_fields == 0u);
    }
    /* Каждый флаг отдельно: обнаруживает перепутанный порядок битов. */
    for (i = 0u; i < 8u; ++i) {
        char frame[sizeof(q1)];
        size_t first = sizeof(q1) - 1u - 8u;
        memcpy(frame, q1, sizeof(q1));
        memset(frame + first, '0', 8u);
        frame[first + i] = '1';
        CHECK(pm_parse_q1(frame, sizeof(frame) - 1u, &s) == PM_OK);
        CHECK(s.flags == (uint8_t)(128u >> i));
    }
    before = s;
    /* У каждого усечения выход остаётся тем же: полупакет не публикуем. */
    for (i = 0u; i < sizeof(q1) - 1u; ++i) {
        CHECK(pm_parse_q1(q1, i, &s) == PM_BAD_FRAME);
        CHECK(memcmp(&s, &before, sizeof(s)) == 0);
    }
    {
        const char *bad[] = {
            "", "(", "(NAK", "(1 2 3 4 5 6 7 0000000",
            "(1 2 3 4 5 6 7 000000000", "(1 2 3 4 5 6 7 00000002",
            "(1 2 3 4 5 6 7 00000000 EXTRA", "(1 2 3 4 5 6 7 00000000 ",
            "(NaN 2 3 4 5 6 7 00000000", "(inf 2 3 4 5 6 7 00000000",
            "(-1 2 3 4 5 6 7 00000000", "(+1 2 3 4 5 6 7 00000000",
            "(1e2 2 3 4 5 6 7 00000000", "(1.234 2 3 4 5 6 7 00000000",
            "(1. 2 3 4 5 6 7 00000000", "(1 2 3 4.5 6 7 8 00000000",
            "(655.36 2 3 4 5 6 7 00000000", "(1 2 3 1000 5 6 7 00000000",
            "(1 2 3 4 5 6 327.68 00000000", "(1 2 3 4 5 6 -327.68 00000000",
            "(1@.0 2 3 4 5 6 7 00000000", "(1 2 3 4 5 6 7 00000000\r",
            "(1\t2 3 4 5 6 7 8 00000000", "( 1 2 3 4 5 6 7 00000000"
        };
        for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            CHECK(pm_parse_q1(bad[i], strlen(bad[i]), &s) == PM_BAD_FRAME);
            CHECK(memcmp(&s, &before, sizeof(s)) == 0);
        }
    }
    CHECK(pm_parse_q1(NULL, 99u, &s) == PM_BAD_FRAME);
    CHECK(pm_parse_q1(q1, sizeof(q1) - 1u, NULL) == PM_INVALID_ARGUMENT);
    CHECK(pm_parse_f("#1 2 3 4 5", 10u, &rating) == PM_BAD_FRAME);
    CHECK(pm_parse_f("#1 2 3 655.36", 13u, &rating) == PM_BAD_FRAME);
    CHECK(pm_parse_f(f, sizeof(f) - 1u, NULL) == PM_INVALID_ARGUMENT);
    /* Массив без NUL: ASan проверит отсутствие чтения за его границей. */
    {
        char *exact = malloc(sizeof(q1) - 1u);
        CHECK(exact != NULL);
        memcpy(exact, q1, sizeof(q1) - 1u);
        CHECK(pm_parse_q1(exact, sizeof(q1) - 1u, &s) == PM_OK);
        free(exact);
    }
}

static void test_transactions(void)
{
    fake_io io;
    pm_context ctx;
    size_t i;
    setup(&ctx, &io, 0u, false);
    pm_request_status(&ctx);
    pm_service(&ctx, 19u);
    CHECK(io.tx_count == 0u);
    pm_service(&ctx, 20u);
    CHECK(strcmp(io.tx[0], "Q1\r") == 0 && ctx.state == PM_WAIT_Q1);
    /* Каждый байт приходит отдельно, но состояние обновится только на CR. */
    for (i = 0u; i < sizeof(q1) - 1u; ++i) {
        feed(&io, q1 + i, 1u);
        pm_service(&ctx, 21u + (uint32_t)i);
        CHECK(!ctx.has_status);
    }
    feed(&io, "\r\n", 2u);
    pm_service(&ctx, 80u);
    CHECK(ctx.has_status && io.last_event == PM_EVENT_STATUS && io.last_result == PM_OK);
    CHECK(pm_status_fresh(&ctx, 80u, 30000u));
    CHECK(pm_status_fresh(&ctx, 30079u, 30000u));
    CHECK(!pm_status_fresh(&ctx, 30080u, 30000u));
    CHECK(!pm_status_fresh(&ctx, 80u, 0u));
    pm_request_rating(&ctx);
    pm_service(&ctx, 100u);
    CHECK(strcmp(io.tx[1], "F\r") == 0);
    reply(&ctx, &io, f, 101u);
    CHECK(ctx.has_rating && ctx.last_f_ok_ms == 101u && ctx.last_q1_ok_ms == 80u);
    CHECK(io.last_event == PM_EVENT_RATING);
    /* Повреждённый пакет не портит прошлые числа/время свежести. */
    pm_request_status(&ctx);
    pm_service(&ctx, 121u);
    reply(&ctx, &io, "(1 2", 122u);
    CHECK(io.last_result == PM_BAD_FRAME && ctx.status.input_cV == 23290u);
    CHECK(ctx.last_q1_ok_ms == 80u);
    pm_request_status(&ctx);
    pm_service(&ctx, 142u);
    reply(&ctx, &io, "(NAK", 143u);
    CHECK(io.last_result == PM_NAK && ctx.last_q1_ok_ms == 80u);
    pm_request_status(&ctx);
    pm_service(&ctx, 163u);
    reply(&ctx, &io, f, 164u);
    CHECK(io.last_result == PM_BAD_FRAME); /* F нельзя принять за Q1. */
}

static void test_timeout_recovery(void)
{
    fake_io io;
    pm_context ctx;
    char noise[PM_RX_CAPACITY + 20u];
    setup(&ctx, &io, 0u, false);
    pm_request_status(&ctx);
    pm_service(&ctx, 20u);
    feed(&io, "(230", 4u);
    pm_service(&ctx, 21u);
    pm_service(&ctx, 120u);
    CHECK(ctx.state == PM_RECOVER && io.last_result == PM_TIMEOUT && !ctx.has_status);
    pm_request_status(&ctx);
    /* Хвост старого ответа приходит после таймаута: сливаем и продлеваем
     * карантин, иначе его можно было бы принять за новый ответ. */
    reply(&ctx, &io, q1, 139u);
    pm_service(&ctx, 158u);
    CHECK(io.tx_count == 1u && !ctx.has_status);
    pm_service(&ctx, 159u);
    CHECK(io.tx_count == 2u && ctx.state == PM_WAIT_Q1);
    memset(noise, '7', sizeof(noise));
    feed(&io, noise, sizeof(noise));
    pm_service(&ctx, 160u);
    pm_service(&ctx, 161u);
    CHECK(io.last_result == PM_OVERFLOW && !ctx.has_status);
    reply(&ctx, &io, q1, 162u);
    CHECK(!ctx.has_status); /* Даже правдоподобный хвост не публикуем. */
    pm_request_status(&ctx);
    pm_service(&ctx, 182u);
    reply(&ctx, &io, q1, 183u);
    CHECK(ctx.has_status);
    pm_request_status(&ctx);
    pm_service(&ctx, 203u);
    io.rx_fail = true;
    pm_service(&ctx, 204u);
    CHECK(io.last_result == PM_IO_ERROR && ctx.last_q1_ok_ms == 183u);
    pm_request_status(&ctx);
    io.tx_fail = true;
    pm_service(&ctx, 224u);
    CHECK(io.last_result == PM_IO_ERROR && ctx.state == PM_RECOVER);
    /* Постоянный мусор не блокирует основной цикл: бюджет 64 байта. */
    feed(&io, io.rx, 0u);
    memset(noise, 'X', sizeof(noise));
    feed(&io, noise, sizeof(noise));
    {
        size_t previous = io.rx_tail;
        pm_service(&ctx, 225u);
        CHECK(io.rx_tail - previous == PM_RX_BUDGET);
    }
}

static void test_controls(void)
{
    fake_io io;
    pm_context ctx;
    unsigned int i;
    const char *good[] = { "Q", "T", "TL", "CT", "C", "T01", "T99", "S.2", "S.9", "S01", "S10", "S10R0001", "S.2R9999" };
    const char *bad[] = { "", "Q1", "F", "I", "T00", "T1", "T100", "S.1", "S00", "S11", "S01R0000", "S01R1", "S01R12345", "Q\rS.2", "Q\n", "Q garbage", "12345678901234567890" };
    setup(&ctx, &io, 0u, false);
    ctx.config.allow_control = false;
    CHECK(pm_queue_control(&ctx, "Q", 1u) == PM_DISABLED);
    ctx.config.allow_control = true;
    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(pm_queue_control(&ctx, bad[i], 1u) == PM_INVALID_ARGUMENT);
    for (i = 0u; i < sizeof(good) / sizeof(good[0]); ++i) {
        setup(&ctx, &io, 0u, false);
        CHECK(pm_queue_control(&ctx, good[i], (uint16_t)i) == PM_OK);
    }
    setup(&ctx, &io, 0u, false);
    CHECK(pm_queue_control(&ctx, "Q", 100u) == PM_OK);
    CHECK(pm_queue_control(&ctx, "T", 101u) == PM_OK);
    CHECK(pm_queue_control(&ctx, "CT", 102u) == PM_OK);
    CHECK(pm_queue_control(&ctx, "C", 103u) == PM_OK);
    CHECK(pm_queue_control(&ctx, "Q", 104u) == PM_QUEUE_FULL);
    pm_service(&ctx, 20u);
    CHECK(strcmp(io.tx[0], "Q\r") == 0 && ctx.queue_count == 3u);
    pm_service(&ctx, 120u);
    CHECK(ctx.last_control_id == 100u && ctx.last_control_result == PM_SENT_UNCONFIRMED);
    CHECK(io.last_event == PM_EVENT_CONTROL && io.tx_count == 1u);
    pm_service(&ctx, 140u);
    CHECK(strcmp(io.tx[1], "T\r") == 0);
    reply(&ctx, &io, "T", 141u);
    CHECK(ctx.last_control_id == 101u && ctx.last_control_result == PM_NAK);
    pm_service(&ctx, 161u);
    CHECK(strcmp(io.tx[2], "CT\r") == 0);
    reply(&ctx, &io, "(ACK", 162u);
    CHECK(ctx.last_control_result == PM_UNEXPECTED_REPLY);
    pm_service(&ctx, 182u);
    CHECK(strcmp(io.tx[3], "C\r") == 0);
    feed(&io, "X", 1u);
    pm_service(&ctx, 183u);
    pm_service(&ctx, 282u);
    CHECK(ctx.last_control_id == 103u && ctx.last_control_result == PM_TIMEOUT);
    pm_service(&ctx, 1000u);
    CHECK(io.tx_count == 4u); /* Ни один переключатель не повторился. */
    setup(&ctx, &io, 0u, false);
    io.tx_fail = true;
    CHECK(pm_queue_control(&ctx, "S.2", 44u) == PM_OK);
    pm_service(&ctx, 20u);
    CHECK(ctx.last_control_result == PM_IO_ERROR && ctx.last_control_id == 44u);
    pm_service(&ctx, 40u);
    CHECK(io.tx_count == 1u); /* Частичная передача тоже не повторяется. */
}

static void test_clock_and_schedule(void)
{
    fake_io io;
    pm_context ctx;
    pm_config config;
    uint32_t start_time = UINT32_MAX - 10u;
    setup(&ctx, &io, start_time, false);
    pm_request_status(&ctx);
    pm_service(&ctx, 8u);
    CHECK(io.tx_count == 0u);
    pm_service(&ctx, 9u);
    CHECK(io.tx_count == 1u);
    reply(&ctx, &io, q1, 10u);
    CHECK(pm_status_fresh(&ctx, 11u, 30u));
    CHECK(!pm_status_fresh(&ctx, 40u, 30u));
    ctx.last_q1_ok_ms = UINT32_MAX - 5u;
    CHECK(pm_status_fresh(&ctx, 5u, 12u));
    CHECK(!pm_status_fresh(&ctx, 6u, 12u));
    setup(&ctx, &io, 0u, true);
    ctx.config.q1_interval_ms = 120u;
    ctx.config.f_interval_ms = 120u;
    pm_service(&ctx, 20u);
    CHECK(strcmp(io.tx[0], "Q1\r") == 0);
    pm_service(&ctx, 120u); /* Timeout Q1. */
    pm_service(&ctx, 140u);
    CHECK(strcmp(io.tx[1], "F\r") == 0); /* F не голодает. */
    pm_service(&ctx, 240u);
    pm_service(&ctx, 260u);
    CHECK(strcmp(io.tx[2], "Q1\r") == 0);
    config = pm_default_config(fake_read, fake_write, &io);
    CHECK(pm_init(NULL, &config, 0u) == PM_INVALID_ARGUMENT);
    CHECK(pm_init(&ctx, NULL, 0u) == PM_INVALID_ARGUMENT);
    config.timeout_ms = 0u;
    CHECK(pm_init(&ctx, &config, 0u) == PM_INVALID_ARGUMENT);
    config.timeout_ms = 0x80000000u;
    CHECK(pm_init(&ctx, &config, 0u) == PM_INVALID_ARGUMENT);
    config = pm_default_config(fake_read, fake_write, &io);
    config.read = NULL;
    CHECK(pm_init(&ctx, &config, 0u) == PM_INVALID_ARGUMENT);
    CHECK(!pm_status_fresh(NULL, 0u, 30000u));
    pm_service(NULL, 0u);
    pm_request_status(NULL);
    pm_request_rating(NULL);
    CHECK(pm_queue_control(NULL, "Q", 0u) == PM_INVALID_ARGUMENT);
    /* Повторная инициализация из собственного config не затирает callbacks. */
    CHECK(pm_init(&ctx, &ctx.config, 0u) == PM_OK);
    CHECK(ctx.config.read == fake_read);
}

/* Детерминированные случайные байты под ASan/UBSan. Не заменяет стенд,
 * но проверяет границы парсера на 20 000 входах, включая встроенные NUL. */
static void test_random_input(void)
{
    uint32_t seed = 123456789u;
    unsigned int round;
    for (round = 0u; round < 20000u; ++round) {
        size_t size = round % 128u;
        char *bytes = malloc(size == 0u ? 1u : size);
        pm_status s, old_s;
        pm_rating r, old_r;
        pm_result result;
        size_t i;
        CHECK(bytes != NULL);
        for (i = 0u; i < size; ++i) {
            seed ^= seed << 13u; seed ^= seed >> 17u; seed ^= seed << 5u;
            bytes[i] = (char)(seed & 255u);
        }
        memset(&s, 0x5a, sizeof(s)); old_s = s;
        memset(&r, 0x5a, sizeof(r)); old_r = r;
        result = pm_parse_q1(bytes, size, &s);
        CHECK(result == PM_OK || result == PM_BAD_FRAME);
        if (result != PM_OK) CHECK(memcmp(&s, &old_s, sizeof(s)) == 0);
        result = pm_parse_f(bytes, size, &r);
        CHECK(result == PM_OK || result == PM_BAD_FRAME);
        if (result != PM_OK) CHECK(memcmp(&r, &old_r, sizeof(r)) == 0);
        free(bytes);
    }
}

int main(void)
{
    test_parser();
    test_transactions();
    test_timeout_recovery();
    test_controls();
    test_clock_and_schedule();
    test_random_input();
    printf("PASS: %lu checks; parser, UART, queue, recovery, clock rollover, random input\n", checks);
    return 0;
}
