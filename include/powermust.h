/* SPDX-License-Identifier: Apache-2.0
 * C99-реализация и комментарии: ChatGPT (OpenAI), по заданию Сергея Богданова.
 * C99-перенос протокола syssi/esphome-powermust, существенно переработан.
 * Подробности происхождения и отличий: NOTICE и docs/ANALYSIS.md.
 */
#ifndef POWERMUST_H
#define POWERMUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* На AVR RAM расходуется только на этот контекст и буферы приложения.
 * Ни malloc, ни float, ни stdio, ни Arduino/ESPHome ядру не нужны. */
#define PM_RX_CAPACITY 96u
#define PM_COMMAND_CAPACITY 9u /* S10R9999 + завершающий NUL. */
#define PM_QUEUE_CAPACITY 4u
#define PM_RX_BUDGET 64u       /* Ограничение работы за один вызов service. */

/* Порядок совпадает с Q1: слева b7, справа b0. */
#define PM_FLAG_UTILITY_FAIL  0x80u
#define PM_FLAG_BATTERY_LOW   0x40u
#define PM_FLAG_BYPASS_AVR    0x20u
#define PM_FLAG_UPS_FAILED    0x10u
#define PM_FLAG_STANDBY      0x08u
#define PM_FLAG_TEST         0x04u
#define PM_FLAG_SHUTDOWN     0x02u
#define PM_FLAG_BEEPER       0x01u

/* valid_fields: бит N означает доступность поля N ответа.
 * @@@ или @.@ от ИБП означают «неизвестно», а не нулевое значение.
 * Перед принятием решения проверяйте и свежесть Q1, и нужный бит. */
#define PM_Q1_INPUT_VALID       0x01u
#define PM_Q1_FAULT_VALID       0x02u
#define PM_Q1_OUTPUT_VALID      0x04u
#define PM_Q1_LOAD_VALID        0x08u
#define PM_Q1_FREQUENCY_VALID   0x10u
#define PM_Q1_BATTERY_VALID     0x20u
#define PM_Q1_TEMPERATURE_VALID 0x40u
#define PM_Q1_FLAGS_VALID       0x80u

typedef struct {
    uint16_t input_cV;         /* Сотые доли В: 23290 = 232.90 В. */
    uint16_t fault_cV;
    uint16_t output_cV;
    uint16_t load_percent;     /* Именно %, НЕ ток в А и НЕ заряд АКБ. */
    uint16_t frequency_cHz;    /* Сотые доли Гц. */
    uint16_t battery_cV;       /* В Q1 может быть напряжение ячейки! */
    int16_t temperature_cC;    /* Сотые доли °C, допускается минус. */
    uint8_t flags;
    uint8_t valid_fields;
} pm_status;

typedef struct {
    uint16_t voltage_cV;
    uint16_t current_cA;       /* Номинальный ток из F; 003 = 3.00 А. */
    uint16_t battery_cV;
    uint16_t frequency_cHz;
    uint8_t valid_fields;     /* Биты 0..3 соответствуют полям F. */
} pm_rating;

typedef enum {
    PM_OK = 0,
    PM_INVALID_ARGUMENT,
    PM_DISABLED,
    PM_QUEUE_FULL,
    PM_BAD_FRAME,
    PM_NAK,
    PM_TIMEOUT,
    PM_OVERFLOW,
    PM_IO_ERROR,
    /* Управляющие команды Megatec часто не имеют положительного ответа.
     * Тишина НЕ доказывает исполнение: сюда попадает и отключённый кабель. */
    PM_SENT_UNCONFIRMED,
    PM_UNEXPECTED_REPLY
} pm_result;

typedef enum { PM_EVENT_STATUS, PM_EVENT_RATING, PM_EVENT_CONTROL,
               PM_EVENT_ERROR } pm_event;
typedef enum { PM_RECOVER, PM_IDLE, PM_WAIT_Q1, PM_WAIT_F,
               PM_WAIT_CONTROL } pm_state;

