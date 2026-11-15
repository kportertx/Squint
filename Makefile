CC ?= gcc
CFLAGS ?= -O2 -g
# Set EV_INC to a directory containing ev.h when libev-dev is not installed system-wide,
# for example: make EV_INC=build/libev-dev/usr/include
EV_INC ?=
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-missing-braces -fno-strict-aliasing -fPIC -Ithird_party \
    $(shell pkg-config --cflags pixman-1) $(if $(EV_INC),-I$(EV_INC))

# The plugin resolves picom_api_get_interfaces from the picom executable at load time,
# so it must not link with --no-undefined. libev is linked by soname so the build does not
# need the libev.so development symlink, only the runtime library picom already uses.
LDLIBS = -l:libev.so.4 -lGL -lX11 -lXrandr -lxcb -lpixman-1

all: squint.so squintctl

# Written to a temporary name and renamed, so a picom that has the old squint.so mapped
# keeps its inode and does not crash while this rebuilds.
squint.so: src/squint.c src/pipeline.h src/config.h src/hotkey.h
	$(CC) $(CFLAGS) -shared -o $@.tmp $< $(LDLIBS) -lm && mv -f $@.tmp $@

pipeline_test: test/pipeline_test.c src/pipeline.h src/config.h
	$(CC) $(CFLAGS) -o $@ $< -lGL -lX11 -lm

hotkey_test: test/hotkey_test.c src/hotkey.h src/config.h
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lxcb -lXtst -lm

test: pipeline_test hotkey_test
	./pipeline_test
	./hotkey_test

squintctl: src/squintctl.c src/config.h
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lm

clean:
	rm -f squint.so squintctl pipeline_test hotkey_test

.PHONY: all test clean
