.PHONY: all host mips test integration integration-tls qemu-test stage clean
JOBS ?= 2
BUILD_TYPE ?= Release
all: host
host:
	cmake -S . -B build/host -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build build/host --parallel $(JOBS)
mips:
	@test -n "$(TOOLCHAIN_ROOT)" || { echo 'Set TOOLCHAIN_ROOT to the Ingenic GCC5.4 toolchain'; exit 1; }
	TOOLCHAIN_ROOT="$(TOOLCHAIN_ROOT)" cmake -S . -B build/mips -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_TOOLCHAIN_FILE=cmake/mips-uclibc.cmake
	cmake --build build/mips --parallel $(JOBS)
test: host
	ctest --test-dir build/host --output-on-failure --parallel 1
integration: host
	python3 tests/integration.py --binary build/host/jooanipc-open --output build/verification/host-replay.json
integration-tls: host
	python3 tests/integration.py --binary build/host/jooanipc-open --tls --output build/verification/host-tls.json
qemu-test:
	@test -n "$(OEM_ROOTFS)" || { echo 'Set OEM_ROOTFS to a private compatible rootfs extraction'; exit 1; }
	qemu-mipsel -L "$(OEM_ROOTFS)" build/mips/test-core
	qemu-mipsel -L "$(OEM_ROOTFS)" build/mips/test-imp
	python3 tests/integration.py --binary build/mips/jooanipc-open --qemu qemu-mipsel --sysroot "$(OEM_ROOTFS)" --tls --output build/verification/mips-qemu-tls.json
stage: host
	cmake --install build/host --prefix "$(CURDIR)/build/stage/usr" --strip
clean:
	cmake -E rm -rf build/host build/mips build/asan build/stage
