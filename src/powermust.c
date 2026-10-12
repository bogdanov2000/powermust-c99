/* SPDX-License-Identifier: Apache-2.0
 * Существенно изменённый перенос syssi/esphome-powermust на C99.
 * ESPHome/STL/динамическая память исключены. См. NOTICE.
 */
#include "powermust.h"
#include <string.h>

#define REQUEST_Q1 1u
#define REQUEST_F  2u
#define TIME_LIMIT UINT32_C(0x80000000)

/* unsigned-вычитание корректно переживает переполнение millis().
 * Предполагается, что service вызывают регулярно, не раз в 49 суток. */
static bool elapsed(uint32_t now, uint32_t then, uint32_t duration)
{
    return (uint32_t)(now - then) >= duration;
}

typedef struct { const char *data; size_t size; } token;

/* Пробелы разрешены между полями, но не в начале/конце ответа.
 * Никаких strtof/sscanf: они тяжеловесны на AVR и плохо контролируют
 * усечённые строки, locale, NaN/Inf и выход за диапазон. */
static bool tokenize(const char *data, size_t size, char prefix,
                     token *fields, size_t count)
{
    size_t pos = 1u, i;
    if (data == NULL || size < 2u || size >= PM_RX_CAPACITY ||
        data[0] != prefix) return false;
    for (i = 0u; i < count; ++i) {
        size_t start = pos;
        while (pos < size && data[pos] != ' ') ++pos;
        if (pos == start) return false;
        fields[i].data = data + start;
        fields[i].size = pos - start;
        if (i + 1u < count) {
            if (pos == size) return false;
            while (pos < size && data[pos] == ' ') ++pos;
        }
    }
    return pos == size; /* Лишний токен или пробел запрещён. */
}

/* Megatec заменяет недоступные позиции знаком @. Принимаем только
 * полностью неизвестное поле: @, @@@, @@.@, @.@@ и аналогичные.
 * Смесь цифр и @ отвергается: её нельзя считать числом. */
static bool unavailable(token t)
{
    size_t i;
    bool seen = false;
    for (i = 0u; i < t.size; ++i) {
        if (t.data[i] == '@') seen = true;
        else if (t.data[i] != '.') return false;
    }
    return seen;
}

/* scale=100: точные сотые без float; scale=1: целое поле нагрузки.
 * Переполнение ловится ДО умножения. Разрешены 0..2 знака после точки,
 * только ASCII-цифры, без экспоненты и знака «+».
 * temperature может быть отрицательной; прочие поля — нет. */
static bool number(token t, uint16_t scale, bool signed_value,
                   int32_t *out, bool *known)
{
    size_t i = 0u;
    uint32_t whole = 0u, fraction = 0u, decimals = 0u, value;
    bool negative = false;
    uint32_t limit = signed_value ? 32767u : 65535u;
    if (unavailable(t)) { *known = false; *out = 0; return true; }
    *known = true;
    if (signed_value && t.data[0] == '-') { negative = true; ++i; }
    if (i == t.size || t.data[i] < '0' || t.data[i] > '9') return false;
    while (i < t.size && t.data[i] >= '0' && t.data[i] <= '9') {
        uint32_t digit = (uint32_t)(t.data[i++] - '0');
        if (whole > (limit - digit) / 10u) return false;
        whole = whole * 10u + digit;
    }
    if (i < t.size && t.data[i] == '.' && scale == 100u) {
        ++i;
        while (i < t.size && t.data[i] >= '0' && t.data[i] <= '9') {
            if (++decimals > 2u) return false;
            fraction = fraction * 10u + (uint32_t)(t.data[i++] - '0');
        }
        if (decimals == 0u) return false;
        if (decimals == 1u) fraction *= 10u;
    }
    if (i != t.size || whole > limit / scale) return false;
    value = whole * scale + fraction;
    if (value > limit) return false;
    *out = negative ? -(int32_t)value : (int32_t)value;
    return true;
}

