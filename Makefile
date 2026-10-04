CC = cc
CPPFLAGS = -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
LDLIBS = -pthread
TLS ?= 1
ifeq ($(TLS),1)
CPPFLAGS += -DWITH_TLS
LDLIBS += -lssl -lcrypto
endif
SOURCES = server.c connection.c http_parser.c static_files.c logging.c cache.c file_io.c metrics.c config.c file_cache.c transport.c router.c json.c

.PHONY: all clean test
all: http_server
http_server: $(SOURCES) connection.h http_parser.h static_files.h logging.h cache.h file_io.h metrics.h config.h file_cache.h transport.h router.h json.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(SOURCES) -o $@ $(LDLIBS)
test: http_server test_file_cache
	./test_file_cache
	python3 tests/test_server.py
	python3 tests/test_precompress.py
clean:
	rm -f http_server fuzz_parser fuzz_libfuzzer test_file_cache cache_lookup_benchmark

.PHONY: fuzz stress benchmark benchmark-cache
fuzz_parser: fuzz/fuzz_parser.c http_parser.c cache.c json.c http_parser.h cache.h json.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. fuzz/fuzz_parser.c http_parser.c cache.c json.c -o $@
fuzz: fuzz_parser
	./fuzz_parser 100000
# Requires a running server at 127.0.0.1:8080.
benchmark:
	python3 tools/benchmark.py
cache_lookup_benchmark: tools/cache_lookup_benchmark.c file_cache.c config.c file_cache.h config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. tools/cache_lookup_benchmark.c file_cache.c config.c -o $@
benchmark-cache: cache_lookup_benchmark
	./cache_lookup_benchmark
stress:
	python3 tools/stress.py
# Optional coverage-guided build, requires Clang with libFuzzer.
fuzz_libfuzzer: fuzz/fuzz_parser.c http_parser.c cache.c json.c
	clang -std=c11 -g -O1 -I. -DLIBFUZZER -fsanitize=fuzzer,address,undefined fuzz/fuzz_parser.c http_parser.c cache.c json.c -o $@

test_file_cache: tests/test_file_cache.c file_cache.c config.c file_cache.h config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. tests/test_file_cache.c file_cache.c config.c -o $@

.PHONY: precompress
precompress:
	python3 tools/precompress.py public
