from unittest.mock import Mock, patch

import pytest

from webhook_ops.webhooks.delivery import deliver_event
from webhook_ops.webhooks.models import DeliveryAttempt, Destination, WebhookEvent, WebhookSource


@pytest.mark.django_db
def test_deliver_event_marks_successful_public_http_delivery():
    destination = Destination.objects.create(
        name="App",
        slug="app",
        url="https://app.example.test/webhooks",
    )
    source = WebhookSource.objects.create(
        name="Stripe",
        slug="stripe",
        provider=WebhookSource.Provider.STRIPE,
        default_destination=destination,
    )
    event = WebhookEvent.objects.create(
        source=source,
        destination=destination,
        idempotency_key="evt_123",
        status=WebhookEvent.Status.QUEUED,
        headers={"content-type": "application/json"},
        payload={"id": "evt_123"},
        raw_body='{"id": "evt_123"}',
        body_sha256="abc",
    )
    response = Mock(status_code=204, headers={}, text="")

    with patch("webhook_ops.webhooks.delivery.requests.post", return_value=response) as post:
        result = deliver_event(event.id)

    event.refresh_from_db()
    attempt = DeliveryAttempt.objects.get(event=event)
    assert result.delivered is True
    assert event.status == WebhookEvent.Status.DELIVERED
    assert attempt.status == DeliveryAttempt.Status.SUCCEEDED
    post.assert_called_once()
    assert post.call_args.kwargs["data"] == b'{"id": "evt_123"}'
    assert post.call_args.kwargs["headers"]["x-webhookops-attempt"] == "1"


@pytest.mark.django_db
def test_deliver_event_marks_failed_delivery_for_retry():
    destination = Destination.objects.create(
        name="App",
        slug="app",
        url="https://app.example.test/webhooks",
    )
    source = WebhookSource.objects.create(
        name="Generic",
        slug="generic",
        default_destination=destination,
    )
    event = WebhookEvent.objects.create(
        source=source,
        destination=destination,
        idempotency_key="evt_456",
        status=WebhookEvent.Status.QUEUED,
        headers={"content-type": "application/json"},
        payload={"id": "evt_456"},
        raw_body='{"id": "evt_456"}',
        body_sha256="def",
    )
    response = Mock(status_code=500, headers={}, text="server error")

    with patch("webhook_ops.webhooks.delivery.requests.post", return_value=response):
        result = deliver_event(event.id)

    event.refresh_from_db()
    attempt = DeliveryAttempt.objects.get(event=event)
    assert result.delivered is False
    assert result.retry_after_seconds == 5
    assert event.status == WebhookEvent.Status.FAILED
    assert event.attempt_count == 1
    assert "HTTP 500" in event.last_error
    assert attempt.status == DeliveryAttempt.Status.FAILED