/* Все callback вызываются из pm_service(), НЕ из ISR.
 * read: 1 = получен байт; 0 = данных нет; -1 = ошибка UART/переполнение
 *       внешнего кольцевого буфера. Вызов никогда не ждёт следующий байт.
 * write: true = ВСЕ байты атомарно приняты TX-очередью/драйвером;
 *        false = ошибка. Даже при частичной физической передаче ядро
 *        не повторяет управляющую команду автоматически.
 * event: уведомление; разрешено читать ctx, нельзя повторно вызывать
 *        pm_service или изменять ctx из callback.
 * Один контекст обслуживает только один потребитель из основного цикла. */
struct pm_context;
typedef int (*pm_read_fn)(void *user, uint8_t *byte);
typedef bool (*pm_write_fn)(void *user, const uint8_t *data, size_t size);
typedef void (*pm_event_fn)(void *user, const struct pm_context *ctx,
                            pm_event event, pm_result result);

typedef struct {
    pm_read_fn read;
    pm_write_fn write;
    pm_event_fn event;         /* NULL допустим. */
    void *user;
    uint32_t q1_interval_ms;   /* 0 = только явные запросы. */
    uint32_t f_interval_ms;
    uint32_t timeout_ms;
    uint32_t recovery_quiet_ms;/* Минимальная тишина после ошибки/старта. */
    bool allow_control;       /* По умолчанию false: только мониторинг. */
} pm_config;

typedef struct {
    char text[PM_COMMAND_CAPACITY];
    uint16_t id;               /* Идентификатор запроса приложения/Modbus. */
} pm_command;

typedef struct pm_context {
    pm_config config;
    pm_status status;
    pm_rating rating;
    pm_command queue[PM_QUEUE_CAPACITY];
    pm_command active_command;
    uint32_t started_ms, quiet_since_ms;
    uint32_t last_q1_poll_ms, last_f_poll_ms;
    uint32_t last_q1_ok_ms, last_f_ok_ms;
    uint16_t last_control_id;
    pm_result last_control_result;
    pm_state state;
    uint8_t queue_head, queue_count, requested;
    uint8_t rx_size;
    bool has_status, has_rating, last_poll_was_q1;
    char rx[PM_RX_CAPACITY];
} pm_context;

/* Значения по умолчанию: Q1 2 с, F 60 с, timeout 1 с, recovery 1 с.
 * Параметры времён должны быть < 2^31 мс, timeout/recovery > 0.
 * Частый вызов service (например каждые 1..10 мс) обязателен. */
pm_config pm_default_config(pm_read_fn read, pm_write_fn write, void *user);
pm_result pm_init(pm_context *ctx, const pm_config *config, uint32_t now_ms);
void pm_service(pm_context *ctx, uint32_t now_ms);
void pm_request_status(pm_context *ctx);
void pm_request_rating(pm_context *ctx);

/* Разрешён только набор команд исходника/Megatec:
 * Q, T, TL, T01..T99, CT, C, S.2..S.9, S01..S10,
 * а также S<n>R0001..R9999. CR добавляет ядро, в text его быть не должно.
 * «Q» переключает пищалку, не задаёт ей состояние ON/OFF.
 * Это транспорт команд, НЕ аварийная логика ворот. Повторов нет.
 * PM_OK означает постановку в очередь, НЕ исполнение команды ИБП. */
pm_result pm_queue_control(pm_context *ctx, const char *text, uint16_t id);

/* Парсеры принимают ровно одну строку БЕЗ CR/LF и с явной длиной.
 * NUL не обязателен; за пределы [data, data+size) они не читают.
 * При ошибке структура out остаётся без изменений. */
pm_result pm_parse_q1(const char *data, size_t size, pm_status *out);
pm_result pm_parse_f(const char *data, size_t size, pm_rating *out);

/* Свежесть считается только от успешно проверенного Q1, не от F,
 * отправки команды или любых байтов на линии. Старые данные сохраняются
 * для диагностики, но приложение обязано проверить эту функцию. */
bool pm_status_fresh(const pm_context *ctx, uint32_t now_ms,
                     uint32_t max_age_ms);

#endif
