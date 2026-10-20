CXX ?= clang++
CXXFLAGS ?= -O2 -std=c++26 -Wall -Wextra -pedantic
PKG_CONFIG ?= pkg-config

XFT_CFLAGS := $(shell $(PKG_CONFIG) --cflags xft 2>/dev/null)
XFT_LIBS := $(shell $(PKG_CONFIG) --libs xft 2>/dev/null)

ifneq ($(strip $(XFT_LIBS)),)
  CXXFLAGS += $(XFT_CFLAGS)
  LDLIBS += $(XFT_LIBS)
endif

LDLIBS += -lX11 -lsqlite3

all: kd_picker

kd_picker: main.cc
	$(CXX) $(CXXFLAGS) -fno-exceptions -o $@ $< $(LDLIBS)

clean:
	rm -f kd_picker

.PHONY: all clean
