CC = cc
CPPFLAGS = -D_FILE_OFFSET_BITS=64 -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
LDLIBS = -pthread -lcrypto
TLS ?= 1
ifeq ($(TLS),1)
CPPFLAGS += -DWITH_TLS
LDLIBS += -lssl
endif
SOURCES = src/server.c src/connection.c src/http_parser.c src/static_files.c src/logging.c src/cache.c src/file_io.c src/metrics.c src/config.c src/file_cache.c src/transport.c src/router.c src/json.c src/auth.c src/deployment.c

.PHONY: all clean test
all: http_server
http_server: $(SOURCES) include/connection.h include/http_parser.h include/static_files.h include/logging.h include/cache.h include/file_io.h include/metrics.h include/config.h include/file_cache.h include/transport.h include/router.h include/json.h include/auth.h include/deployment.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(SOURCES) -o $@ $(LDLIBS)
test: http_server test_file_cache test_auth test_deployment
	./test_file_cache
	./test_auth
	./test_deployment
	python3 tests/test_server.py
	python3 tests/test_precompress.py
	python3 tests/test_auth_tools.py
clean:
	rm -f http_server fuzz_parser fuzz_libfuzzer test_file_cache test_auth test_deployment cache_lookup_benchmark

.PHONY: fuzz stress benchmark benchmark-cache
fuzz_parser: fuzz/fuzz_parser.c src/http_parser.c src/cache.c src/json.c include/http_parser.h include/cache.h include/json.h
	$(CC) $(CPPFLAGS) $(CFLAGS) fuzz/fuzz_parser.c src/http_parser.c src/cache.c src/json.c -o $@ -lcrypto
fuzz: fuzz_parser
	./fuzz_parser 100000
# Requires a running server at 127.0.0.1:8080.
benchmark:
	python3 tools/benchmark.py
cache_lookup_benchmark: tools/cache_lookup_benchmark.c src/file_cache.c src/config.c include/file_cache.h include/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/cache_lookup_benchmark.c src/file_cache.c src/config.c -o $@
benchmark-cache: cache_lookup_benchmark
	./cache_lookup_benchmark
stress:
	python3 tools/stress.py
# Optional coverage-guided build, requires Clang with libFuzzer.
fuzz_libfuzzer: fuzz/fuzz_parser.c src/http_parser.c src/cache.c src/json.c
	clang -std=c11 -g -O1 -Iinclude -DLIBFUZZER -fsanitize=fuzzer,address,undefined fuzz/fuzz_parser.c src/http_parser.c src/cache.c src/json.c -o $@ -lcrypto

test_file_cache: tests/test_file_cache.c src/file_cache.c src/config.c include/file_cache.h include/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_file_cache.c src/file_cache.c src/config.c -o $@

.PHONY: precompress
precompress:
	python3 tools/precompress.py public

test_auth: tests/test_auth.c src/auth.c src/config.c include/auth.h include/config.h include/http_parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_auth.c src/auth.c src/config.c -o $@ -lcrypto

test_deployment: tests/test_deployment.c $(filter-out src/server.c src/connection.c,$(SOURCES)) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread tests/test_deployment.c $(filter-out src/server.c src/connection.c,$(SOURCES)) -o $@ $(LDLIBS)
