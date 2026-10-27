import logging
import os
import smtplib
from email.message import EmailMessage
from urllib.parse import urlparse

import httpx

log = logging.getLogger("alerts.notify")


def format_message(
    state: str, rule_name: str, host: str, metric: str, value: float | None, severity: str
) -> str:
    shown = "no data" if value is None else f"{value:.1f}"
    if state == "firing":
        icon = "🔥" if severity == "critical" else "⚠️"
        return f"{icon} {rule_name} on {host}: {metric} = {shown} (rule: {rule_name})"
    return f"✅ Resolved: {rule_name} on {host}: {metric} = {shown}"


def webhook_payload(url: str, text: str) -> dict:
    """Slack incoming webhooks take {"text"}; Discord (and others) take {"content"}."""
    host = urlparse(url).hostname or ""
    return {"text": text} if host.endswith("slack.com") else {"content": text}


def send_webhook(url: str, text: str, client: httpx.Client | None = None) -> bool:
    try:
        owns = client is None
        client = client or httpx.Client(timeout=5.0)
        try:
            r = client.post(url, json=webhook_payload(url, text))
        finally:
            if owns:
                client.close()
        r.raise_for_status()
        return True
    except httpx.HTTPError as exc:
        log.warning("webhook delivery failed: %s", type(exc).__name__)  # URL is a secret
        return False


def send_email(text: str, subject: str = "Meridian Core alert") -> bool:
    host = os.environ.get("SMTP_HOST")
    to = os.environ.get("SMTP_TO")
    if not host or not to:
        return False
    msg = EmailMessage()
    msg["Subject"], msg["To"] = subject, to
    msg["From"] = os.environ.get("SMTP_FROM", os.environ.get("SMTP_USER", "meridian@localhost"))
    msg.set_content(text)
    try:
        with smtplib.SMTP(host, int(os.environ.get("SMTP_PORT", "587")), timeout=10) as smtp:
            smtp.starttls()
            if os.environ.get("SMTP_USER"):
                smtp.login(os.environ["SMTP_USER"], os.environ.get("SMTP_PASSWORD", ""))
            smtp.send_message(msg)
        return True
    except (OSError, smtplib.SMTPException) as exc:
        log.warning("email delivery failed: %s", type(exc).__name__)
        return False


def notify(text: str, rule_url: str | None) -> None:
    url = rule_url or os.environ.get("DISCORD_WEBHOOK_URL") or None
    if url:
        send_webhook(url, text)
    send_email(text)
