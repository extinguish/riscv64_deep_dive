CROSS_COMPILE ?= riscv64-unknown-elf-
CC := $(CROSS_COMPILE)gcc
NM := $(shell command -v $(CROSS_COMPILE)nm 2>/dev/null || command -v nm)
OBJDUMP := $(CROSS_COMPILE)objdump
READELF := $(shell command -v $(CROSS_COMPILE)readelf 2>/dev/null || command -v readelf)
QEMU ?= qemu-system-riscv64
BUILD := build
ARCH := -march=rv64imach_zicsr_zifencei -mabi=lp64 -mcmodel=medany
COMMON := $(ARCH) -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -msmall-data-limit=0 -Wall -Wextra -Werror -O2 -g
LINK := $(ARCH) -nostdlib -nostartfiles -static -no-pie -Wl,-T,linker.ld -Wl,--build-id=none
SOURCES := infra/reset.S infra/trap.S infra/guest.S infra/platform.c infra/main.c exercises/gstage.S
QFLAGS := -machine virt -cpu rv64,h=true -m 128M -smp 1 -display none -monitor none -serial stdio -no-reboot -bios none
QEMU_TIMEOUT ?= 12s

.PHONY: build smoke unit test inspect toolcheck clean
build: $(BUILD)/lab14.elf
$(BUILD)/lab14.elf: $(SOURCES) linker.ld include/lab_api.h include/lab_layout.h
	mkdir -p $(BUILD)
	$(CC) $(COMMON) -Iinclude $(LINK) $(SOURCES) -o $@
$(BUILD)/lab14-smoke.elf: $(SOURCES) linker.ld include/lab_api.h include/lab_layout.h
	mkdir -p $(BUILD)
	$(CC) $(COMMON) -DLAB_SMOKE -Iinclude $(LINK) $(SOURCES) -o $@
$(BUILD)/lab14-unit.elf: $(SOURCES) linker.ld include/lab_api.h include/lab_layout.h
	mkdir -p $(BUILD)
	$(CC) $(COMMON) -DLAB_UNIT -Iinclude $(LINK) $(SOURCES) -o $@
smoke: $(BUILD)/lab14-smoke.elf
	./scripts/run-qemu.sh $(QEMU_TIMEOUT) $(QEMU) $(QFLAGS) -kernel $<
unit: $(BUILD)/lab14-unit.elf
	./scripts/run-qemu.sh $(QEMU_TIMEOUT) $(QEMU) $(QFLAGS) -kernel $<
test: check-elf
	./scripts/run-qemu.sh $(QEMU_TIMEOUT) $(QEMU) $(QFLAGS) -kernel $(BUILD)/lab14.elf
check-elf: $(BUILD)/lab14.elf
	READELF=$(READELF) NM=$(NM) OBJDUMP=$(OBJDUMP) ./scripts/check-elf.sh $<
inspect: $(BUILD)/lab14.elf
	$(OBJDUMP) -d -S -M no-aliases,numeric $(BUILD)/lab14.elf > $(BUILD)/lab14.disasm.txt
	$(READELF) -SW $(BUILD)/lab14.elf > $(BUILD)/lab14.sections.txt
	$(READELF) -lW $(BUILD)/lab14.elf > $(BUILD)/lab14.segments.txt
	$(READELF) -rW $(BUILD)/lab14.elf > $(BUILD)/lab14.relocations.txt
	$(READELF) -A $(BUILD)/lab14.elf > $(BUILD)/lab14.attributes.txt
toolcheck:
	@command -v $(CC) >/dev/null
	@command -v $(QEMU) >/dev/null
	@command -v $(READELF) >/dev/null
	@command -v $(OBJDUMP) >/dev/null
	@command -v timeout >/dev/null
	@$(CC) --version | head -1
	@$(QEMU) --version | head -1
clean:
	rm -rf $(BUILD)
