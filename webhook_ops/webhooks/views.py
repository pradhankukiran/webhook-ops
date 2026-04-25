from django.shortcuts import get_object_or_404
from rest_framework import status
from rest_framework.permissions import AllowAny
from rest_framework.response import Response
from rest_framework.views import APIView

from .ingestion import ingest_event, normalize_headers
from .models import AuditLog, WebhookEvent, WebhookSource
from .signatures import verify_source_signature
from .tasks import deliver_webhook_event


class WebhookIngestView(APIView):
    authentication_classes = []
    permission_classes = [AllowAny]

    def post(self, request, source_slug: str) -> Response:
        source = get_object_or_404(WebhookSource, slug=source_slug, is_active=True)
        raw_body = request.body
        headers = normalize_headers(request.headers)
        signature = verify_source_signature(source, headers, raw_body)
        if not signature.ok:
            AuditLog.objects.create(
                action="webhook.signature_rejected",
                object_type="WebhookSource",
                object_id=str(source.id),
                message=signature.reason,
                metadata={"source": source.slug},
            )
            return Response({"detail": signature.reason}, status=status.HTTP_401_UNAUTHORIZED)

        ingested = ingest_event(source=source, headers=headers, raw_body=raw_body)
        event = ingested.event
        if ingested.created and event.status == WebhookEvent.Status.QUEUED:
            deliver_webhook_event.delay(event.id)

        response_status = status.HTTP_202_ACCEPTED if ingested.created else status.HTTP_200_OK
        return Response(
            {
                "event_id": event.id,
                "status": event.status,
                "duplicate": not ingested.created,
            },
            status=response_status,
        )