pm_result pm_parse_q1(const char *data, size_t size, pm_status *out)
{
    token t[8];
    pm_status result;
    int32_t values[7];
    size_t i;
    if (out == NULL) return PM_INVALID_ARGUMENT;
    if (!tokenize(data, size, '(', t, 8u)) return PM_BAD_FRAME;
    memset(&result, 0, sizeof(result));
    for (i = 0u; i < 7u; ++i) {
        bool known;
        if (!number(t[i], i == 3u ? 1u : 100u, i == 6u,
                    &values[i], &known)) return PM_BAD_FRAME;
        /* 999% допускается форматом (перегрузка/особенности модели),
         * но не безграничное целое. */
        if (i == 3u && values[i] > 999) return PM_BAD_FRAME;
        if (known) result.valid_fields |= (uint8_t)(1u << i);
    }
    if (!unavailable(t[7])) {
        if (t[7].size != 8u) return PM_BAD_FRAME;
        for (i = 0u; i < 8u; ++i) {
            char bit = t[7].data[i];
            if (bit != '0' && bit != '1') return PM_BAD_FRAME;
            result.flags = (uint8_t)((result.flags << 1u) | (uint8_t)(bit - '0'));
        }
        result.valid_fields |= PM_Q1_FLAGS_VALID;
    }
    result.input_cV = (uint16_t)values[0];
    result.fault_cV = (uint16_t)values[1];
    result.output_cV = (uint16_t)values[2];
    result.load_percent = (uint16_t)values[3];
    result.frequency_cHz = (uint16_t)values[4];
    result.battery_cV = (uint16_t)values[5];
    result.temperature_cC = (int16_t)values[6];
    *out = result; /* Публикуем целиком, только после проверки всех полей. */
    return PM_OK;
}

pm_result pm_parse_f(const char *data, size_t size, pm_rating *out)
{
    token t[4];
    pm_rating result;
    int32_t values[4];
    size_t i;
    if (out == NULL) return PM_INVALID_ARGUMENT;
    if (!tokenize(data, size, '#', t, 4u)) return PM_BAD_FRAME;
    memset(&result, 0, sizeof(result));
    for (i = 0u; i < 4u; ++i) {
        bool known;
        if (!number(t[i], 100u, false, &values[i], &known)) return PM_BAD_FRAME;
        if (known) result.valid_fields |= (uint8_t)(1u << i);
    }
    result.voltage_cV = (uint16_t)values[0];
    result.current_cA = (uint16_t)values[1];
    result.battery_cV = (uint16_t)values[2];
    result.frequency_cHz = (uint16_t)values[3];
    *out = result;
    return PM_OK;
}

pm_config pm_default_config(pm_read_fn read, pm_write_fn write, void *user)
{
    pm_config config;
    memset(&config, 0, sizeof(config));
    config.read = read;
    config.write = write;
    config.user = user;
    config.q1_interval_ms = 2000u;
    config.f_interval_ms = 60000u;
    config.timeout_ms = 1000u;
    config.recovery_quiet_ms = 1000u;
    return config;
}

pm_result pm_init(pm_context *ctx, const pm_config *config, uint32_t now)
{
    /* Копия позволяет передать &ctx->config при повторной инициализации.
     * При невалидных аргументах уже работающий контекст не меняется. */
    pm_config copy;
    if (ctx == NULL || config == NULL || config->read == NULL ||
        config->write == NULL || config->timeout_ms == 0u ||
        config->recovery_quiet_ms == 0u || config->timeout_ms >= TIME_LIMIT ||
        config->recovery_quiet_ms >= TIME_LIMIT ||
        config->q1_interval_ms >= TIME_LIMIT ||
        config->f_interval_ms >= TIME_LIMIT) return PM_INVALID_ARGUMENT;
    copy = *config;
    memset(ctx, 0, sizeof(*ctx));
    ctx->config = copy;
    ctx->state = PM_RECOVER;
    ctx->quiet_since_ms = now;
    ctx->last_q1_poll_ms = ctx->last_f_poll_ms = now;
    /* Первый опрос сразу после очистки старого UART-трафика. */
    if (copy.q1_interval_ms != 0u) ctx->requested |= REQUEST_Q1;
    if (copy.f_interval_ms != 0u) ctx->requested |= REQUEST_F;
    return PM_OK;
}

void pm_request_status(pm_context *ctx)
{
    if (ctx != NULL) ctx->requested |= REQUEST_Q1;
}
void pm_request_rating(pm_context *ctx)
{
    if (ctx != NULL) ctx->requested |= REQUEST_F;
}

static bool digits(const char *p, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i)
        if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

/* Ограничение грамматики защищает от CR-инъекций и случайных байтов.
 * Допустимая грамматика ещё не доказывает поддержку команды ELTENA. */
