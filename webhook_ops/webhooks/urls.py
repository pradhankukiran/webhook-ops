from django.urls import include, path
from rest_framework.routers import DefaultRouter

from .api import (
    AgentViewSet,
    DestinationViewSet,
    ReplayRequestViewSet,
    WebhookEventViewSet,
    WebhookSourceViewSet,
)
from .views import TunnelAgentStatusView, WebhookIngestView

router = DefaultRouter()
router.register("agents", AgentViewSet, basename="agents")
router.register("destinations", DestinationViewSet, basename="destinations")
router.register("sources", WebhookSourceViewSet, basename="sources")
router.register("events", WebhookEventViewSet, basename="events")
router.register("replay-requests", ReplayRequestViewSet, basename="replay-requests")

urlpatterns = [
    path("in/<slug:source_slug>/", WebhookIngestView.as_view(), name="webhook-ingest"),
    path(
        "internal/tunnel/agent-status/",
        TunnelAgentStatusView.as_view(),
        name="tunnel-agent-status",
    ),
    path("api/", include(router.urls)),
]
