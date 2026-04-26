# Makefile for looper~, patch_menu, stereo_pan~, and statusline externals

PD_PATH ?= pd                        # Path to Pd headers (vendored from Pd-0.56-2.app)

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

LOOPER_TARGET      = looper~.$(EXT)
PATCH_MENU_TARGET  = patch_menu.$(EXT)
STEREO_PAN_TARGET  = stereo_pan~.$(EXT)
DISKRECORD_TARGET  = diskrecord~.$(EXT)
STATUSLINE_TARGET  = statusline.$(EXT)

TEST_PM  = tests/test_patch_menu
TEST_PAN = tests/test_stereo_pan_tilde
TEST_SL  = tests/test_statusline

CXXTEST = $(CXX) -std=c++11 -Itests -Wall -Wno-unused -Wno-missing-field-initializers

all: $(LOOPER_TARGET) $(PATCH_MENU_TARGET) $(STEREO_PAN_TARGET) $(DISKRECORD_TARGET) $(STATUSLINE_TARGET)

$(LOOPER_TARGET): looper~.c
	$(CC) $(CFLAGS) $(CFLAGS_C) -o $@ $< $(LDFLAGS)

$(PATCH_MENU_TARGET): patch_menu.cpp
	$(CXX) $(CFLAGS) -std=c++11 -o $@ $< $(LDFLAGS)

$(STEREO_PAN_TARGET): stereo_pan~.cpp
	$(CXX) $(CFLAGS) -std=c++11 -o $@ $< $(LDFLAGS)

$(DISKRECORD_TARGET): diskrecord~.c
	$(CC) $(CFLAGS) $(CFLAGS_C) -std=gnu11 -o $@ $< $(LDFLAGS) -lpthread

$(STATUSLINE_TARGET): statusline.cpp
	$(CXX) $(CFLAGS) -std=c++11 -o $@ $< $(LDFLAGS)

# Unit tests
test: $(TEST_PM) $(TEST_PAN) $(TEST_SL)
	./$(TEST_PM);  rm -f $(TEST_PM)
	./$(TEST_PAN); rm -f $(TEST_PAN)
	./$(TEST_SL);  rm -f $(TEST_SL)

$(TEST_PM): tests/test_patch_menu.cpp patch_menu.cpp tests/m_pd.h
	$(CXXTEST) -o $@ tests/test_patch_menu.cpp

$(TEST_PAN): tests/test_stereo_pan_tilde.cpp stereo_pan~.cpp tests/m_pd.h
	$(CXXTEST) -o $@ tests/test_stereo_pan_tilde.cpp

$(TEST_SL): tests/test_statusline.cpp statusline.cpp tests/m_pd.h
	$(CXXTEST) -o $@ tests/test_statusline.cpp

clean:
	rm -f $(LOOPER_TARGET) $(PATCH_MENU_TARGET) $(STEREO_PAN_TARGET) $(DISKRECORD_TARGET) $(STATUSLINE_TARGET) $(TEST_PM) $(TEST_PAN) $(TEST_SL)

.PHONY: all clean test
