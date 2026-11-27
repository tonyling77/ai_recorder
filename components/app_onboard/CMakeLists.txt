idf_component_register(
    SRCS "app_onboard.c" "recorder_node.c" "wifi_gesture.c" "ai_key_wifi.c" "node_presence.c" "inbox_config_rpc.c"
            "recorder_control_rpc.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_hello app_recorder app_clock esp_rmaker_neo app_network network_provisioning nvs_flash esp_netif esp_wifi esp_event
                 esp-mosaico-bsp esp_driver_gpio esp_timer esp_system esp_iris)
