import base64
import hashlib
import hmac
import time

import pytest
from django.urls import reverse

from webhook_ops.webhooks.ingestion import ingest_event
from webhook_ops.webhooks.models import AuditLog, WebhookEvent, WebhookSource
from webhook_ops.webhooks.signatures import verify_source_signature


@pytest.mark.django_db
def test_ingest_event_deduplicates_by_provider_event_id():
    source = WebhookSource.objects.create(
        name="GitHub",
        slug="github",
        provider=WebhookSource.Provider.GITHUB,
    )
    headers = {
        "x-github-delivery": "delivery-123",
        "x-github-event": "push",
    }

    first = ingest_event(source, headers, b'{"ref": "main"}')
    second = ingest_event(source, headers, b'{"ref": "main"}')

    assert first.created is True
    assert second.created is False
    assert first.event.id == second.event.id
    assert WebhookEvent.objects.count() == 1
    assert AuditLog.objects.filter(action="webhook.duplicate").count() == 1


def test_github_signature_verification_accepts_valid_signature():
    source = WebhookSource(
        provider=WebhookSource.Provider.GITHUB,
        signing_secret="secret",
    )
    body = b'{"ok": true}'
    signature = hmac.new(b"secret", body, hashlib.sha256).hexdigest()

    result = verify_source_signature(
        source,
        {"x-hub-signature-256": f"sha256={signature}"},
        body,
    )

    assert result.ok is True


def test_github_signature_verification_rejects_invalid_signature():
    source = WebhookSource(
        provider=WebhookSource.Provider.GITHUB,
        signing_secret="secret",
    )

    result = verify_source_signature(
        source,
        {"x-hub-signature-256": "sha256=wrong"},
        b'{"ok": true}',
    )

    assert result.ok is False


def test_generic_signature_accepts_valid_and_rejects_others():
    source = WebhookSource(
        provider=WebhookSource.Provider.GENERIC,
        signing_secret="secret",
    )
    body = b'{"a": 1}'
    sig = hmac.new(b"secret", body, hashlib.sha256).hexdigest()

    assert verify_source_signature(
        source, {"x-webhookops-signature": f"sha256={sig}"}, body
    ).ok
    assert not verify_source_signature(
        source, {"x-webhookops-signature": "sha256=wrong"}, body
    ).ok
    assert not verify_source_signature(source, {}, body).ok


def test_shopify_signature_accepts_valid_base64_hmac():
    source = WebhookSource(
        provider=WebhookSource.Provider.SHOPIFY,
        signing_secret="secret",
    )
    body = b'{"order_id": 1}'
    digest = hmac.new(b"secret", body, hashlib.sha256).digest()
    sig = base64.b64encode(digest).decode()

    assert verify_source_signature(
        source, {"x-shopify-hmac-sha256": sig}, body
    ).ok
    assert not verify_source_signature(
        source, {"x-shopify-hmac-sha256": "AAAA"}, body
    ).ok
    assert not verify_source_signature(source, {}, body).ok


def test_slack_signature_accepts_fresh_and_rejects_stale():
    source = WebhookSource(
        provider=WebhookSource.Provider.SLACK,
        signing_secret="slack-secret",
    )
    body = b"payload=hi"
    fresh_ts = str(int(time.time()))
    fresh_sig = hmac.new(
        b"slack-secret",
        f"v0:{fresh_ts}:".encode() + body,
        hashlib.sha256,
    ).hexdigest()

    assert verify_source_signature(
        source,
        {"x-slack-request-timestamp": fresh_ts, "x-slack-signature": f"v0={fresh_sig}"},
        body,
    ).ok

    stale_ts = str(int(time.time()) - 60 * 10)
    stale_sig = hmac.new(
        b"slack-secret",
        f"v0:{stale_ts}:".encode() + body,
        hashlib.sha256,
    ).hexdigest()
    stale = verify_source_signature(
        source,
        {"x-slack-request-timestamp": stale_ts, "x-slack-signature": f"v0={stale_sig}"},
        body,
    )
    assert not stale.ok
    assert "stale" in stale.reason

    assert not verify_source_signature(
        source,
        {"x-slack-request-timestamp": fresh_ts, "x-slack-signature": "v0=wrong"},
        body,
    ).ok
    assert not verify_source_signature(source, {}, body).ok


def test_stripe_signature_accepts_any_matching_v1_and_rejects_stale():
    source = WebhookSource(
        provider=WebhookSource.Provider.STRIPE,
        signing_secret="stripe-secret",
    )
    body = b'{"id": "evt_1"}'
    fresh_ts = str(int(time.time()))
    fresh_sig = hmac.new(
        b"stripe-secret",
        f"{fresh_ts}.".encode() + body,
        hashlib.sha256,
    ).hexdigest()

    assert verify_source_signature(
        source,
        {"stripe-signature": f"t={fresh_ts},v1={fresh_sig}"},
        body,
    ).ok

    multi = verify_source_signature(
        source,
        {"stripe-signature": f"t={fresh_ts},v1=oldsig,v1={fresh_sig}"},
        body,
    )
    assert multi.ok

    stale_ts = str(int(time.time()) - 60 * 10)
    stale_sig = hmac.new(
        b"stripe-secret",
        f"{stale_ts}.".encode() + body,
        hashlib.sha256,
    ).hexdigest()
    stale = verify_source_signature(
        source,
        {"stripe-signature": f"t={stale_ts},v1={stale_sig}"},
        body,
    )
    assert not stale.ok
    assert "stale" in stale.reason

    assert not verify_source_signature(
        source,
        {"stripe-signature": f"t={fresh_ts},v1=deadbeef"},
        body,
    ).ok
    assert not verify_source_signature(
        source, {"stripe-signature": "garbage"}, body
    ).ok
    assert not verify_source_signature(source, {}, body).ok


@pytest.mark.django_db
def test_ingest_view_rejects_invalid_signature(client):
    source = WebhookSource.objects.create(
        name="GitHub",
        slug="github",
        provider=WebhookSource.Provider.GITHUB,
        signing_secret="secret",
    )

    response = client.post(
        reverse("webhook-ingest", kwargs={"source_slug": source.slug}),
        data=b'{"ref": "main"}',
        content_type="application/json",
        HTTP_X_HUB_SIGNATURE_256="sha256=wrong",
    )

    assert response.status_code == 401
    assert WebhookEvent.objects.count() == 0
    assert AuditLog.objects.filter(action="webhook.signature_rejected").exists()
