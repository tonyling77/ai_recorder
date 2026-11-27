import json
import unittest

from rainmaker import publish_summary


class Response:
    def __init__(self, payload):
        self.payload = json.dumps(payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def read(self):
        return self.payload


class RainMakerPublisherTest(unittest.TestCase):
    def test_summary_is_merged_into_custom_user_data(self):
        requests = []

        def opener(request):
            requests.append(request)
            if request.method == "GET":
                return Response(
                    {
                        "aiRecorderSummaries": {
                            "value": [
                                {
                                    "id": "old",
                                    "nodeId": "node-1",
                                    "sessionId": 1,
                                    "title": "旧纪要",
                                    "markdown": "# 旧纪要",
                                    "createdAt": "2026-11-07T00:00:00Z",
                                }
                            ]
                        }
                    }
                )
            return Response({"status": "success"})

        publish_summary(
            access_token="secret-token",
            node_id="node-1",
            session_id=2,
            title="新纪要",
            markdown="# 新纪要\n\n无明确待办。\n",
            created_at="2026-11-08T00:00:00Z",
            opener=opener,
        )

        self.assertEqual([request.method for request in requests], ["GET", "PUT"])
        self.assertEqual(requests[0].get_header("Authorization"), "secret-token")
        payload = json.loads(requests[1].data)
        summaries = payload["aiRecorderSummaries"]["value"]
        self.assertEqual([item["sessionId"] for item in summaries], [1, 2])
        self.assertNotIn("secret-token", requests[1].data.decode())

    def test_oversized_custom_data_is_rejected_before_upload(self):
        methods = []

        def opener(request):
            methods.append(request.method)
            return Response({})

        with self.assertRaisesRegex(RuntimeError, "399 KB"):
            publish_summary(
                access_token="secret-token",
                node_id="node-1",
                session_id=2,
                title="大纪要",
                markdown="x" * 410_000,
                created_at="2026-11-08T00:00:00Z",
                opener=opener,
            )
        self.assertEqual(methods, ["GET"])


if __name__ == "__main__":
    unittest.main()