static bool control_valid(const char *text, size_t size)
{
    if (size == 1u) return text[0] == 'Q' || text[0] == 'T' || text[0] == 'C';
    if (size == 2u) return memcmp(text, "TL", 2u) == 0 || memcmp(text, "CT", 2u) == 0;
    if (size == 3u && text[0] == 'T')
        return digits(text + 1, 2u) && (text[1] != '0' || text[2] != '0');
    if ((size == 3u || size == 8u) && text[0] == 'S') {
        bool delay = (text[1] == '.' && text[2] >= '2' && text[2] <= '9') ||
                     (text[1] == '0' && text[2] >= '1' && text[2] <= '9') ||
                     (text[1] == '1' && text[2] == '0');
        if (!delay) return false;
        if (size == 3u) return true;
        return text[3] == 'R' && digits(text + 4, 4u) &&
               memcmp(text + 4, "0000", 4u) != 0;
    }
    return false;
}

pm_result pm_queue_control(pm_context *ctx, const char *text, uint16_t id)
{
    size_t size = 0u;
    uint8_t tail;
    if (ctx == NULL || text == NULL) return PM_INVALID_ARGUMENT;
    if (!ctx->config.allow_control) return PM_DISABLED;
    /* Не strlen: даже слишком длинную входную строку сканируем ограниченно.
     * Как обычно в C API, text должен указывать на доступную C-строку. */
    while (size < PM_COMMAND_CAPACITY && text[size] != '\0') ++size;
    if (size == PM_COMMAND_CAPACITY || !control_valid(text, size))
        return PM_INVALID_ARGUMENT;
    if (ctx->queue_count == PM_QUEUE_CAPACITY) return PM_QUEUE_FULL;
    tail = (uint8_t)((ctx->queue_head + ctx->queue_count) % PM_QUEUE_CAPACITY);
    memcpy(ctx->queue[tail].text, text, size + 1u);
    ctx->queue[tail].id = id;
    ++ctx->queue_count;
    return PM_OK;
}

static void notify(pm_context *ctx, pm_event event, pm_result result)
{
    if (ctx->config.event != NULL)
        ctx->config.event(ctx->config.user, ctx, event, result);
}

static void recover(pm_context *ctx, uint32_t now)
{
    ctx->state = PM_RECOVER;
    ctx->rx_size = 0u;
    ctx->quiet_since_ms = now;
}

static void complete(pm_context *ctx, pm_result result, uint32_t now)
{
    bool control = ctx->state == PM_WAIT_CONTROL;
    if (control) {
        ctx->last_control_id = ctx->active_command.id;
        ctx->last_control_result = result;
    }
    /* Любой исход заканчивает транзакцию. Никаких автоматических повторов
     * Q/T/S: повтор переключателя или отключения может изменить смысл. */
    recover(ctx, now);
    if (control) notify(ctx, PM_EVENT_CONTROL, result);
    else if (result != PM_OK) notify(ctx, PM_EVENT_ERROR, result);
}

static bool nak(const char *data, size_t size)
{
    return (size == 4u && memcmp(data, "(NAK", 4u) == 0) ||
           (size == 5u && memcmp(data, "(NAK)", 5u) == 0) ||
           (size == 3u && memcmp(data, "NAK", 3u) == 0);
}

static void decode(pm_context *ctx, uint32_t now)
{
    pm_result result;
    pm_state waiting = ctx->state;
    if (nak(ctx->rx, ctx->rx_size)) { complete(ctx, PM_NAK, now); return; }
    if (waiting == PM_WAIT_CONTROL) {
        /* По Megatec эхо неизвестной команды означает отказ.
         * Прочие ответы (включая ACK без проверенной семантики модели)
         * не считаем доказательством физического исполнения. */
        size_t size = strlen(ctx->active_command.text);
        result = size == ctx->rx_size &&
                 memcmp(ctx->rx, ctx->active_command.text, size) == 0 ?
                 PM_NAK : PM_UNEXPECTED_REPLY;
        complete(ctx, result, now);
        return;
    }
    if (waiting == PM_WAIT_Q1) {
        result = pm_parse_q1(ctx->rx, ctx->rx_size, &ctx->status);
        if (result == PM_OK) { ctx->has_status = true; ctx->last_q1_ok_ms = now; }
    } else {
        result = pm_parse_f(ctx->rx, ctx->rx_size, &ctx->rating);
        if (result == PM_OK) { ctx->has_rating = true; ctx->last_f_ok_ms = now; }
    }
    complete(ctx, result, now);
    if (result == PM_OK)
        notify(ctx, waiting == PM_WAIT_Q1 ? PM_EVENT_STATUS : PM_EVENT_RATING, result);
}

static void start(pm_context *ctx, pm_state state, const char *text, uint32_t now)
{
    uint8_t frame[PM_COMMAND_CAPACITY];
    size_t size = strlen(text); /* Здесь только проверенные строки. */
    memcpy(frame, text, size);
    frame[size++] = '\r';
    ctx->state = state;
    ctx->started_ms = now;
    ctx->rx_size = 0u;
    if (!ctx->config.write(ctx->config.user, frame, size))
        complete(ctx, PM_IO_ERROR, now);
}

