import hashlib
import hmac

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
