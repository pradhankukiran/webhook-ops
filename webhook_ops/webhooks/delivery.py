from __future__ import annotations

from dataclasses import dataclass
from datetime import timedelta
from typing import Any

import requests
from django.conf import settings
from django.db import transaction
from django.utils import timezone

from .models import Agent, AuditLog, DeliveryAttempt, Destination, WebhookEvent

HOP_BY_HOP_HEADERS = {
    "host",
    "content-length",
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
}


@dataclass(frozen=True)
class DeliveryResult:
    event_id: int
    delivered: bool
    retry_after_seconds: int | None = None
    detail: str = ""


def retry_delay_seconds(attempt_count: int) -> int:
    return min(5 * (2 ** max(attempt_count - 1, 0)), 60 * 60)


def _truncate(value: str, limit: int = 4000) -> str:
    if len(value) <= limit:
        return value
    return value[:limit] + "...[truncated]"


def build_forward_headers(
    event: WebhookEvent,
    destination: Destination,
    attempt_number: int,
) -> dict[str, str]:
    headers = {
        key: str(value)
        for key, value in event.headers.items()
        if key.lower() not in HOP_BY_HOP_HEADERS
    }
    headers["x-webhookops-event-id"] = str(event.id)
    headers["x-webhookops-source"] = event.source.slug
    headers["x-webhookops-attempt"] = str(attempt_number)
    headers.update({str(key): str(value) for key, value in destination.forward_headers.items()})
    return headers


def mark_event_without_destination(event: WebhookEvent) -> DeliveryResult:
    event.status = WebhookEvent.Status.FAILED
    event.last_error = "event has no active destination"
    event.save(update_fields=["status", "last_error", "updated_at"])
    AuditLog.objects.create(
        action="webhook.delivery_skipped",
        object_type="WebhookEvent",
        object_id=str(event.id),
        message=event.last_error,
    )
    return DeliveryResult(event_id=event.id, delivered=False, detail=event.last_error)


def begin_delivery_attempt(event_id: int) -> DeliveryAttempt | None:
    with transaction.atomic():
        event = (
            WebhookEvent.objects.select_for_update()
            .select_related("source", "destination")
            .get(id=event_id)
        )
        if event.status == WebhookEvent.Status.DELIVERED:
            return None
        if not event.destination_id or not event.destination or not event.destination.is_active:
            mark_event_without_destination(event)
            return None

        event.attempt_count += 1
        event.status = WebhookEvent.Status.DELIVERING
        event.last_error = ""
        event.save(update_fields=["attempt_count", "status", "last_error", "updated_at"])

        return DeliveryAttempt.objects.create(
            event=event,
            destination=event.destination,
            attempt_number=event.attempt_count,
            request_url=event.destination.url,
            request_headers=build_forward_headers(event, event.destination, event.attempt_count),
        )


def perform_public_http_delivery(attempt: DeliveryAttempt) -> tuple[bool, dict[str, Any]]:
    event = attempt.event
    destination = attempt.destination
    if destination is None:
        return False, {"error": "missing destination"}
    if destination.mode != Destination.DeliveryMode.PUBLIC_HTTP:
        return False, {"error": f"unsupported delivery mode: {destination.mode}"}

    try:
        response = requests.post(
            destination.url,
            data=event.raw_body.encode("utf-8"),
            headers=attempt.request_headers,
            timeout=destination.timeout_seconds,
            verify=destination.verify_tls,
        )
    except requests.RequestException as exc:
        return False, {"error": str(exc)}

    ok = 200 <= response.status_code < 300
    return ok, {
        "response_status_code": response.status_code,
        "response_headers": dict(response.headers),
        "response_body": _truncate(response.text),
        "error": "" if ok else f"destination returned HTTP {response.status_code}",
    }


