CC = cc
CPPFLAGS = -Iinclude
CFLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes -pedantic-errors
BUILD = build
AVR_CC = avr-gcc
AVR_MCU = atmega328p
AVR_F_CPU = 16000000UL

.PHONY: all test sanitize avr clean
all: $(BUILD)/libpowermust.a $(BUILD)/host-demo

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/powermust.o: src/powermust.c include/powermust.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/libpowermust.a: $(BUILD)/powermust.o
	$(AR) rcs $@ $<

$(BUILD)/host-demo: examples/host_demo.c src/powermust.c include/powermust.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/powermust.c examples/host_demo.c -o $@

$(BUILD)/test: tests/test_powermust.c src/powermust.c include/powermust.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/powermust.c tests/test_powermust.c -o $@

test: $(BUILD)/test
	./$(BUILD)/test

sanitize: | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer src/powermust.c tests/test_powermust.c -o $(BUILD)/test-sanitize
	./$(BUILD)/test-sanitize

# Это проверочный пример UART, не готовая прошивка автоматики ворот.
avr: | $(BUILD)
	$(AVR_CC) $(CPPFLAGS) $(CFLAGS) -Os -mmcu=$(AVR_MCU) -DF_CPU=$(AVR_F_CPU) -ffunction-sections -fdata-sections src/powermust.c examples/atmega328p/main.c -Wl,--gc-sections -o $(BUILD)/atmega328p.elf
	avr-size -C --mcu=$(AVR_MCU) $(BUILD)/atmega328p.elf
	avr-objcopy -O ihex -R .eeprom $(BUILD)/atmega328p.elf $(BUILD)/atmega328p.hex

clean:
	rm -rf $(BUILD)
