from celery import shared_task
from django.utils import timezone

from .delivery import deliver_event
from .models import WebhookEvent


@shared_task(bind=True, name="webhooks.deliver_webhook_event")
def deliver_webhook_event(self, event_id: int) -> str:
    result = deliver_event(event_id)
    if result.retry_after_seconds:
        deliver_webhook_event.apply_async(args=[event_id], countdown=result.retry_after_seconds)
    return result.detail


@shared_task(name="webhooks.enqueue_due_webhook_events")
def enqueue_due_webhook_events() -> int:
    event_ids = list(
        WebhookEvent.objects.filter(
            status__in=[WebhookEvent.Status.QUEUED, WebhookEvent.Status.FAILED],
            next_attempt_at__lte=timezone.now(),
        ).values_list("id", flat=True)[:100]
    )
    for event_id in event_ids:
        deliver_webhook_event.delay(event_id)
    return len(event_ids)