void pm_service(pm_context *ctx, uint32_t now)
{
    uint8_t byte;
    unsigned int count;
    int received;
    if (ctx == NULL) return;
    if (ctx->state == PM_WAIT_Q1 || ctx->state == PM_WAIT_F ||
        ctx->state == PM_WAIT_CONTROL) {
        if (elapsed(now, ctx->started_ms, ctx->config.timeout_ms)) {
            /* Частично полученная команда не превращается в «успех». */
            pm_result result = ctx->state == PM_WAIT_CONTROL && ctx->rx_size == 0u ?
                               PM_SENT_UNCONFIRMED : PM_TIMEOUT;
            complete(ctx, result, now);
        }
    }
    for (count = 0u; count < PM_RX_BUDGET; ++count) {
        received = ctx->config.read(ctx->config.user, &byte);
        if (received == 0) break;
        if (received < 0) {
            if (ctx->state == PM_RECOVER || ctx->state == PM_IDLE) {
                recover(ctx, now);
                notify(ctx, PM_EVENT_ERROR, PM_IO_ERROR);
            } else complete(ctx, PM_IO_ERROR, now);
            return;
        }
        if (ctx->state == PM_IDLE || ctx->state == PM_RECOVER) {
            /* Незапрошенные/запоздалые данные нельзя выдать за ответ
             * на следующий запрос. Сливаем и начинаем отсчёт тишины. */
            recover(ctx, now);
            continue;
        }
        if (byte == '\n' && ctx->rx_size == 0u) continue;
        if (byte == '\r') { decode(ctx, now); continue; }
        if (ctx->rx_size >= PM_RX_CAPACITY - 1u) {
            /* Оставляем место для NUL; хвост сливается в RECOVER.
             * В отличие от исходника не начинаем разбирать хвост заново. */
            complete(ctx, PM_OVERFLOW, now);
            continue;
        }
        ctx->rx[ctx->rx_size++] = (char)byte;
        ctx->rx[ctx->rx_size] = '\0'; /* NUL лишь для отладки, parser использует size. */
    }
    /* Если бюджет исчерпан, во внешнем UART ещё может быть хвост.
     * Не отправляем новый запрос, пока всё старое не вычитано. */
    if (count == PM_RX_BUDGET) return;
    if (ctx->state == PM_RECOVER) {
        if (!elapsed(now, ctx->quiet_since_ms, ctx->config.recovery_quiet_ms)) return;
        ctx->state = PM_IDLE;
    }
    if (ctx->state != PM_IDLE) return;
    if (ctx->config.q1_interval_ms != 0u &&
        elapsed(now, ctx->last_q1_poll_ms, ctx->config.q1_interval_ms))
        ctx->requested |= REQUEST_Q1;
    if (ctx->config.f_interval_ms != 0u &&
        elapsed(now, ctx->last_f_poll_ms, ctx->config.f_interval_ms))
        ctx->requested |= REQUEST_F;
    if (ctx->queue_count != 0u) {
        ctx->active_command = ctx->queue[ctx->queue_head];
        ctx->queue_head = (uint8_t)((ctx->queue_head + 1u) % PM_QUEUE_CAPACITY);
        --ctx->queue_count;
        start(ctx, PM_WAIT_CONTROL, ctx->active_command.text, now);
    } else if ((ctx->requested & REQUEST_F) != 0u &&
               (((ctx->requested & REQUEST_Q1) == 0u) || ctx->last_poll_was_q1)) {
        /* Когда оба опроса назрели, чередуем их: медленный/молчащий
         * Q1 не должен навсегда вытеснить F из расписания. */
        ctx->requested &= (uint8_t)~REQUEST_F;
        ctx->last_f_poll_ms = now;
        ctx->last_poll_was_q1 = false;
        start(ctx, PM_WAIT_F, "F", now);
    } else if ((ctx->requested & REQUEST_Q1) != 0u) {
        ctx->requested &= (uint8_t)~REQUEST_Q1;
        ctx->last_q1_poll_ms = now;
        ctx->last_poll_was_q1 = true;
        start(ctx, PM_WAIT_Q1, "Q1", now);
    }
}

bool pm_status_fresh(const pm_context *ctx, uint32_t now, uint32_t max_age)
{
    return ctx != NULL && ctx->has_status && max_age != 0u && max_age < TIME_LIMIT &&
           (uint32_t)(now - ctx->last_q1_ok_ms) < max_age;
}
