import base64
import hashlib
import hmac
import time
from dataclasses import dataclass

from .models import WebhookSource


@dataclass(frozen=True)
class SignatureResult:
    ok: bool
    reason: str = ""


def _header(headers: dict[str, str], name: str) -> str:
    return headers.get(name.lower(), "")


def _hmac_hex(secret: str, message: bytes) -> str:
    return hmac.new(secret.encode(), message, hashlib.sha256).hexdigest()


def _hmac_base64(secret: str, message: bytes) -> str:
    digest = hmac.new(secret.encode(), message, hashlib.sha256).digest()
    return base64.b64encode(digest).decode()


def _constant_time_signature(value: str, expected: str) -> bool:
    return hmac.compare_digest(value.strip(), expected.strip())


def _verify_generic(secret: str, headers: dict[str, str], raw_body: bytes) -> SignatureResult:
    signature = _header(headers, "x-webhookops-signature")
    expected = f"sha256={_hmac_hex(secret, raw_body)}"
    if not signature:
        return SignatureResult(False, "missing x-webhookops-signature")
    if not _constant_time_signature(signature, expected):
        return SignatureResult(False, "invalid generic signature")
    return SignatureResult(True)


def _verify_github(secret: str, headers: dict[str, str], raw_body: bytes) -> SignatureResult:
    signature = _header(headers, "x-hub-signature-256")
    expected = f"sha256={_hmac_hex(secret, raw_body)}"
    if not signature:
        return SignatureResult(False, "missing x-hub-signature-256")
    if not _constant_time_signature(signature, expected):
        return SignatureResult(False, "invalid github signature")
    return SignatureResult(True)


def _verify_shopify(secret: str, headers: dict[str, str], raw_body: bytes) -> SignatureResult:
    signature = _header(headers, "x-shopify-hmac-sha256")
    expected = _hmac_base64(secret, raw_body)
    if not signature:
        return SignatureResult(False, "missing x-shopify-hmac-sha256")
    if not _constant_time_signature(signature, expected):
        return SignatureResult(False, "invalid shopify signature")
    return SignatureResult(True)


def _verify_slack(secret: str, headers: dict[str, str], raw_body: bytes) -> SignatureResult:
    timestamp = _header(headers, "x-slack-request-timestamp")
    signature = _header(headers, "x-slack-signature")
    if not timestamp or not signature:
        return SignatureResult(False, "missing slack signature headers")
    try:
        timestamp_int = int(timestamp)
    except ValueError:
        return SignatureResult(False, "invalid slack timestamp")
    if abs(time.time() - timestamp_int) > 60 * 5:
        return SignatureResult(False, "stale slack timestamp")
    signed_payload = f"v0:{timestamp}:".encode() + raw_body
    expected = f"v0={_hmac_hex(secret, signed_payload)}"
    if not _constant_time_signature(signature, expected):
        return SignatureResult(False, "invalid slack signature")
    return SignatureResult(True)


def _verify_stripe(secret: str, headers: dict[str, str], raw_body: bytes) -> SignatureResult:
    header_value = _header(headers, "stripe-signature")
    if not header_value:
        return SignatureResult(False, "missing stripe-signature")

    parts: dict[str, list[str]] = {}
    for item in header_value.split(","):
        key, _, value = item.partition("=")
        if key and value:
            parts.setdefault(key, []).append(value)

    timestamp = parts.get("t", [""])[0]
    signatures = parts.get("v1", [])
    if not timestamp or not signatures:
        return SignatureResult(False, "invalid stripe-signature format")

    try:
        timestamp_int = int(timestamp)
    except ValueError:
        return SignatureResult(False, "invalid stripe timestamp")
    if abs(time.time() - timestamp_int) > 60 * 5:
        return SignatureResult(False, "stale stripe timestamp")

    signed_payload = f"{timestamp}.".encode() + raw_body
    expected = _hmac_hex(secret, signed_payload)
    if not any(_constant_time_signature(value, expected) for value in signatures):
        return SignatureResult(False, "invalid stripe signature")
    return SignatureResult(True)


def verify_source_signature(
    source: WebhookSource,
    headers: dict[str, str],
    raw_body: bytes,
) -> SignatureResult:
    if not source.signing_secret:
        return SignatureResult(True)

    verifiers = {
        WebhookSource.Provider.GENERIC: _verify_generic,
        WebhookSource.Provider.GITHUB: _verify_github,
        WebhookSource.Provider.SHOPIFY: _verify_shopify,
        WebhookSource.Provider.SLACK: _verify_slack,
        WebhookSource.Provider.STRIPE: _verify_stripe,
    }
    verifier = verifiers.get(source.provider, _verify_generic)
    return verifier(source.signing_secret, headers, raw_body)
