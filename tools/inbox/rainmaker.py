"""Publish sanitized final notes to RainMaker Custom User Data."""

from __future__ import annotations

import hashlib
import json
import urllib.error
import urllib.request


CUSTOM_DATA_KEY = "aiRecorderSummaries"
CUSTOM_DATA_LIMIT = 399 * 1024
DEFAULT_API_BASE = "https://api.rainmaker.espressif.com/v1"


def publish_summary(
    *,
    access_token: str,
    node_id: str,
    session_id: int,
    title: str,
    markdown: str,
    created_at: str,
    api_base: str = DEFAULT_API_BASE,
    opener=urllib.request.urlopen,
) -> None:
    if not access_token or not node_id or session_id < 0 or not title or not markdown or not created_at:
        raise ValueError("complete RainMaker summary settings are required")
    endpoint = f"{api_base.rstrip('/')}/user/custom_data"
    current = _request_json(endpoint, access_token, opener)
    custom_data = current.get("data", current) if isinstance(current, dict) else {}
    key_data = custom_data.get(CUSTOM_DATA_KEY, {}) if isinstance(custom_data, dict) else {}
    existing = key_data.get("value", []) if isinstance(key_data, dict) else []
    summaries = list(existing) if isinstance(existing, list) else []
    summaries = [
        item
        for item in summaries
        if not (
            isinstance(item, dict)
            and item.get("nodeId") == node_id
            and item.get("sessionId") == session_id
        )
    ]
    digest = hashlib.sha256(f"{node_id}:{session_id}:{markdown}".encode()).hexdigest()[:20]
    summaries.append(
        {
            "id": digest,
            "nodeId": node_id,
            "sessionId": session_id,
            "title": title,
            "markdown": markdown,
            "createdAt": created_at,
        }
    )
    payload = {
        CUSTOM_DATA_KEY: {
            "value": summaries,
            "perms": [{"read": ["user"], "write": ["user"]}],
        }
    }
    encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
    if len(encoded) > CUSTOM_DATA_LIMIT:
        raise RuntimeError("RainMaker Custom User Data would exceed 399 KB")
    request = urllib.request.Request(endpoint, data=encoded, method="PUT")
    request.add_header("Authorization", access_token)
    request.add_header("Content-Type", "application/json")
    try:
        with opener(request) as response:
            response.read()
    except (urllib.error.HTTPError, urllib.error.URLError, OSError) as error:
        raise RuntimeError(f"RainMaker summary upload failed: {type(error).__name__}") from error


def _request_json(endpoint: str, access_token: str, opener) -> dict:
    request = urllib.request.Request(endpoint, method="GET")
    request.add_header("Authorization", access_token)
    try:
        with opener(request) as response:
            raw = response.read()
    except (urllib.error.HTTPError, urllib.error.URLError, OSError) as error:
        raise RuntimeError(f"RainMaker summary fetch failed: {type(error).__name__}") from error
    try:
        parsed = json.loads(raw)
    except (json.JSONDecodeError, TypeError) as error:
        raise RuntimeError("RainMaker custom data returned invalid JSON") from error
    return parsed if isinstance(parsed, dict) else {}
