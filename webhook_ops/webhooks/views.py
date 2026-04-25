import hmac

from django.conf import settings
from django.shortcuts import get_object_or_404
from rest_framework import status
from rest_framework.permissions import AllowAny
from rest_framework.response import Response
from rest_framework.views import APIView

from .agents import VALID_STATUS_EVENTS, record_agent_status, verify_agent_auth
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


class TunnelAgentStatusView(APIView):
    authentication_classes = []
    permission_classes = [AllowAny]

    def post(self, request) -> Response:
        token = request.headers.get("x-webhookops-tunnel-token", "")
        expected = settings.WEBHOOKOPS_TUNNEL_STATUS_TOKEN
        if not expected or not hmac.compare_digest(token, expected):
            return Response({"detail": "invalid tunnel token"}, status=status.HTTP_401_UNAUTHORIZED)

        agent_id = str(request.data.get("agent_id", "")).strip()
        event = str(request.data.get("event", "")).strip()
        if not agent_id:
            return Response({"detail": "agent_id is required"}, status=status.HTTP_400_BAD_REQUEST)
        if event not in VALID_STATUS_EVENTS:
            return Response({"detail": "unsupported event"}, status=status.HTTP_400_BAD_REQUEST)

        metadata = request.data.get("metadata")
        if not isinstance(metadata, dict):
            metadata = {}
        agent = record_agent_status(agent_id, event, metadata=metadata)
        return Response(
            {
                "agent_id": agent.slug,
                "status": agent.status,
                "last_seen_at": agent.last_seen_at,
            },
            status=status.HTTP_200_OK,
        )


class TunnelAgentAuthView(APIView):
    authentication_classes = []
    permission_classes = [AllowAny]

    def post(self, request) -> Response:
        token = request.headers.get("x-webhookops-tunnel-token", "")
        expected = settings.WEBHOOKOPS_TUNNEL_AUTH_TOKEN
        if not expected or not hmac.compare_digest(token, expected):
            return Response({"detail": "invalid tunnel token"}, status=status.HTTP_401_UNAUTHORIZED)

        metadata = request.data.get("metadata")
        if not isinstance(metadata, dict):
            metadata = {}
        try:
            agent = verify_agent_auth(
                agent_slug=str(request.data.get("agent_id", "")).strip(),
                nonce=str(request.data.get("nonce", "")).strip(),
                proof=str(request.data.get("proof", "")).strip(),
                metadata=metadata,
            )
        except ValueError as exc:
            return Response({"detail": str(exc)}, status=status.HTTP_401_UNAUTHORIZED)

        return Response(
            {
                "agent_id": agent.slug,
                "ok": True,
            },
            status=status.HTTP_200_OK,
        )
