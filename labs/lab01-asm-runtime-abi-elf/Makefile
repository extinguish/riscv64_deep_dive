CROSS_COMPILE ?= riscv64-unknown-elf-
CC := $(CROSS_COMPILE)gcc
NM := $(shell command -v $(CROSS_COMPILE)nm 2>/dev/null || command -v nm)
OBJDUMP := $(shell command -v $(CROSS_COMPILE)objdump 2>/dev/null || command -v objdump)
READELF := $(shell command -v $(CROSS_COMPILE)readelf 2>/dev/null || command -v readelf)
QEMU ?= qemu-system-riscv64

BUILD_DIR := build
FULL_ELF := $(BUILD_DIR)/lab01.elf
SMOKE_ELF := $(BUILD_DIR)/lab01-smoke.elf

ARCH_FLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
COMMON_FLAGS := $(ARCH_FLAGS) -ffreestanding -fno-builtin -fno-stack-protector \
	-fno-pic -fno-pie -msmall-data-limit=0 -Wall -Wextra -Werror -O2 -g
ASFLAGS := $(COMMON_FLAGS)
CFLAGS := $(COMMON_FLAGS) -std=c11
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles -static -no-pie \
	-Wl,-T,linker.ld -Wl,--gc-sections -Wl,--build-id=none
QEMU_FLAGS := -machine virt -cpu rv64 -m 128M -smp 1 -display none \
	-monitor none -serial stdio -no-reboot -bios none
QEMU_TIMEOUT ?= 10s

COMMON_OBJECTS := \
	$(BUILD_DIR)/infra/start.o \
	$(BUILD_DIR)/infra/platform.o

EXERCISE_OBJECTS := \
	$(BUILD_DIR)/exercises/runtime.o \
	$(BUILD_DIR)/exercises/abi_frame.o \
	$(BUILD_DIR)/exercises/addressing.o \
	$(BUILD_DIR)/exercises/pic_blob.o

FULL_OBJECTS := \
	$(COMMON_OBJECTS) \
	$(BUILD_DIR)/infra/test_runner.o \
	$(BUILD_DIR)/infra/abi_check.o \
	$(EXERCISE_OBJECTS)

.PHONY: all build smoke test inspect check-elf toolcheck clean

all: build

build: $(FULL_ELF)

$(BUILD_DIR)/infra/%.o: infra/%.S | $(BUILD_DIR)/infra
	$(CC) $(ASFLAGS) -Iinclude -c $< -o $@

$(BUILD_DIR)/infra/%.o: infra/%.c | $(BUILD_DIR)/infra
	$(CC) $(CFLAGS) -Iinclude -c $< -o $@

$(BUILD_DIR)/exercises/%.o: exercises/%.S | $(BUILD_DIR)/exercises
	$(CC) $(ASFLAGS) -Iinclude -c $< -o $@

$(FULL_ELF): linker.ld $(FULL_OBJECTS)
	$(CC) $(LDFLAGS) $(FULL_OBJECTS) -o $@

$(SMOKE_ELF): linker.ld $(COMMON_OBJECTS) $(BUILD_DIR)/infra/smoke_main.o
	$(CC) $(LDFLAGS) $(COMMON_OBJECTS) $(BUILD_DIR)/infra/smoke_main.o -o $@

$(BUILD_DIR)/infra $(BUILD_DIR)/exercises:
	mkdir -p $@

smoke: $(SMOKE_ELF)
	@./scripts/run-qemu.sh "$(QEMU_TIMEOUT)" "$(QEMU)" $(QEMU_FLAGS) -kernel $(SMOKE_ELF)

test: check-elf
	@./scripts/run-qemu.sh "$(QEMU_TIMEOUT)" "$(QEMU)" $(QEMU_FLAGS) -kernel $(FULL_ELF)

check-elf: $(FULL_ELF)
	READELF=$(READELF) NM=$(NM) OBJDUMP=$(OBJDUMP) ./scripts/check-elf.sh $(FULL_ELF)

inspect: check-elf
	$(OBJDUMP) -d -S -M no-aliases,numeric $(FULL_ELF) > $(BUILD_DIR)/lab01.disasm.txt
	$(READELF) -SW $(FULL_ELF) > $(BUILD_DIR)/lab01.sections.txt
	{ $(READELF) -rW $(FULL_ELF); \
	  for object in $(EXERCISE_OBJECTS); do \
	    echo; echo "Relocations in $$object"; $(READELF) -rW $$object; \
	  done; } > $(BUILD_DIR)/lab01.relocations.txt
	$(READELF) -A $(FULL_ELF) > $(BUILD_DIR)/lab01.attributes.txt
	$(READELF) --debug-dump=frames-interp $(FULL_ELF) > $(BUILD_DIR)/lab01.frames.txt
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
