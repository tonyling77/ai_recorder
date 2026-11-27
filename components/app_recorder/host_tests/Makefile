CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O0 -g
ROOT := ..
INCLUDES := -I$(ROOT)/include
SESSION_SRCS := $(ROOT)/recorder_session.c test_recorder_session.c
SESSION_BIN := test_recorder_session
CAPTURE_SRCS := $(ROOT)/recorder_session.c $(ROOT)/recorder_capture.c test_recorder_capture.c
CAPTURE_BIN := test_recorder_capture
ENCODE_SRCS := $(ROOT)/recorder_encoder.c $(ROOT)/recorder_ogg.c test_recorder_encoder.c
ENCODE_BIN := test_recorder_encoder
FRAMES_SRCS := $(ROOT)/recorder_session.c $(ROOT)/recorder_capture.c $(ROOT)/recorder_encoder.c \
	$(ROOT)/recorder_ogg.c $(ROOT)/recorder_frames.c test_recorder_frames.c
FRAMES_BIN := test_recorder_frames
STORE_SRCS := $(ROOT)/recorder_store.c $(ROOT)/recorder_store_lock.c test_recorder_store.c
STORE_BIN := test_recorder_store
LIBRARY_SRCS := $(ROOT)/recorder_store.c $(ROOT)/recorder_store_lock.c $(ROOT)/recorder_library.c test_recorder_library.c
LIBRARY_BIN := test_recorder_library
TAKE_SRCS := $(ROOT)/recorder_session.c $(ROOT)/recorder_capture.c $(ROOT)/recorder_encoder.c \
	$(ROOT)/recorder_ogg.c $(ROOT)/recorder_frames.c $(ROOT)/recorder_store.c $(ROOT)/recorder_store_lock.c \
	$(ROOT)/recorder_library.c $(ROOT)/recorder_take.c test_recorder_take.c
TAKE_BIN := test_recorder_take
UPLOAD_SRCS := $(ROOT)/recorder_upload.c $(ROOT)/recorder_inbox.c test_recorder_upload.c
UPLOAD_BIN := test_recorder_upload
UPLOAD_LAN_SRCS := $(ROOT)/recorder_upload.c $(ROOT)/recorder_inbox.c test_recorder_upload_lan.c
UPLOAD_LAN_BIN := test_recorder_upload_lan
CONTROL_SRCS := $(ROOT)/recorder_session.c $(ROOT)/recorder_capture.c $(ROOT)/recorder_encoder.c \
	$(ROOT)/recorder_ogg.c $(ROOT)/recorder_frames.c $(ROOT)/recorder_store.c $(ROOT)/recorder_store_lock.c \
	$(ROOT)/recorder_library.c $(ROOT)/recorder_take.c $(ROOT)/recorder_control.c test_recorder_control.c
CONTROL_BIN := test_recorder_control
ASYNC_SRCS := $(ROOT)/recorder_async.c test_recorder_async.c
ASYNC_BIN := test_recorder_async
OPUS_CFLAGS := $(shell pkg-config --cflags opus)
OPUS_LIBS := $(shell pkg-config --libs opus)

.PHONY: test clean
test: $(SESSION_BIN) $(CAPTURE_BIN) $(ENCODE_BIN) $(FRAMES_BIN) $(STORE_BIN) $(LIBRARY_BIN) \
		$(TAKE_BIN) $(UPLOAD_BIN) $(UPLOAD_LAN_BIN) $(CONTROL_BIN) $(ASYNC_BIN)
	./$(SESSION_BIN)
	./$(CAPTURE_BIN)
	./$(ENCODE_BIN)
	./$(FRAMES_BIN)
	./$(STORE_BIN)
	./$(LIBRARY_BIN)
	./$(TAKE_BIN)
	./$(UPLOAD_BIN)
	./$(UPLOAD_LAN_BIN)
	./$(CONTROL_BIN)
	./$(ASYNC_BIN)
	python3 -m unittest test_upload_lock_policy.py
	python3 -m unittest test_clock_upload_policy.py
	python3 -m unittest test_store_file_policy.py
	python3 -m unittest test_lan_inbox_policy.py
	python3 -m unittest test_write_stack_policy.py

$(SESSION_BIN): $(SESSION_SRCS) $(ROOT)/include/recorder_session.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SESSION_SRCS)

$(CAPTURE_BIN): $(CAPTURE_SRCS) $(ROOT)/include/recorder_session.h $(ROOT)/include/recorder_capture.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(CAPTURE_SRCS)

$(ENCODE_BIN): $(ENCODE_SRCS) $(ROOT)/include/recorder_encoder.h
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) $(INCLUDES) -o $@ $(ENCODE_SRCS) $(OPUS_LIBS)

$(FRAMES_BIN): $(FRAMES_SRCS) $(ROOT)/include/recorder_frames.h
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) $(INCLUDES) -o $@ $(FRAMES_SRCS) $(OPUS_LIBS)

$(STORE_BIN): $(STORE_SRCS) $(ROOT)/include/recorder_store.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(STORE_SRCS)

$(LIBRARY_BIN): $(LIBRARY_SRCS) $(ROOT)/include/recorder_library.h $(ROOT)/include/recorder_store.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(LIBRARY_SRCS)

$(TAKE_BIN): $(TAKE_SRCS) $(ROOT)/include/recorder_take.h
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) $(INCLUDES) -o $@ $(TAKE_SRCS) $(OPUS_LIBS)

# Host tests inject a fake origin; firmware builds use UPLOAD_ORIGIN from .env.
# The firmware generates the same header at configure time, so the tests read
# the origin the way the firmware does.
GEN := gen
HTTPS_CONFIG := $(GEN)/https/recorder_upload_config.h
LAN_CONFIG := $(GEN)/lan/recorder_upload_config.h

$(HTTPS_CONFIG):
	mkdir -p $(dir $@)
	printf '#pragma once\n#define RECORDER_UPLOAD_TOKEN "0123456789abcdef"\n#define RECORDER_UPLOAD_ORIGIN "https://inbox.example.test"\n' > $@

$(LAN_CONFIG):
	mkdir -p $(dir $@)
	printf '#pragma once\n#define RECORDER_UPLOAD_TOKEN "0123456789abcdef"\n#define RECORDER_UPLOAD_ORIGIN "http://inbox.example.test:8765"\n' > $@

$(UPLOAD_BIN): $(UPLOAD_SRCS) $(ROOT)/include/recorder_upload.h $(HTTPS_CONFIG)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(GEN)/https -o $@ $(UPLOAD_SRCS)

$(UPLOAD_LAN_BIN): $(UPLOAD_LAN_SRCS) $(ROOT)/include/recorder_upload.h $(LAN_CONFIG)
	$(CC) $(CFLAGS) $(INCLUDES) -I$(GEN)/lan -o $@ $(UPLOAD_LAN_SRCS)

$(CONTROL_BIN): $(CONTROL_SRCS) $(ROOT)/include/recorder_control.h
	$(CC) $(CFLAGS) $(OPUS_CFLAGS) $(INCLUDES) -o $@ $(CONTROL_SRCS) $(OPUS_LIBS)

$(ASYNC_BIN): $(ASYNC_SRCS) $(ROOT)/include/recorder_async.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(ASYNC_SRCS)

clean:
	rm -f $(SESSION_BIN) $(CAPTURE_BIN) $(ENCODE_BIN) $(FRAMES_BIN) $(STORE_BIN) $(LIBRARY_BIN) \
		$(TAKE_BIN) $(UPLOAD_BIN) $(UPLOAD_LAN_BIN) $(CONTROL_BIN) $(ASYNC_BIN)
