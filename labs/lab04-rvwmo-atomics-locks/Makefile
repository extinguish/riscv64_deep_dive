CROSS_COMPILE ?= riscv64-unknown-elf-
CC := $(CROSS_COMPILE)gcc
NM := $(shell command -v $(CROSS_COMPILE)nm 2>/dev/null || command -v nm)
OBJDUMP := $(shell command -v $(CROSS_COMPILE)objdump 2>/dev/null || command -v objdump)
READELF := $(shell command -v $(CROSS_COMPILE)readelf 2>/dev/null || command -v readelf)
QEMU ?= qemu-system-riscv64

BUILD_DIR := build
FULL_ELF := $(BUILD_DIR)/lab04.elf
SMOKE_ELF := $(BUILD_DIR)/lab04-smoke.elf
UNIT_ELF := $(BUILD_DIR)/lab04-unit.elf

ARCH_FLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
COMMON_FLAGS := $(ARCH_FLAGS) -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -msmall-data-limit=0 -Wall -Wextra -Werror -O2 -g
ASFLAGS := $(COMMON_FLAGS)
CFLAGS := $(COMMON_FLAGS) -std=c11
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
	-Wl,-T,linker.ld -Wl,--gc-sections -Wl,--build-id=none
QEMU_FLAGS := -machine virt -cpu rv64 -m 128M -display none -monitor none \
	-serial stdio -no-reboot -bios none
QEMU_TIMEOUT ?= 15s

COMMON_OBJECTS := \
	$(BUILD_DIR)/infra/reset.o \
	$(BUILD_DIR)/infra/early_console.o \
	$(BUILD_DIR)/infra/boot_state.o

RUNTIME_OBJECTS := \
	$(BUILD_DIR)/infra/platform.o \
	$(BUILD_DIR)/infra/memory.o \
	$(BUILD_DIR)/infra/unit_tests.o

EXERCISE_OBJECTS := \
	$(BUILD_DIR)/exercises/atomic.o \
	$(BUILD_DIR)/exercises/spinlock.o \
	$(BUILD_DIR)/exercises/ticketlock.o \
	$(BUILD_DIR)/exercises/message.o \
	$(BUILD_DIR)/exercises/bitops.o

FULL_OBJECTS := $(COMMON_OBJECTS) $(RUNTIME_OBJECTS) \
	$(BUILD_DIR)/infra/full_entry.o \
	$(BUILD_DIR)/infra/stress.o \
	$(EXERCISE_OBJECTS)

UNIT_OBJECTS := $(COMMON_OBJECTS) $(RUNTIME_OBJECTS) \
	$(BUILD_DIR)/infra/unit_entry.o \
	$(BUILD_DIR)/infra/unit_main.o \
	$(EXERCISE_OBJECTS)

.PHONY: all build smoke unit test inspect check-elf toolcheck clean

all: build
build: $(FULL_ELF)

$(BUILD_DIR)/infra/%.o: infra/%.S include/lab_layout.h | $(BUILD_DIR)/infra
	$(CC) $(ASFLAGS) -Iinclude -c $< -o $@

$(BUILD_DIR)/infra/%.o: infra/%.c include/lab_api.h include/lab_layout.h | $(BUILD_DIR)/infra
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

$(BUILD_DIR)/exercises/%.o: exercises/%.S include/lab_layout.h | $(BUILD_DIR)/exercises
	$(CC) $(ASFLAGS) -Iinclude -c $< -o $@

$(FULL_ELF): linker.ld $(FULL_OBJECTS)
	$(CC) $(LDFLAGS) $(FULL_OBJECTS) -o $@

$(SMOKE_ELF): linker.ld $(COMMON_OBJECTS) $(BUILD_DIR)/infra/smoke_entry.o
	$(CC) $(LDFLAGS) $(COMMON_OBJECTS) $(BUILD_DIR)/infra/smoke_entry.o -o $@

$(UNIT_ELF): linker.ld $(UNIT_OBJECTS)
	$(CC) $(LDFLAGS) $(UNIT_OBJECTS) -o $@

$(BUILD_DIR)/infra $(BUILD_DIR)/exercises:
	mkdir -p $@

smoke: $(SMOKE_ELF)
	@./scripts/run-qemu.sh "$(QEMU_TIMEOUT)" "$(QEMU)" $(QEMU_FLAGS) -smp 4 -kernel $(SMOKE_ELF)

unit: $(UNIT_ELF)
	@./scripts/run-qemu.sh "$(QEMU_TIMEOUT)" "$(QEMU)" $(QEMU_FLAGS) -smp 1 -kernel $(UNIT_ELF)

test: check-elf
	@./scripts/run-qemu.sh "$(QEMU_TIMEOUT)" "$(QEMU)" $(QEMU_FLAGS) -smp 4 -kernel $(FULL_ELF)

check-elf: $(FULL_ELF)
	READELF=$(READELF) NM=$(NM) ./scripts/check-elf.sh $(FULL_ELF)

inspect: check-elf
	$(OBJDUMP) -d -S -M no-aliases,numeric $(FULL_ELF) > $(BUILD_DIR)/lab04.disasm.txt
	$(READELF) -SW $(FULL_ELF) > $(BUILD_DIR)/lab04.sections.txt
	$(READELF) -lW $(FULL_ELF) > $(BUILD_DIR)/lab04.segments.txt
	{ $(READELF) -rW $(FULL_ELF); \
	  for object in $(EXERCISE_OBJECTS); do \
	    echo; echo "Relocations in $$object"; $(READELF) -rW $$object; \
	  done; } > $(BUILD_DIR)/lab04.relocations.txt
	$(READELF) -A $(FULL_ELF) > $(BUILD_DIR)/lab04.attributes.txt
	@echo "Inspection reports written under $(BUILD_DIR)/"

toolcheck:
	@command -v $(CC) >/dev/null || { echo "missing: $(CC)"; exit 1; }
	@command -v $(READELF) >/dev/null || { echo "missing: $(READELF)"; exit 1; }
	@command -v $(OBJDUMP) >/dev/null || { echo "missing: $(OBJDUMP)"; exit 1; }
	@command -v $(QEMU) >/dev/null || { echo "missing: $(QEMU)"; exit 1; }
	@command -v timeout >/dev/null || { echo "missing: timeout"; exit 1; }
	@echo "Compiler: $$($(CC) --version | head -n 1)"
	@echo "QEMU:     $$($(QEMU) --version | head -n 1)"

clean:
	rm -rf $(BUILD_DIR)
