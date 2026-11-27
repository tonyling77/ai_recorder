CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O0 -g
ROOT := ..
INCLUDES := -I$(ROOT)/include
SRCS := $(ROOT)/recorder_node.c test_recorder_node.c
BIN := test_recorder_node
GESTURE_SRCS := $(ROOT)/wifi_gesture.c test_wifi_gesture.c
GESTURE_BIN := test_wifi_gesture
AI_KEY_SRCS := $(ROOT)/wifi_gesture.c $(ROOT)/ai_key_wifi.c test_ai_key_wifi.c
AI_KEY_BIN := test_ai_key_wifi

.PHONY: test clean
test: $(BIN) $(GESTURE_BIN) $(AI_KEY_BIN)
	./$(BIN)
	./$(GESTURE_BIN)
	./$(AI_KEY_BIN)
	python3 -m unittest test_factory_policy.py
	python3 -m unittest test_clock_policy.py

$(BIN): $(SRCS) $(ROOT)/include/recorder_node.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRCS)

$(GESTURE_BIN): $(GESTURE_SRCS) $(ROOT)/include/wifi_gesture.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(GESTURE_SRCS)

$(AI_KEY_BIN): $(AI_KEY_SRCS) $(ROOT)/include/ai_key_wifi.h $(ROOT)/include/wifi_gesture.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(AI_KEY_SRCS)

clean:
	rm -f $(BIN) $(GESTURE_BIN) $(AI_KEY_BIN)
