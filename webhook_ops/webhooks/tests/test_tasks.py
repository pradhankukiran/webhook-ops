from datetime import timedelta
from unittest.mock import patch

import pytest
from django.utils import timezone

from webhook_ops.webhooks.models import Destination, WebhookEvent, WebhookSource
from webhook_ops.webhooks.tasks import enqueue_due_webhook_events


def _make_event(source, destination, key, status, *, next_attempt_at):
    return WebhookEvent.objects.create(
        source=source,
        destination=destination,
        idempotency_key=key,
        status=status,
        next_attempt_at=next_attempt_at,
        headers={},
        payload={"id": key},
        raw_body='{"id": "x"}',
        body_sha256=key,
    )


@pytest.mark.django_db
def test_enqueue_due_only_picks_due_queued_or_failed_events():
    destination = Destination.objects.create(
        name="App", slug="app", url="https://app.example.test/webhooks"
    )
    source = WebhookSource.objects.create(
        name="Generic", slug="generic", default_destination=destination
    )
    now = timezone.now()
    past = now - timedelta(seconds=30)
    future = now + timedelta(minutes=5)

    due_queued = _make_event(
        source, destination, "due-queued", WebhookEvent.Status.QUEUED, next_attempt_at=past
    )
    due_failed = _make_event(
        source, destination, "due-failed", WebhookEvent.Status.FAILED, next_attempt_at=past
    )
    _make_event(
        source, destination, "future", WebhookEvent.Status.QUEUED, next_attempt_at=future
    )
    _make_event(
        source, destination, "delivered", WebhookEvent.Status.DELIVERED, next_attempt_at=past
    )
    _make_event(
        source, destination, "dead", WebhookEvent.Status.DEAD_LETTERED, next_attempt_at=past
    )
    _make_event(
        source, destination, "received", WebhookEvent.Status.RECEIVED, next_attempt_at=past
    )

    with patch("webhook_ops.webhooks.tasks.deliver_webhook_event.delay") as delay:
        count = enqueue_due_webhook_events()

    assert count == 2
    enqueued_ids = sorted(call.args[0] for call in delay.call_args_list)
    assert enqueued_ids == sorted([due_queued.id, due_failed.id])


@pytest.mark.django_db
def test_enqueue_due_returns_zero_when_nothing_is_due():
    with patch("webhook_ops.webhooks.tasks.deliver_webhook_event.delay") as delay:
        count = enqueue_due_webhook_events()

    assert count == 0
    delay.assert_not_called()
