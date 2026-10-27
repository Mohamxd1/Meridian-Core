import httpx

from alerts import notifiers


def test_payload_shapes():
    assert notifiers.webhook_payload("https://hooks.slack.com/services/x", "hi") == {"text": "hi"}
    assert notifiers.webhook_payload("https://discord.com/api/webhooks/x", "hi") == {
        "content": "hi"
    }


def test_message_text():
    m = notifiers.format_message("firing", "GPU hot", "pc", "gpu.temp_c", 87.0, "critical")
    assert "GPU hot on pc" in m and "87.0" in m
    assert notifiers.format_message(
        "resolved", "GPU hot", "pc", "gpu.temp_c", 70, "critical"
    ).startswith("✅")


def test_send_webhook_success_and_failure():
    ok = httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(204)))
    bad = httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(500)))
    assert notifiers.send_webhook("https://discord.com/x", "t", ok)
    assert not notifiers.send_webhook("https://discord.com/x", "t", bad)
