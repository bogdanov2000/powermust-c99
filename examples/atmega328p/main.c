/* SPDX-License-Identifier: Apache-2.0
 * ATmega328P, 16 МГц, UART0 2400 8N1, Timer0 — миллисекунды.
 * Это самостоятельный avr-libc C99 пример, без Arduino runtime.
 * Он только опрашивает ИБП. Реле и автоматика ворот здесь отсутствуют.
 * UART0 занят ИБП; USB Serial логирование на том же UART запрещено.
 * RS232 подключают через подходящий преобразователь уровней.
 */
#include "powermust.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>

#if F_CPU != 16000000UL
#error "Recalculate Timer0 and UART for a clock other than 16 MHz"
#endif

#define RX_SIZE 64u
#define TX_SIZE 16u
static volatile uint8_t rx_buffer[RX_SIZE], rx_head, rx_tail;
static volatile uint8_t tx_buffer[TX_SIZE], tx_head, tx_tail;
static volatile bool rx_fault;
static volatile uint32_t ticks;
static pm_context ups;

ISR(TIMER0_COMPA_vect) { ++ticks; }

ISR(USART_RX_vect)
{
    uint8_t status = UCSR0A; /* Статус читают ДО UDR0. */
    uint8_t byte = UDR0;
    uint8_t next = (uint8_t)((rx_head + 1u) % RX_SIZE);
    if ((status & (_BV(FE0) | _BV(DOR0) | _BV(UPE0))) != 0u || next == rx_tail)
        rx_fault = true;
    else { rx_buffer[rx_head] = byte; rx_head = next; }
}

ISR(USART_UDRE_vect)
{
    if (tx_tail == tx_head) UCSR0B &= (uint8_t)~_BV(UDRIE0);
    else { UDR0 = tx_buffer[tx_tail]; tx_tail = (uint8_t)((tx_tail + 1u) % TX_SIZE); }
}

static int uart_read(void *user, uint8_t *byte)
{
    int result = 0;
    (void)user;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (rx_fault) {
            rx_fault = false;
            rx_tail = rx_head; /* Нельзя разбирать строку с потерянными байтами. */
            result = -1;
        } else if (rx_tail != rx_head) {
            *byte = rx_buffer[rx_tail];
            rx_tail = (uint8_t)((rx_tail + 1u) % RX_SIZE);
            result = 1;
        }
    }
    return result;
}

static bool uart_write(void *user, const uint8_t *data, size_t size)
{
    bool accepted = false;
    (void)user;
    /* Не ждём опустошения UART: копируем короткий кадр в TX кольцо.
     * Проверка места и запись атомарны относительно UDRE ISR. */
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        uint8_t free_slots = (uint8_t)((tx_tail + TX_SIZE - tx_head - 1u) % TX_SIZE);
        if (size <= free_slots) {
            size_t i;
            for (i = 0u; i < size; ++i) {
                tx_buffer[tx_head] = data[i];
                tx_head = (uint8_t)((tx_head + 1u) % TX_SIZE);
            }
            UCSR0B |= _BV(UDRIE0);
            accepted = true;
        }
    }
    return accepted;
}

static uint32_t milliseconds(void)
{
    uint32_t value;
    /* AVR 8-битный: чтение uint32_t без atomic может быть разорвано ISR. */
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { value = ticks; }
    return value;
}

int main(void)
{
    pm_config config = pm_default_config(uart_read, uart_write, NULL);
    uint16_t divisor = (uint16_t)(F_CPU / (16UL * 2400UL) - 1UL);
    UBRR0H = (uint8_t)(divisor >> 8u);
    UBRR0L = (uint8_t)divisor;
    UCSR0A = 0u;
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00); /* 8 бит, 1 stop, no parity. */
    UCSR0B = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
    TCCR0A = _BV(WGM01);
    TCCR0B = _BV(CS01) | _BV(CS00); /* 16 МГц / 64 / 250 = 1000 Гц. */
    OCR0A = 249u;
    TIMSK0 = _BV(OCIE0A);
    if (pm_init(&ups, &config, 0u) != PM_OK) for (;;) {}
    sei();
    for (;;) {
        uint32_t now = milliseconds();
        pm_service(&ups, now);
        /* Здесь основной код коробочки: Modbus, входы, реле, watchdog.
         * Он должен отдавать управление быстро, без delay/ожидания UART.
         * Для решений о сети используйте свежесть И valid_fields:
         * bool usable = pm_status_fresh(&ups, now, 30000u) &&
         *     (ups.status.valid_fields & PM_Q1_FLAGS_VALID) != 0u;
         * Процент заряда этот протокол не предоставляет. */
    }
}
