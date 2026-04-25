from django.urls import path

from .views import WebhookIngestView

urlpatterns = [
    path("in/<slug:source_slug>/", WebhookIngestView.as_view(), name="webhook-ingest"),
]
