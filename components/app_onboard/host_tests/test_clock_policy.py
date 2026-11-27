import re
import unittest
from pathlib import Path


ONBOARD_ROOT = Path(__file__).parent.parent
PROJECT_ROOT = ONBOARD_ROOT.parents[1]
CLOCK_ROOT = PROJECT_ROOT / "components" / "app_clock"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class ClockSourcePolicyTest(unittest.TestCase):
    """The board needs a wall clock, and it must not come from RainMaker.

    The RainMaker MQTT client used to carry the time sync. Commit e140853
    stopped the client, because its endless TLS reconnect loop drained the
    internal DMA pool and broke the microphone, but that also took the only
    clock source with it. Without a wall clock the certificate bundle cannot
    validate a server certificate and recorder_stamp_now() cannot produce a
    timestamp. A plain lwIP SNTP client replaces that clock source.
    """

    def test_the_clock_component_exists(self):
        self.assertTrue((CLOCK_ROOT / "app_clock.c").is_file())
        self.assertTrue((CLOCK_ROOT / "include" / "app_clock.h").is_file())

    def test_an_sntp_client_is_started_somewhere_in_the_firmware(self):
        """Something has to ask a time server for the wall clock."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        self.assertIn("esp_netif_sntp_init", clock)
        self.assertIn("sntp", clock.lower())

    def test_the_client_polls_a_time_server(self):
        """The lwIP client has to run in poll mode against a named server.

        The netif client sets the poll mode and the server name on the lwIP
        SNTP client before it starts it, so the configuration has to name a
        server and start the client.
        """
        clock = _read(CLOCK_ROOT / "app_clock.c")
        task = _body(clock, "static void clock_task(void *arg)", "\nvoid app_clock_start")
        self.assertIn("ESP_NETIF_SNTP_DEFAULT_CONFIG", task)
        self.assertIn("config.start = true", task)
        # The server is a named host, not an address.
        self.assertRegex(clock, r'APP_CLOCK_SNTP_SERVER\s+"[a-z0-9.]+"')

    def test_the_server_is_reachable_from_the_board_network(self):
        """The board is in China, so the pool has to be one that answers."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        self.assertRegex(clock, r'#define\s+APP_CLOCK_SNTP_SERVER\s+"[a-z0-9.]+"')
        server = re.search(r'#define\s+APP_CLOCK_SNTP_SERVER\s+"([^"]+)"', clock).group(1)
        # pool.ntp.org and its country pool are the two candidates.
        self.assertIn(server, ("pool.ntp.org", "cn.pool.ntp.org"))

    def test_the_sntp_client_is_not_the_rainmaker_one(self):
        """The RainMaker MQTT client must stay out of the binary.

        Its reconnect loop takes a hardware SHA context out of the internal
        DMA pool on every TLS handshake, and the I2S microphone buffers need
        that pool. With the client running the recording fails with
        "Microphone unavailable". The clock component is a new module, so it
        must never reach for the RainMaker time sync.
        """
        clock = _read(CLOCK_ROOT / "app_clock.c")
        for marker in ("esp_rmaker_time_sync",
                       "esp_rmaker_timezone_service_enable",
                       "esp_rmaker_node_init",
                       "esp_rmaker_start(",
                       "esp_rmaker_pre_prov_init",
                       "app_network_provision("):
            self.assertNotIn(marker, clock, f"app_clock.c must not call {marker}")

    def test_the_rainmaker_time_sync_is_not_the_clock_source(self):
        """The RainMaker time sync went away with the MQTT client.

        esp_rmaker_node_init() was called with enable_time_sync and the
        timezone service was enabled, and those set up the RainMaker SNTP
        client. They sit behind the cloud switch now, so the switch has to
        keep them off.
        """
        onboard = _read(ONBOARD_ROOT / "app_onboard.c")
        self.assertIn("#define RAINMAKER_CLOUD_ENABLED 0", onboard)
        task = _body(onboard, "static void onboard_task", "\nvoid app_onboard_start")
        # The whole cloud block, including the node init and the timezone
        # service, has to sit behind the switch.
        switch = task.index("if (RAINMAKER_CLOUD_ENABLED) {")
        for marker in ("esp_rmaker_node_init", "esp_rmaker_timezone_service_enable", "esp_rmaker_start("):
            self.assertIn(marker, task, f"{marker} has to stay in the tree")
            self.assertLess(switch, task.index(marker), f"{marker} has to sit behind the cloud switch")

    def test_the_onboarding_never_starts_the_rainmaker_cloud(self):
        """The provisioning bring-up starts the RainMaker MQTT client."""
        onboard = _read(ONBOARD_ROOT / "app_onboard.c")
        task = _body(onboard, "static void onboard_task", "\nvoid app_onboard_start")
        self.assertIn("#define RAINMAKER_CLOUD_ENABLED 0", onboard)
        # The call has to stay behind the switch.
        self.assertLess(task.index("if (RAINMAKER_CLOUD_ENABLED) {"), task.index("app_network_provision("))

    def test_the_sntp_client_starts_after_the_station_has_an_address(self):
        """SNTP needs a route, so the client cannot start at boot."""
        onboard = _read(ONBOARD_ROOT / "app_onboard.c")
        handler = _body(onboard, "static void on_network_event", "\nstatic void add_name_only")
        self.assertIn("IP_EVENT_STA_GOT_IP", handler)
        self.assertIn("app_clock_start()", handler)

    def test_the_clock_start_never_blocks_the_wifi_join(self):
        """The clock runs on its own task, so the join is never held up."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        start = _body(clock, "void app_clock_start(void)", "\nbool app_clock_valid")
        self.assertIn("xTaskCreate", start)
        self.assertNotIn("xSemaphoreTake", start)
        self.assertNotIn("vTaskDelay", start)

    def test_the_client_retries_when_the_first_attempt_fails(self):
        """The board may join before the router or the internet is ready."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        task = _body(clock, "static void clock_task(void *arg)", "\nvoid app_clock_start")
        self.assertIn("while (true)", task)
        self.assertIn("esp_netif_sntp_init", task)
        # The failure path has to wait and try again.
        self.assertIn("APP_CLOCK_RETRY_MS", task)
        self.assertRegex(task, r"retrying|retry")

    def test_the_timezone_is_set_to_utc_plus_eight(self):
        """The recorded stamp has to be in local time."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        self.assertIn('setenv("TZ"', clock)
        self.assertIn("tzset()", clock)
        # CST-8 is the POSIX form of UTC+8: the POSIX sign is the offset west
        # of Greenwich.
        self.assertIn('"CST-8"', clock)

    def test_the_sync_completion_is_logged(self):
        """The clock state has to be visible in the logs."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        self.assertIn("esp_sntp_config_t", clock)
        self.assertIn("sync_cb", clock)
        self.assertIn("log_clock", clock)


