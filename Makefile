CROSS_COMPILE ?= riscv64-unknown-elf-
CC := $(CROSS_COMPILE)gcc
OBJDUMP := $(CROSS_COMPILE)objdump
QEMU ?= qemu-system-riscv64

BUILD_DIR := build
TARGET := $(BUILD_DIR)/memcpy_demo.elf

CFLAGS := -march=rv64imac -mabi=lp64 -mcmodel=medany \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-pic \
	-Wall -Wextra -Werror -O2 -g
LDFLAGS := -nostdlib -nostartfiles -Wl,-T,linker.ld -Wl,--gc-sections
QEMU_FLAGS := -machine virt -nographic -bios none -kernel $(TARGET)

OBJECTS := \
	$(BUILD_DIR)/start.o \
	$(BUILD_DIR)/memcpy.o \
	$(BUILD_DIR)/test_memcpy.o

.PHONY: all run disasm clean toolcheck

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/start.o: start.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memcpy.o: memcpy.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_memcpy.o: test_memcpy.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): linker.ld $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o $@

run: $(TARGET)
	$(QEMU) $(QEMU_FLAGS); status=$$?; \
	if [ $$status -eq 0 ]; then \
		echo 'PASS: assembly memcpy passed all checks'; \
	else \
		echo "FAIL: QEMU exited with status $$status"; exit $$status; \
	fi

disasm: $(TARGET)
	$(OBJDUMP) -d -S $(TARGET) | less

toolcheck:
	@command -v $(CC) >/dev/null || { echo "Missing compiler: $(CC)"; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo "Missing emulator: $(QEMU)"; exit 1; }
	@echo "Compiler: $$($(CC) --version | head -n 1)"
	@echo "QEMU:     $$($(QEMU) --version | head -n 1)"

clean:
	rm -rf $(BUILD_DIR)

