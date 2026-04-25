from django.contrib.auth import get_user_model
from django.db import transaction
from django.utils import timezone

from .models import AuditLog, ReplayRequest, WebhookEvent

User = get_user_model()


def replay_event(
    event: WebhookEvent,
    *,
    requested_by: User | None = None,
    reason: str = "",
) -> ReplayRequest:
    with transaction.atomic():
        locked_event = WebhookEvent.objects.select_for_update().get(id=event.id)
        replay = ReplayRequest.objects.create(
            event=locked_event,
            requested_by=requested_by if requested_by and requested_by.is_authenticated else None,
            reason=reason,
            status=ReplayRequest.Status.QUEUED,
        )
        locked_event.status = WebhookEvent.Status.QUEUED
        locked_event.attempt_count = 0
        locked_event.next_attempt_at = timezone.now()
        locked_event.last_error = ""
        locked_event.save(
            update_fields=[
                "status",
                "attempt_count",
                "next_attempt_at",
                "last_error",
                "updated_at",
            ]
        )
        AuditLog.objects.create(
            actor=replay.requested_by,
            action="webhook.replay_queued",
            object_type="WebhookEvent",
            object_id=str(locked_event.id),
            message="Webhook replay queued",
            metadata={"replay_request_id": replay.id, "reason": reason},
        )
        return replay
