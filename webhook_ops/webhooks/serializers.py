from rest_framework import serializers

from .models import Agent, DeliveryAttempt, Destination, ReplayRequest, WebhookEvent, WebhookSource


class AgentSerializer(serializers.ModelSerializer):
    class Meta:
        model = Agent
        fields = [
            "id",
            "name",
            "slug",
            "status",
            "allowed_targets",
            "last_seen_at",
            "is_active",
            "created_at",
            "updated_at",
        ]


class DestinationSerializer(serializers.ModelSerializer):
    class Meta:
        model = Destination
        fields = [
            "id",
            "name",
            "slug",
            "mode",
            "url",
            "agent",
            "timeout_seconds",
            "verify_tls",
            "forward_headers",
            "is_active",
            "created_at",
            "updated_at",
        ]


class WebhookSourceSerializer(serializers.ModelSerializer):
    class Meta:
        model = WebhookSource
        fields = [
            "id",
            "name",
            "slug",
            "provider",
            "signing_secret",
            "default_destination",
            "is_active",
            "created_at",
            "updated_at",
        ]
        extra_kwargs = {"signing_secret": {"write_only": True, "required": False}}


class DeliveryAttemptSerializer(serializers.ModelSerializer):
    class Meta:
        model = DeliveryAttempt
        fields = [
            "id",
            "event",
            "destination",
            "attempt_number",
            "status",
            "request_url",
            "response_status_code",
            "response_body",
            "error",
            "started_at",
            "finished_at",
            "duration_ms",
            "created_at",
            "updated_at",
        ]
        read_only_fields = fields


class WebhookEventSerializer(serializers.ModelSerializer):
    attempts = DeliveryAttemptSerializer(many=True, read_only=True)

    class Meta:
        model = WebhookEvent
        fields = [
            "id",
            "source",
            "destination",
            "provider_event_id",
            "event_type",
            "idempotency_key",
            "status",
            "payload",
            "body_sha256",
            "attempt_count",
            "max_attempts",
            "next_attempt_at",
            "last_error",
            "attempts",
            "created_at",
            "updated_at",
        ]
        read_only_fields = fields


class ReplayRequestSerializer(serializers.ModelSerializer):
    class Meta:
        model = ReplayRequest
        fields = [
            "id",
            "event",
            "requested_by",
            "reason",
            "status",
            "processed_at",
            "error",
            "created_at",
            "updated_at",
        ]
        read_only_fields = [
            "id",
            "requested_by",
            "status",
            "processed_at",
            "error",
            "created_at",
            "updated_at",
        ]
