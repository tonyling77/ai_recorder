import unittest
from pathlib import Path


class FactoryPartitionPolicyTest(unittest.TestCase):
    def test_claim_credentials_are_reset_once_before_provisioning(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        reset = source.split("static void reset_claim_once", 1)[1].split("\nstatic ", 1)[0]
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        self.assertIn("esp_rmaker_credentials_erase_claim_data", reset)
        self.assertIn("app_network_reset_credentials", reset)
        self.assertIn("claim_purged", reset)
        self.assertIn("esp_restart", reset)
        self.assertNotIn("nvs_flash_erase", reset)
        self.assertLess(onboard.index("app_network_init"), onboard.index("reset_claim_once"))
        self.assertLess(onboard.index("reset_claim_once"), onboard.index("app_network_provision("))

    def test_onboarding_never_erases_factory_partition(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        self.assertNotIn("nvs_flash_erase_partition", source)

    def test_ai_key_clears_wifi_credentials_only(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        self.assertIn("ai_key_wifi_sample", source)
        self.assertIn("BSP_BUTTON_AI_GPIO", source)
        self.assertIn("AI_KEY_WIFI_RECORD", source)
        self.assertNotIn("nvs_flash_erase", source)
        self.assertNotIn("NEO_MFG_DATA_DEVICE_TYPE_NONE", source)
        self.assertIn("NEO_MFG_DATA_DEVICE_TYPE_LIGHT", source)

    def test_five_press_factory_resets_after_stopping_sta(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        task = source.split("static void ai_key_task", 1)[1].split("static void start_ai_key", 1)[0]
        clear = task.split("AI_KEY_WIFI_CLEAR", 1)[1].split("AI_KEY_WIFI_RECORD", 1)[0]
        self.assertIn("esp_rmaker_system_ctrl_factory_reset(0, 0, stop_sta_and_clear_wifi)", clear)
        self.assertNotIn("esp_restart", clear)
        self.assertNotIn("nvs_flash_erase", clear)
        helper = source.split("static esp_rmaker_error_t stop_sta_and_clear_wifi", 1)[1].split("\nstatic ", 1)[0]
        self.assertLess(helper.index("esp_wifi_stop"), helper.index("WIFI_STORAGE_FLASH"))
        self.assertLess(helper.index("WIFI_STORAGE_FLASH"), helper.index("esp_wifi_set_config"))
        self.assertLess(helper.index("esp_wifi_set_config"), helper.index("app_network_reset_credentials"))
        self.assertNotIn("nvs_flash_erase", helper)

    def test_qr_skip_leaves_provisioning_running(self):
        onboard = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        screen = (Path(__file__).parents[2] / "app_hello" / "provision_screen.c").read_text(encoding="utf-8")
        hello = (Path(__file__).parents[2] / "app_hello" / "app_hello.c").read_text(encoding="utf-8")
        handler = onboard.split("static void on_network_event", 1)[1].split("\nstatic void", 1)[0]
        self.assertIn("app_hello_offer_provision", handler)
        self.assertNotIn("app_network_reset_credentials", handler)
        self.assertNotIn("esp_restart", handler)
        self.assertNotIn("esp_wifi_restore", screen)
        self.assertNotIn("nvs_flash_erase", screen)
        self.assertIn("save_wifi_from_screen", onboard)
        self.assertIn("on_wifi_0", hello)
        self.assertNotIn("app_network_reset_credentials", hello)

    def test_wifi_success_closes_the_qr(self):
        onboard = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        hello = (Path(__file__).parents[2] / "app_hello" / "app_hello.c").read_text(encoding="utf-8")
        handler = onboard.split("static void on_network_event", 1)[1].split("\nstatic void", 1)[0]
        self.assertIn("NETWORK_PROV_WIFI_CRED_SUCCESS", handler)
        self.assertIn("app_hello_finish_provision", handler)
        self.assertNotIn("app_network_reset_credentials", handler)
        self.assertNotIn("esp_restart", handler)
        self.assertIn("provision_screen_complete", hello)

    def test_params_use_home_text_controls(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        self.assertIn("esp.device.recorder", source)
        self.assertIn("esp_rmaker_param_add_ui_type", source)
        self.assertIn("recorder_param_ui_type", source)
        self.assertIn('strcmp(val.val.s, "invoke")', source)
        self.assertIn("esp_rmaker_str(state->recordings_json)", source)
        self.assertNotIn("esp_rmaker_array", source)

    def test_node_config_is_reported_after_cloud_setup(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        snapshot = source.split("static void on_recorder_snapshot", 1)[1].split("\nstatic ", 1)[0]
        report = source.split("static void node_config_report_task", 1)[1].split("\nstatic ", 1)[0]
        self.assertIn("recorder node started", onboard)
        self.assertNotIn("vTaskDelay(pdMS_TO_TICKS(15000))", onboard)
        self.assertIn("node_config_report_task", onboard)
        self.assertIn("12288", onboard)
        self.assertIn("cloud_values_ready", snapshot)
        self.assertIn("esp_rmaker_param_update(", snapshot)
        self.assertIn("esp_rmaker_param_update_and_report", snapshot)
        self.assertIn("presence follow-up", report)
        self.assertIn("esp_rmaker_report_node_config", report)
        self.assertIn("pdMS_TO_TICKS(20000)", report)

    def test_online_state_is_managed_by_sdk_without_manual_heartbeats(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        self.assertNotIn("online_heartbeat_task", source)
        self.assertNotIn("publish_online_heartbeat", source)
        self.assertNotIn("schedule_online_heartbeat", source)
        self.assertNotIn("online_state_report_task", source)
        self.assertNotIn("esp_rmaker_state_mark_for_update_online_for_node", source)
        self.assertNotIn("esp_rmaker_node_set_online", source)
        self.assertNotIn("rec_online", onboard)
        self.assertNotIn("rec_online", source)

        event = source.split("static void on_rmaker_event", 1)[1].split("\nstatic ", 1)[0]
        self.assertIn("RMAKER_EVENT_CORE_STARTED", event)

    def test_noisy_driver_logs_are_reduced_before_provisioning(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        self.assertIn('esp_log_level_set("wifi", ESP_LOG_ERROR)', onboard)
        self.assertIn('esp_log_level_set("present_te", ESP_LOG_ERROR)', onboard)
        self.assertLess(onboard.index('esp_log_level_set("wifi", ESP_LOG_ERROR)'),
                        onboard.index("app_network_provision("))
        self.assertLess(onboard.index('esp_log_level_set("present_te", ESP_LOG_ERROR)'),
                        onboard.index("app_network_provision("))

    def test_wifi_power_save_is_disabled_after_join(self):
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        handler = source.split("static void on_network_event", 1)[1].split("\nstatic void", 1)[0]
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        self.assertIn("IP_EVENT_STA_GOT_IP", handler)
        self.assertIn("esp_wifi_set_ps(WIFI_PS_NONE)", handler)
        self.assertLess(onboard.index("esp_wifi_set_ps(WIFI_PS_NONE)"),
                        onboard.index("esp_rmaker_start()"))

    def test_rainmaker_cloud_client_can_be_switched_off(self):
        """The RainMaker MQTT client must be able to stay down.

        Its reconnect loop takes a hardware SHA context out of the internal DMA
        pool on every TLS handshake. The I2S microphone buffers need that pool,
        so with the client running the recording fails with "Microphone
        unavailable". The switch keeps the cloud code in the tree so it can be
        turned back on, while the Wi-Fi path stays independent of it.
        """
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        self.assertIn("#define RAINMAKER_CLOUD_ENABLED 0", source)
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        # The cloud start must sit behind the switch, not be removed.
        self.assertIn("if (RAINMAKER_CLOUD_ENABLED) {", onboard)
        self.assertLess(onboard.index("if (RAINMAKER_CLOUD_ENABLED) {"),
                        onboard.index("esp_rmaker_start()"))
        # The observer must not touch the shadow when the cloud is off.
        snapshot = source.split("static void on_recorder_snapshot", 1)[1].split("\nstatic ", 1)[0]
        self.assertIn("if (!RAINMAKER_CLOUD_ENABLED) {", snapshot)
        # Wi-Fi must not depend on the cloud switch.
        self.assertIn("app_network_provision(", onboard)
        self.assertIn("app_network_reset_credentials", source)
        self.assertIn("esp_wifi_connect", source)

    def test_provisioning_bring_up_is_behind_the_cloud_switch(self):
        """The provisioning bring-up must not start the RainMaker MQTT client.

        app_network_provision() calls esp_rmaker_pre_prov_init(), which starts
        the RainMaker MQTT client through __init_common_components(). That
        client keeps reconnecting to a broker this device does not use, and
        every TLS handshake takes a hardware SHA context out of the internal
        DMA pool. The I2S microphone buffers need that pool, so with the
        client running the recording fails with "Microphone unavailable".
        """
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        onboard = source.split("static void onboard_task", 1)[1].split("\nvoid app_onboard_start", 1)[0]
        # The provisioning call must sit inside the switch, not run before it.
        switch = onboard.index("if (RAINMAKER_CLOUD_ENABLED) {")
        provision = onboard.index("app_network_provision(")
        self.assertLess(switch, provision)
        # The switch block must close before the else branch starts the
        # station, so the provisioning call can never run with the cloud off.
        else_branch = onboard.index("} else {", switch)
        self.assertLess(provision, else_branch)
        self.assertIn("start_wifi_only()", onboard)
        self.assertLess(else_branch, onboard.index("start_wifi_only()"))

    def test_wifi_only_start_keeps_the_reconnect_behaviour(self):
        """The Wi-Fi-only path has to reconnect the station after a drop.

        rmaker_app_network registers its own handler on the already
        provisioned branch, but that branch is never reached when the
        provisioning bring-up is skipped. The app has to register its own
        handler and keep honouring the app_wifi_reconnect_enabled() gate,
        which goes false while the on-screen scan or join holds the radio.
        """
        source = (Path(__file__).parent.parent / "app_onboard.c").read_text(encoding="utf-8")
        self.assertIn("static bool start_wifi_only(void)", source)
        helper = source.split("static bool start_wifi_only(void)", 1)[1].split("\nstatic ", 1)[0]
        self.assertIn("esp_wifi_start", helper)
        self.assertIn("WIFI_MODE_STA", helper)
        self.assertIn("esp_event_handler_register(WIFI_EVENT", helper)
        # The provisioning bring-up must not be reachable from the helper.
        self.assertNotIn("app_network_provision(", helper)
        self.assertNotIn("esp_rmaker_pre_prov_init", helper)
        # The helper has to install a handler that reconnects the station.
        handler = source.split("static void wifi_reconnect_event_handler", 1)[1].split("\nstatic ", 1)[0]
        self.assertIn("esp_wifi_connect", handler)
        self.assertIn("WIFI_EVENT_STA_DISCONNECTED", handler)
        self.assertIn("wifi_reconnect_event_handler", helper)
        # The reconnect gate has to be honoured by the reconnect handler.
        self.assertIn("app_wifi_reconnect_enabled()", handler)
        self.assertLess(handler.index("app_wifi_reconnect_enabled()"),
                        handler.index("esp_wifi_connect"))


if __name__ == "__main__":
    unittest.main()
