# 1.7 suite: local tests and implementation, shared harness from 1.6.
ifeq ($(origin CC),default)
  CC := gcc
endif

COMMON  := ../mythread_tests
CFLAGS  ?= -O2 -g
LDFLAGS ?= -pthread
IMPL    ?= custom

ifeq ($(IMPL),reference)
  IMPL_DIR := $(COMMON)/impl/reference
  IMPL_SRC := $(IMPL_DIR)/mythread.c
else ifeq ($(IMPL),custom)
  IMPL_DIR := impl/custom
  IMPL_SRC := $(IMPL_DIR)/mythread.c
else
  $(error Unknown IMPL=$(IMPL))
endif

override CFLAGS += -std=c11 -Wall -Wextra
override CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
override CFLAGS += -I$(COMMON)/include -I$(IMPL_DIR) -I$(COMMON)/framework -Itests
override CFLAGS += -include $(COMMON)/include/mythread.h

ifeq ($(strip $(STRICT)),1)
  override CFLAGS += -DMYTHREAD_STRICT_ERRORS
endif

SRC := \
    main.c \
    $(COMMON)/framework/runner.c \
    tests/test_create.c \
    tests/test_join.c \
    tests/test_detach.c \
    tests/test_exit.c \
    tests/test_stress.c \
    $(IMPL_SRC)

mythread_tests: $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

check: mythread_tests
	./mythread_tests

check-custom:
	$(MAKE) clean
	$(MAKE) IMPL=custom mythread_tests
	./mythread_tests

check-reference:
	$(MAKE) clean
	$(MAKE) IMPL=reference mythread_tests
	./mythread_tests

check-strict:
	$(MAKE) clean
	$(MAKE) IMPL=custom STRICT=1 mythread_tests
	./mythread_tests

check-repeat: mythread_tests
	MYTHREAD_REPEAT=20 ./mythread_tests

clean:
	rm -f mythread_tests

.PHONY: check check-custom check-reference check-strict check-repeat clean
