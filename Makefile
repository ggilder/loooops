# Makefile for looper~ and patch_menu externals

PD_PATH ?= /usr/local/include        # Path to Pd headers

# Detect platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  EXT     = pd_darwin
  CFLAGS  += -fPIC -O2 -I$(PD_PATH) -Wall -W -Wshadow -Wno-unused -Wno-cast-function-type
  CFLAGS_C  = -Wstrict-prototypes
  LDFLAGS += -bundle -undefined dynamic_lookup
else
  EXT     = pd_linux
  CFLAGS  += -fPIC -O2 -I$(PD_PATH) -Wall -W -Wshadow -Wno-unused -Wno-cast-function-type
  CFLAGS_C  = -Wstrict-prototypes
  LDFLAGS += -shared
endif

LOOPER_TARGET     = looper~.$(EXT)
PATCH_MENU_TARGET = patch_menu.$(EXT)

TEST_BIN = tests/test_patch_menu

all: $(LOOPER_TARGET) $(PATCH_MENU_TARGET)

$(LOOPER_TARGET): looper~.c
	$(CC) $(CFLAGS) $(CFLAGS_C) -o $@ $< $(LDFLAGS)

$(PATCH_MENU_TARGET): patch_menu.cpp
	$(CXX) $(CFLAGS) -std=c++11 -o $@ $< $(LDFLAGS)

# Unit tests — compiled against a mock PD API, no PD runtime required.
# -Itests/ makes the compiler find tests/m_pd.h before the real m_pd.h.
test: $(TEST_BIN)
	./$(TEST_BIN); rm -f $(TEST_BIN)

$(TEST_BIN): tests/test_patch_menu.cpp patch_menu.cpp tests/m_pd.h
	$(CXX) -std=c++11 -Itests -Wall -Wno-unused -Wno-missing-field-initializers \
	    -o $@ tests/test_patch_menu.cpp

clean:
	rm -f $(LOOPER_TARGET) $(PATCH_MENU_TARGET) $(TEST_BIN)

.PHONY: all clean
