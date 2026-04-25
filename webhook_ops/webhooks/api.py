from rest_framework import status, viewsets
from rest_framework.decorators import action
from rest_framework.response import Response

from .models import Agent, Destination, ReplayRequest, WebhookEvent, WebhookSource
from .replay import replay_event
from .serializers import (
    AgentSerializer,
    DestinationSerializer,
    ReplayRequestSerializer,
    WebhookEventSerializer,
    WebhookSourceSerializer,
)
from .tasks import deliver_webhook_event


class AgentViewSet(viewsets.ModelViewSet):
    queryset = Agent.objects.all()
    serializer_class = AgentSerializer
    search_fields = ["name", "slug"]
    ordering_fields = ["name", "status", "updated_at"]


class DestinationViewSet(viewsets.ModelViewSet):
    queryset = Destination.objects.select_related("agent").all()
    serializer_class = DestinationSerializer
    search_fields = ["name", "slug", "url"]
    ordering_fields = ["name", "mode", "updated_at"]


class WebhookSourceViewSet(viewsets.ModelViewSet):
    queryset = WebhookSource.objects.select_related("default_destination").all()
    serializer_class = WebhookSourceSerializer
    search_fields = ["name", "slug", "provider"]
    ordering_fields = ["name", "provider", "updated_at"]


class WebhookEventViewSet(viewsets.ReadOnlyModelViewSet):
    queryset = (
        WebhookEvent.objects.select_related("source", "destination")
        .prefetch_related("attempts")
        .all()
    )
    serializer_class = WebhookEventSerializer
    filterset_fields = ["source", "destination", "status", "event_type"]
    search_fields = ["idempotency_key", "provider_event_id", "body_sha256", "event_type"]
    ordering_fields = ["created_at", "updated_at", "next_attempt_at", "attempt_count"]

    @action(detail=True, methods=["post"])
    def replay(self, request, pk=None) -> Response:
        replay = replay_event(
            self.get_object(),
            requested_by=request.user,
            reason=str(request.data.get("reason", "")),
        )
        deliver_webhook_event.delay(replay.event_id)
        serializer = ReplayRequestSerializer(replay)
        return Response(serializer.data, status=status.HTTP_202_ACCEPTED)


class ReplayRequestViewSet(viewsets.ReadOnlyModelViewSet):
    queryset = ReplayRequest.objects.select_related("event", "requested_by").all()
    serializer_class = ReplayRequestSerializer
    filterset_fields = ["event", "status"]
    ordering_fields = ["created_at", "updated_at", "processed_at"]
