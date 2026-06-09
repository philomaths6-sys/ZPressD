CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g \
          -I include \
          -D_GNU_SOURCE
LDFLAGS =

SRCDIR  = src
OBJDIR  = build
BINDIR  = bin

SRCS    = $(wildcard $(SRCDIR)/*.c)
OBJS    = $(SRCS:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
TARGET  = $(BINDIR)/zpressd

.PHONY: all clean install uninstall test fmt check

all: $(TARGET)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	@echo '  LINK  $@'

$(OBJDIR) $(BINDIR):
	mkdir -p $@

install: $(TARGET)
	sudo install -m 755 $(TARGET) /usr/local/sbin/zpressd
	sudo install -m 644 zpressd.conf.example /etc/zpressd.conf
	sudo install -m 644 zpressd.service /etc/systemd/system/
	sudo systemctl daemon-reload
	@echo 'Installed. Enable with: sudo systemctl enable --now zpressd'

uninstall:
	sudo systemctl stop zpressd 2>/dev/null || true
	sudo systemctl disable zpressd 2>/dev/null || true
	sudo rm -f /usr/local/sbin/zpressd
	sudo rm -f /etc/systemd/system/zpressd.service
	sudo systemctl daemon-reload

test:
	$(MAKE) -C tests

clean:
	rm -rf $(OBJDIR) $(BINDIR)

# Static analysis (requires cppcheck)
check:
	cppcheck --enable=all --std=c11 -I include src/

# Code format (requires clang-format)
fmt:
	clang-format -i src/*.c include/*.h