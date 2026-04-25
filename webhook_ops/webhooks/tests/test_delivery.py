from unittest.mock import Mock, patch

import pytest
from django.contrib.auth import get_user_model
from django.urls import reverse

from webhook_ops.webhooks.delivery import deliver_event
from webhook_ops.webhooks.models import (
    Agent,
    DeliveryAttempt,
    Destination,
    ReplayRequest,
    WebhookEvent,
    WebhookSource,
)


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


@pytest.mark.django_db
def test_deliver_event_routes_private_agent_delivery_through_proxy(settings):
    settings.WEBHOOKOPS_PRIVATE_PROXY_URL = "http://127.0.0.1:9080"
    agent = Agent.objects.create(
        name="Dev machine",
        slug="dev-machine",
        status=Agent.Status.ONLINE,
    )
    destination = Destination.objects.create(
        name="Private app",
        slug="private-app",
        mode=Destination.DeliveryMode.PRIVATE_AGENT,
        url="http://localhost:8000/webhooks",
        agent=agent,
    )
    source = WebhookSource.objects.create(
        name="Generic",
        slug="generic",
        default_destination=destination,
    )
    event = WebhookEvent.objects.create(
        source=source,
        destination=destination,
        idempotency_key="evt_private",
        status=WebhookEvent.Status.QUEUED,
        headers={"content-type": "application/json"},
        payload={"id": "evt_private"},
        raw_body='{"id": "evt_private"}',
        body_sha256="private",
    )
    response = Mock(status_code=200, headers={}, text="ok")

    with patch("webhook_ops.webhooks.delivery.requests.post", return_value=response) as post:
        result = deliver_event(event.id)

    event.refresh_from_db()
    assert result.delivered is True
    assert event.status == WebhookEvent.Status.DELIVERED
    post.assert_called_once()
    assert post.call_args.args[0] == "http://localhost:8000/webhooks"
    assert post.call_args.kwargs["proxies"] == {
        "http": "http://127.0.0.1:9080",
        "https": "http://127.0.0.1:9080",
    }


@pytest.mark.django_db
def test_private_agent_delivery_requires_agent():
    destination = Destination.objects.create(
        name="Private app",
        slug="private-app",
        mode=Destination.DeliveryMode.PRIVATE_AGENT,
        url="http://localhost:8000/webhooks",
    )
    source = WebhookSource.objects.create(
        name="Generic",
        slug="generic",
        default_destination=destination,
    )
    event = WebhookEvent.objects.create(
        source=source,
        destination=destination,
        idempotency_key="evt_missing_agent",
        status=WebhookEvent.Status.QUEUED,
        headers={"content-type": "application/json"},
        payload={"id": "evt_missing_agent"},
        raw_body='{"id": "evt_missing_agent"}',
        body_sha256="missing-agent",
    )

    with patch("webhook_ops.webhooks.delivery.requests.post") as post:
        result = deliver_event(event.id)

    event.refresh_from_db()
    assert result.delivered is False
    assert result.retry_after_seconds == 5
    assert event.status == WebhookEvent.Status.FAILED
    assert "requires an agent" in event.last_error
    post.assert_not_called()


@pytest.mark.django_db
def test_successful_delivery_marks_pending_replay_completed():
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
        idempotency_key="evt_replay_done",
        status=WebhookEvent.Status.QUEUED,
        headers={"content-type": "application/json"},
        payload={"id": "evt_replay_done"},
        raw_body='{"id": "evt_replay_done"}',
        body_sha256="replay-done",
    )
    replay = ReplayRequest.objects.create(
        event=event,
        status=ReplayRequest.Status.QUEUED,
        reason="manual",
    )
    response = Mock(status_code=200, headers={}, text="ok")

    with patch("webhook_ops.webhooks.delivery.requests.post", return_value=response):
        deliver_event(event.id)

    replay.refresh_from_db()
    assert replay.status == ReplayRequest.Status.COMPLETED
    assert replay.processed_at is not None
    assert replay.error == ""


@pytest.mark.django_db
def test_replay_api_resets_event_and_queues_delivery(client):
    user = get_user_model().objects.create_superuser(
        username="admin",
        email="admin@example.test",
        password="password",
    )
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
        idempotency_key="evt_789",
        status=WebhookEvent.Status.DEAD_LETTERED,
        attempt_count=6,
        headers={"content-type": "application/json"},
        payload={"id": "evt_789"},
        raw_body='{"id": "evt_789"}',
        body_sha256="ghi",
        last_error="exhausted",
    )
    client.force_login(user)

    with patch("webhook_ops.webhooks.api.deliver_webhook_event.delay") as delay:
        response = client.post(
            reverse("events-replay", kwargs={"pk": event.id}),
            data={"reason": "fixed receiver"},
            content_type="application/json",
        )

    event.refresh_from_db()
    replay = ReplayRequest.objects.get(event=event)
    assert response.status_code == 202
    assert event.status == WebhookEvent.Status.QUEUED
    assert event.attempt_count == 0
    assert event.last_error == ""
    assert replay.status == ReplayRequest.Status.QUEUED
    assert replay.requested_by == user
    delay.assert_called_once_with(event.id)
