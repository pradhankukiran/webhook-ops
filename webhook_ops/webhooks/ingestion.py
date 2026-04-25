import hashlib
import json
from dataclasses import dataclass
from typing import Any

from django.db import IntegrityError, transaction

from .models import AuditLog, WebhookEvent, WebhookSource


@dataclass(frozen=True)
class IngestedEvent:
    event: WebhookEvent
    created: bool


def normalize_headers(headers: Any) -> dict[str, str]:
    return {str(key).lower(): str(value) for key, value in headers.items()}


def sha256_hex(raw_body: bytes) -> str:
    return hashlib.sha256(raw_body).hexdigest()


def parse_json_payload(raw_body: bytes) -> dict[str, Any]:
    if not raw_body:
        return {}
    try:
        value = json.loads(raw_body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {"value": value}


def _header(headers: dict[str, str], name: str) -> str:
    return headers.get(name.lower(), "")


def extract_event_metadata(
    source: WebhookSource,
    headers: dict[str, str],
    payload: dict[str, Any],
    body_hash: str,
) -> tuple[str, str, str]:
    provider_event_id = ""
    event_type = ""

    if source.provider == WebhookSource.Provider.STRIPE:
        provider_event_id = str(payload.get("id") or "")
        event_type = str(payload.get("type") or "")
    elif source.provider == WebhookSource.Provider.GITHUB:
        provider_event_id = _header(headers, "x-github-delivery")
        event_type = _header(headers, "x-github-event")
    elif source.provider == WebhookSource.Provider.SHOPIFY:
        provider_event_id = _header(headers, "x-shopify-webhook-id")
        event_type = _header(headers, "x-shopify-topic")
    elif source.provider == WebhookSource.Provider.SLACK:
        provider_event_id = str(payload.get("event_id") or payload.get("trigger_id") or "")
        event_type = str(payload.get("type") or "")
    else:
        provider_event_id = (
            _header(headers, "x-webhook-id")
            or _header(headers, "x-event-id")
            or _header(headers, "x-request-id")
        )
        event_type = _header(headers, "x-event-type")

    idempotency_key = provider_event_id or body_hash
    return provider_event_id, event_type, idempotency_key


def ingest_event(
    source: WebhookSource,
    headers: dict[str, str],
    raw_body: bytes,
) -> IngestedEvent:
    normalized_headers = normalize_headers(headers)
    body_hash = sha256_hex(raw_body)
    payload = parse_json_payload(raw_body)
    provider_event_id, event_type, idempotency_key = extract_event_metadata(
        source=source,
        headers=normalized_headers,
        payload=payload,
        body_hash=body_hash,
    )
    destination = source.default_destination if source.default_destination_id else None
    initial_status = (
        WebhookEvent.Status.QUEUED
        if destination and destination.is_active
        else WebhookEvent.Status.RECEIVED
    )

    defaults = {
        "destination": destination,
        "provider_event_id": provider_event_id,
        "event_type": event_type,
        "status": initial_status,
        "headers": normalized_headers,
        "payload": payload,
        "raw_body": raw_body.decode("utf-8", errors="replace"),
        "body_sha256": body_hash,
    }

    try:
        with transaction.atomic():
            event, created = WebhookEvent.objects.get_or_create(
                source=source,
                idempotency_key=idempotency_key,
                defaults=defaults,
            )
            if created:
                AuditLog.objects.create(
                    action="webhook.received",
                    object_type="WebhookEvent",
                    object_id=str(event.id),
                    message=f"Received webhook from {source.slug}",
                    metadata={"source": source.slug, "event_type": event_type},
                )
                return IngestedEvent(event=event, created=True)
    except IntegrityError:
        event = WebhookEvent.objects.get(source=source, idempotency_key=idempotency_key)

    AuditLog.objects.create(
        action="webhook.duplicate",
        object_type="WebhookEvent",
        object_id=str(event.id),
        message=f"Duplicate webhook ignored for {source.slug}",
        metadata={"source": source.slug, "idempotency_key": idempotency_key},
    )
    return IngestedEvent(event=event, created=False)
