from celery import shared_task


@shared_task(bind=True, name="webhooks.deliver_webhook_event")
def deliver_webhook_event(self, event_id: int) -> str:
    return f"delivery pending for event {event_id}"