class ClockVisibilityPolicyTest(unittest.TestCase):
    """The device has to report its clock.

    Without a log line there is no way to tell a missing clock from a dead
    network, and the whole failure looked silent.
    """

    def test_the_boot_sequence_logs_the_wall_clock(self):
        onboard = _read(ONBOARD_ROOT / "app_onboard.c")
        self.assertIn('app_clock_log("wall clock at boot")', onboard)

    def test_the_boot_sequence_logs_the_clock_again_at_the_ip(self):
        onboard = _read(ONBOARD_ROOT / "app_onboard.c")
        self.assertIn('app_clock_log("wall clock at IP")', onboard)

    def test_the_clock_helper_can_report_the_time(self):
        clock = _read(CLOCK_ROOT / "app_clock.c")
        self.assertIn("void app_clock_log(", clock)
        self.assertIn("ESP_LOGI", clock)

    def test_the_clock_validity_is_derived_from_the_wall_clock(self):
        """A clock that was never set reads the epoch."""
        clock = _read(CLOCK_ROOT / "app_clock.c")
        valid = _body(clock, "bool app_clock_valid(void)", "\nvoid app_clock_log")
        self.assertIn("time(NULL)", valid)
        self.assertIn("APP_CLOCK_MIN_YEAR", valid)


if __name__ == "__main__":
    unittest.main()
