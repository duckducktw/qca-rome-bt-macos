CC ?= clang
CXX ?= clang++
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WARN := -Wall -Wextra -Werror
GEN := build/gen/qca_rome_firmware.h

.PHONY: test test-nofw firmware clean kext
firmware: $(GEN)
$(GEN): scripts/gen_firmware_header.py
	python3 scripts/gen_firmware_header.py $@

build/test_nofw: src/core/qca_rome.c src/core/qca_rome.h tests/test_qca_rome.cpp
	@mkdir -p build
	$(CC) -std=c11 $(WARN) $(SAN) -g -c src/core/qca_rome.c -o build/qca_rome_nofw.o
	$(CXX) -std=c++17 $(WARN) $(SAN) -g -Isrc/core tests/test_qca_rome.cpp build/qca_rome_nofw.o -o $@

build/test_fw: src/core/qca_rome.c src/core/qca_rome.h tests/test_qca_rome.cpp $(GEN)
	@mkdir -p build
	$(CC) -std=c11 $(WARN) $(SAN) -g -c src/core/qca_rome.c -o build/qca_rome_fw.o
	$(CXX) -std=c++17 $(WARN) $(SAN) -g -DQCA_HAVE_FIRMWARE -Isrc/core -Ibuild/gen tests/test_qca_rome.cpp build/qca_rome_fw.o -o $@

test-nofw: build/test_nofw
	./build/test_nofw
test: build/test_nofw build/test_fw
	./build/test_nofw
	./build/test_fw

kext: $(GEN)
	./scripts/build_kext_macos.sh

clean:
	rm -rf build
