from django.contrib import admin

from .models import (
    Agent,
    AuditLog,
    DeliveryAttempt,
    Destination,
    ReplayRequest,
    WebhookEvent,
    WebhookSource,
)


@admin.register(Agent)
class AgentAdmin(admin.ModelAdmin):
    list_display = ("name", "slug", "status", "allowed_target_count", "is_active", "last_seen_at")
    list_filter = ("status", "is_active")
    search_fields = ("name", "slug")
    prepopulated_fields = {"slug": ("name",)}
    readonly_fields = (
        "shared_secret_hash",
        "tunnel_secret",
        "last_seen_at",
        "created_at",
        "updated_at",
    )

    @admin.display(description="Allowed targets")
    def allowed_target_count(self, obj):
        return len(obj.allowed_targets or [])


@admin.register(Destination)
class DestinationAdmin(admin.ModelAdmin):
    list_display = ("name", "slug", "mode", "url", "agent", "is_active", "updated_at")
    list_filter = ("mode", "is_active", "verify_tls")
    search_fields = ("name", "slug", "url")
    prepopulated_fields = {"slug": ("name",)}


@admin.register(WebhookSource)
class WebhookSourceAdmin(admin.ModelAdmin):
    list_display = ("name", "slug", "provider", "default_destination", "is_active", "updated_at")
    list_filter = ("provider", "is_active")
    search_fields = ("name", "slug")
    prepopulated_fields = {"slug": ("name",)}


class DeliveryAttemptInline(admin.TabularInline):
    model = DeliveryAttempt
    extra = 0
    readonly_fields = (
        "attempt_number",
        "status",
        "response_status_code",
        "error",
        "started_at",
        "finished_at",
        "duration_ms",
    )
    can_delete = False


@admin.register(WebhookEvent)
class WebhookEventAdmin(admin.ModelAdmin):
    list_display = (
        "id",
        "source",
        "destination",
        "status",
        "event_type",
        "attempt_count",
        "next_attempt_at",
        "created_at",
    )
    list_filter = ("status", "source", "destination")
    search_fields = ("idempotency_key", "provider_event_id", "body_sha256", "event_type")
    readonly_fields = ("body_sha256", "attempt_count", "created_at", "updated_at")
    inlines = [DeliveryAttemptInline]


@admin.register(DeliveryAttempt)
class DeliveryAttemptAdmin(admin.ModelAdmin):
    list_display = (
        "event",
        "destination",
        "attempt_number",
        "status",
        "response_status_code",
        "duration_ms",
        "created_at",
    )
    list_filter = ("status", "destination")
    search_fields = ("event__idempotency_key", "request_url", "error")
    readonly_fields = ("started_at", "finished_at", "duration_ms")


@admin.register(ReplayRequest)
class ReplayRequestAdmin(admin.ModelAdmin):
    list_display = ("event", "requested_by", "status", "processed_at", "created_at")
    list_filter = ("status",)
    search_fields = ("event__idempotency_key", "reason", "error")


@admin.register(AuditLog)
class AuditLogAdmin(admin.ModelAdmin):
    list_display = ("action", "object_type", "object_id", "actor", "created_at")
    list_filter = ("action", "object_type")
    search_fields = ("action", "object_id", "message")
    readonly_fields = ("created_at", "updated_at")
