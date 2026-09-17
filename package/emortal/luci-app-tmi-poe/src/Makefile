# SPDX-License-Identifier: GPL-2.0-only

CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Werror
LDLIBS += -luci
OBJECTS := main.o tmi.o log.o log_sink.o

.PHONY: compile install clean
compile: tmi-poe

tmi-poe: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJECTS): tmi.h log.h log_sink.h

install: compile
	install -d $(DESTDIR)/usr/sbin
	install -m 0755 tmi-poe $(DESTDIR)/usr/sbin/tmi-poe

clean:
	rm -f tmi-poe $(OBJECTS)
