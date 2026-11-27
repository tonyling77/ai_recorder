CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O0 -g
ROOT := ..
QRCODE := ../../../managed_components/espressif__qrcode
INCLUDES := -I$(ROOT)/include -I$(QRCODE) -I../../app_recorder/include
SCREEN_SRCS := $(ROOT)/provision_screen.c test_provision_screen.c
SCREEN_BIN := test_provision_screen
WIFI_SRCS := $(ROOT)/wifi_picker.c test_wifi_picker.c
WIFI_BIN := test_wifi_picker
SHEET_SRCS := $(ROOT)/wifi_sheet.c test_wifi_sheet.c
SHEET_BIN := test_wifi_sheet
LINK_SRCS := $(ROOT)/wifi_link.c test_wifi_link.c
LINK_BIN := test_wifi_link
PLACE_SRCS := $(ROOT)/wifi_key_place.c test_wifi_key_place.c
PLACE_BIN := test_wifi_key_place
NOTES_SRCS := $(ROOT)/meeting_notes.c test_meeting_notes.c
NOTES_BIN := test_meeting_notes
CHANNEL_SRCS := $(ROOT)/uplink_channel.c test_uplink_channel.c
CHANNEL_BIN := test_uplink_channel
XIAOZHI_NOTE_SRCS := $(ROOT)/meeting_notes.c $(ROOT)/xiaozhi_notes.c $(ROOT)/xiaozhi_ws.c ../../app_recorder/recorder_ogg.c test_xiaozhi_notes.c
XIAOZHI_NOTE_BIN := test_xiaozhi_notes
XIAOZHI_WS_SRCS := $(ROOT)/xiaozhi_ws.c test_xiaozhi_ws.c
XIAOZHI_WS_BIN := test_xiaozhi_ws
SCAN_SRCS := $(ROOT)/wifi_scan_plan.c test_wifi_scan_plan.c
SCAN_BIN := test_wifi_scan_plan
QR_SRCS := $(ROOT)/provision_qr.c $(QRCODE)/qrcodegen.c test_provision_qr.c
QR_BIN := test_provision_qr

.PHONY: test clean
test: $(SCREEN_BIN) $(QR_BIN) $(WIFI_BIN) $(SHEET_BIN) $(LINK_BIN) $(PLACE_BIN) $(NOTES_BIN) $(CHANNEL_BIN) $(SCAN_BIN) $(XIAOZHI_NOTE_BIN) $(XIAOZHI_WS_BIN)
	./$(SCREEN_BIN)
	./$(QR_BIN)
	./$(WIFI_BIN)
	./$(SHEET_BIN)
	./$(LINK_BIN)
	./$(PLACE_BIN)
	./$(NOTES_BIN)
	./$(CHANNEL_BIN)
	./$(SCAN_BIN)
	./$(XIAOZHI_NOTE_BIN)
	./$(XIAOZHI_WS_BIN)
	python3 -m unittest test_notes_sync_policy.py
	python3 -m unittest test_upload_status_policy.py
	python3 -m unittest test_xiaozhi_logging_policy.py
	python3 -m unittest test_recordings_panel_policy.py

$(SCREEN_BIN): $(SCREEN_SRCS) $(ROOT)/include/provision_screen.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SCREEN_SRCS)

$(QR_BIN): $(QR_SRCS) $(ROOT)/include/provision_qr.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(QR_SRCS)

$(WIFI_BIN): $(WIFI_SRCS) $(ROOT)/include/wifi_picker.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(WIFI_SRCS)

$(SHEET_BIN): $(SHEET_SRCS) $(ROOT)/include/wifi_sheet.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SHEET_SRCS)

$(LINK_BIN): $(LINK_SRCS) $(ROOT)/include/wifi_link.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(LINK_SRCS)

$(PLACE_BIN): $(PLACE_SRCS) $(ROOT)/include/wifi_key_place.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(PLACE_SRCS)

$(NOTES_BIN): $(NOTES_SRCS) $(ROOT)/include/meeting_notes.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(NOTES_SRCS)

$(CHANNEL_BIN): $(CHANNEL_SRCS) $(ROOT)/include/uplink_channel.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(CHANNEL_SRCS)

$(SCAN_BIN): $(SCAN_SRCS) $(ROOT)/include/wifi_scan_plan.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SCAN_SRCS)

$(XIAOZHI_NOTE_BIN): $(XIAOZHI_NOTE_SRCS) $(ROOT)/include/xiaozhi_notes.h $(ROOT)/include/meeting_notes.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(XIAOZHI_NOTE_SRCS)

$(XIAOZHI_WS_BIN): $(XIAOZHI_WS_SRCS) $(ROOT)/include/xiaozhi_ws.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(XIAOZHI_WS_SRCS)

clean:
	rm -f $(SCREEN_BIN) $(QR_BIN) $(WIFI_BIN) $(SHEET_BIN) $(LINK_BIN) $(PLACE_BIN) $(NOTES_BIN) $(CHANNEL_BIN) $(SCAN_BIN) $(XIAOZHI_NOTE_BIN) $(XIAOZHI_WS_BIN)