def perform_private_agent_delivery(attempt: DeliveryAttempt) -> tuple[bool, dict[str, Any]]:
    event = attempt.event
    destination = attempt.destination
    if destination is None:
        return False, {"error": "missing destination"}
    if not destination.agent_id:
        return False, {"error": "private destination requires an agent"}
    if destination.agent and (
        not destination.agent.is_active or destination.agent.status == Agent.Status.DISABLED
    ):
        return False, {"error": "private destination agent is disabled"}

    proxy_url = settings.WEBHOOKOPS_PRIVATE_PROXY_URL
    proxies = {
        "http": proxy_url,
        "https": proxy_url,
    }

    try:
        response = requests.post(
            destination.url,
            data=event.raw_body.encode("utf-8"),
            headers=attempt.request_headers,
            timeout=destination.timeout_seconds,
            verify=destination.verify_tls,
            proxies=proxies,
        )
    except requests.RequestException as exc:
        return False, {"error": str(exc)}

    ok = 200 <= response.status_code < 300
    return ok, {
        "response_status_code": response.status_code,
        "response_headers": dict(response.headers),
        "response_body": _truncate(response.text),
        "error": "" if ok else f"private destination returned HTTP {response.status_code}",
    }


def perform_delivery(attempt: DeliveryAttempt) -> tuple[bool, dict[str, Any]]:
    if attempt.destination is None:
        return False, {"error": "missing destination"}
    if attempt.destination.mode == Destination.DeliveryMode.PUBLIC_HTTP:
        return perform_public_http_delivery(attempt)
    if attempt.destination.mode == Destination.DeliveryMode.PRIVATE_AGENT:
        return perform_private_agent_delivery(attempt)
    return False, {"error": f"unsupported delivery mode: {attempt.destination.mode}"}


def finalize_delivery_attempt(
    attempt: DeliveryAttempt,
    ok: bool,
    details: dict[str, Any],
) -> DeliveryResult:
    finished_at = timezone.now()
    attempt.status = DeliveryAttempt.Status.SUCCEEDED if ok else DeliveryAttempt.Status.FAILED
    attempt.response_status_code = details.get("response_status_code")
    attempt.response_headers = details.get("response_headers", {})
    attempt.response_body = details.get("response_body", "")
    attempt.error = details.get("error", "")
    attempt.finished_at = finished_at
    attempt.duration_ms = int((finished_at - attempt.started_at).total_seconds() * 1000)
    attempt.save(
        update_fields=[
            "status",
            "response_status_code",
            "response_headers",
            "response_body",
            "error",
            "finished_at",
            "duration_ms",
            "updated_at",
        ]
    )

    event = attempt.event
    if ok:
        event.status = WebhookEvent.Status.DELIVERED
        event.last_error = ""
        event.save(update_fields=["status", "last_error", "updated_at"])
        AuditLog.objects.create(
            action="webhook.delivered",
            object_type="WebhookEvent",
            object_id=str(event.id),
            message=f"Delivered event to {attempt.request_url}",
            metadata={"attempt": attempt.attempt_number},
        )
        return DeliveryResult(event_id=event.id, delivered=True, detail="delivered")

    if event.attempt_count >= event.max_attempts:
        event.status = WebhookEvent.Status.DEAD_LETTERED
        event.next_attempt_at = timezone.now()
        event.last_error = attempt.error
        event.save(update_fields=["status", "next_attempt_at", "last_error", "updated_at"])
        AuditLog.objects.create(
            action="webhook.dead_lettered",
            object_type="WebhookEvent",
            object_id=str(event.id),
            message=attempt.error,
            metadata={"attempt": attempt.attempt_number},
        )
        return DeliveryResult(event_id=event.id, delivered=False, detail="dead_lettered")

    retry_after = retry_delay_seconds(event.attempt_count)
    event.status = WebhookEvent.Status.FAILED
    event.next_attempt_at = timezone.now() + timedelta(seconds=retry_after)
    event.last_error = attempt.error
    event.save(update_fields=["status", "next_attempt_at", "last_error", "updated_at"])
    AuditLog.objects.create(
        action="webhook.delivery_failed",
        object_type="WebhookEvent",
        object_id=str(event.id),
        message=attempt.error,
        metadata={"attempt": attempt.attempt_number, "retry_after_seconds": retry_after},
    )
    return DeliveryResult(
        event_id=event.id,
        delivered=False,
        retry_after_seconds=retry_after,
        detail=attempt.error,
    )


def deliver_event(event_id: int) -> DeliveryResult:
    attempt = begin_delivery_attempt(event_id)
    if attempt is None:
        return DeliveryResult(event_id=event_id, delivered=False, detail="nothing to deliver")

    attempt = DeliveryAttempt.objects.select_related(
        "event",
        "event__source",
        "destination",
    ).get(id=attempt.id)
    ok, details = perform_delivery(attempt)
    return finalize_delivery_attempt(attempt, ok, details)
