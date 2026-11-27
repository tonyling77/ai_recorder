"""The private channel over plain HTTP, to the inbox on the developer's machine.

The endpoint itself is checked by test_recorder_upload_lan.c, which is the
same code built with an http:// origin. What is left for a policy test is
the part around it: which client the request builds, and whether the device
waits for a wall clock before it talks to the server.

The clock matters here. The certificate bundle checks the validity window of
the server certificate against the system clock, so a device without a wall
clock cannot do the handshake. That reason belongs to TLS only: a plain
HTTP link has no handshake and no certificate, so a board on a network
without a route to the internet, where SNTP never answers, can still reach
the inbox on the developer's own machine.
"""

import unittest
from pathlib import Path


RECORDER_ROOT = Path(__file__).parent.parent
PROJECT_ROOT = RECORDER_ROOT.parents[1]
HELLO_ROOT = PROJECT_ROOT / "components" / "app_hello"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _body(text: str, start: str, end: str) -> str:
    """Return the source slice between two markers."""
    return text.split(start, 1)[1].split(end, 1)[0]


class LanUploadPolicyTest(unittest.TestCase):
    """The slice upload has to work over a plain link."""

    def test_the_post_helper_accepts_a_plain_link(self):
        """The old check refused every endpoint without a certificate.

        That was right for the tunnel link and wrong for the inbox on the
        developer's own machine, where the recording travels in plain text
        inside their own network."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        post = _body(source, "static int post_request(", "\n/* The recordings tree")
        self.assertIn("endpoint.plain_http", post)
        self.assertIn("!endpoint.plain_http &&", post)

    def test_the_certificate_bundle_stays_on_a_tunnel_link(self):
        """The bundle is what makes the tunnel link check the certificate."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        post = _body(source, "static int post_request(", "\n/* The recordings tree")
        self.assertIn(".crt_bundle_attach = endpoint.plain_http ? NULL : esp_crt_bundle_attach,", post)

    def test_the_upload_does_not_wait_for_a_clock_on_a_plain_link(self):
        """A plain link has no handshake, so it does not need a wall clock."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        helper = _body(source, "static int uplink_clock_ready(void)", "\nint recorder_uplink_send_pending")
        self.assertIn("endpoint.plain_http", helper)
        # The wait itself stays for the link that needs it.
        self.assertIn("while (!app_clock_valid())", helper)
        self.assertIn("UPLINK_CLOCK_WAIT_MS", helper)

    def test_the_clock_gate_stays_in_front_of_the_request(self):
        """The gate has to run before anything goes out."""
        source = _read(RECORDER_ROOT / "recorder_uplink.c")
        send = _body(source, "int recorder_uplink_send_pending(", "\nconst char *recorder_uplink_failure")
        self.assertLess(send.index("uplink_clock_ready()"), send.index("recorder_upload_prepare("))
        self.assertLess(send.index("uplink_clock_ready()"), send.index("post_request("))


class LanNotesPolicyTest(unittest.TestCase):
    """The minutes have to come back over a plain link too."""

    def test_the_download_keeps_the_bundle_off_a_plain_link(self):
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn(".crt_bundle_attach = endpoint.plain_http ? NULL : esp_crt_bundle_attach,", download)

    def test_the_download_does_not_wait_for_a_clock_on_a_plain_link(self):
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertIn("!endpoint.plain_http && wait_for_clock()", download)

    def test_the_endpoint_is_built_before_the_clock_is_asked_for(self):
        """The download cannot know the scheme of the link before it has the
        endpoint, so the order is: endpoint, then the clock."""
        source = _read(HELLO_ROOT / "meeting_notes_sync.c")
        download = _body(source, "static int download(char *out, size_t cap)", "\nstatic void sync_task")
        self.assertLess(download.index("recorder_upload_endpoint("),
                        download.index("wait_for_clock()"))


class LanBuildPolicyTest(unittest.TestCase):
    """The build has to accept a plain origin."""

    def test_the_build_accepts_an_http_origin(self):
        """The build refused anything that was not https, so the plain inbox
        could not even be configured."""
        source = _read(RECORDER_ROOT / "CMakeLists.txt")
        self.assertIn('"^https://"', source)
        self.assertIn('"^http://"', source)

    def test_the_build_still_refuses_an_origin_that_is_not_a_base_url(self):
        """A plain origin is a base URL as well: no path, no query, no
        trailing slash, and short enough for the firmware buffer."""
        source = _read(RECORDER_ROOT / "CMakeLists.txt")
        self.assertIn("_upload_origin MATCHES \"/$\"", source)
        self.assertIn("_upload_origin MATCHES \"\\\\?\"", source)
        self.assertIn("_upload_origin_len GREATER 80", source)

    def test_the_build_needs_no_secrets(self):
        """A developer who runs the inbox on their own machine has to be able
        to build the firmware without a .env.

        The origin is learned at runtime, so the build only carries the
        fallback. A build that stops on an empty token or origin would make
        the board impossible to flash for anyone but the person who filled
        the file in."""
        source = _read(RECORDER_ROOT / "CMakeLists.txt")
        self.assertNotIn("UPLOAD_TOKEN is empty", source)
        self.assertNotIn("UPLOAD_ORIGIN is empty", source)

    def test_the_fallback_is_a_name(self):
        """The fallback has to be a name, so the address of the machine may
        change without the board losing the inbox."""
        source = _read(RECORDER_ROOT / "CMakeLists.txt")
        self.assertIn("http://inbox.local:8765", source)


class LearnedOriginPolicyTest(unittest.TestCase):
    """The origin that was learned comes first."""

    def test_the_endpoint_prefers_the_learned_origin(self):
        source = _read(RECORDER_ROOT / "recorder_upload.c")
        endpoint = _body(source, "int recorder_upload_endpoint(", "\nvoid recorder_upload_result")
        self.assertIn("recorder_inbox_get(", endpoint)
        self.assertIn("learned[0] ? learned : RECORDER_UPLOAD_ORIGIN", endpoint)

    def test_the_learned_origin_is_kept_in_nvs(self):
        """The address has to survive a restart, or the board would need the
        host every time it boots."""
        source = _read(RECORDER_ROOT / "recorder_inbox.c")
        self.assertIn("nvs_set_str(", source)
        self.assertIn("nvs_get_str(", source)
        self.assertIn("nvs_commit(", source)

    def test_the_learned_origin_is_a_base_url(self):
        """Anything that is not a base URL would break the request."""
        source = _read(RECORDER_ROOT / "recorder_inbox.c")
        body = _body(source, "static int origin_is_usable(", "\nint recorder_inbox_get")
        self.assertIn('"http://"', body)
        self.assertIn('"https://"', body)
        self.assertIn("strchr(authority, '/')", body)

    def test_the_host_may_point_the_board_at_an_inbox(self):
        """The developer flashes the board and runs the inbox on their own
        machine, so the host has to be able to tell the board where it is."""
        source = _read(RECORDER_ROOT.parent / "app_onboard" / "inbox_config_rpc.c")
        self.assertIn("esp_iris_rpc_register(", source)
        self.assertIn("recorder_inbox_set(", source)


if __name__ == "__main__":
    unittest.main()
