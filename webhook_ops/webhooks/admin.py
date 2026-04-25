from django.contrib import admin
from django.contrib.auth import get_user_model
from django.contrib.auth.admin import GroupAdmin as DjangoGroupAdmin
from django.contrib.auth.admin import UserAdmin as DjangoUserAdmin
from django.contrib.auth.models import Group
from unfold.admin import ModelAdmin, TabularInline
from unfold.forms import AdminPasswordChangeForm, UserChangeForm, UserCreationForm

from .models import (
    Agent,
    AuditLog,
    DeliveryAttempt,
    Destination,
    ReplayRequest,
    WebhookEvent,
    WebhookSource,
)
from .replay import replay_event
from .tasks import deliver_webhook_event

User = get_user_model()
admin.site.unregister(User)
admin.site.unregister(Group)


@admin.register(User)
class UserAdmin(DjangoUserAdmin, ModelAdmin):
    form = UserChangeForm
    add_form = UserCreationForm
    change_password_form = AdminPasswordChangeForm
    list_display = ("username", "is_staff", "is_superuser", "last_login", "date_joined")
    search_fields = ("username",)
    fieldsets = (
        (None, {"fields": ("username", "password")}),
        (
            "Permissions",
            {
                "fields": (
                    "is_active",
                    "is_staff",
                    "is_superuser",
                    "groups",
                    "user_permissions",
                )
            },
        ),
        ("Important dates", {"fields": ("last_login", "date_joined")}),
    )
    add_fieldsets = (
        (
            None,
            {
                "classes": ("wide",),
                "fields": ("username", "password1", "password2"),
            },
        ),
    )


@admin.register(Group)
class GroupAdmin(DjangoGroupAdmin, ModelAdmin):
    pass


@admin.register(Agent)
class AgentAdmin(ModelAdmin):
    list_display = ("name", "slug", "status", "allowed_target_count", "is_active", "last_seen_at")
    list_filter = ("status", "is_active")
    search_fields = ("name", "slug")
    prepopulated_fields = {"slug": ("name",)}
    exclude = ("shared_secret_hash", "tunnel_secret")
    readonly_fields = (
        "last_seen_at",
        "created_at",
        "updated_at",
    )

    @admin.display(description="Allowed targets")
    def allowed_target_count(self, obj):
        return len(obj.allowed_targets or [])


@admin.register(Destination)
class DestinationAdmin(ModelAdmin):
    list_display = ("name", "slug", "mode", "url", "agent", "is_active", "updated_at")
    list_filter = ("mode", "is_active", "verify_tls")
    search_fields = ("name", "slug", "url")
    prepopulated_fields = {"slug": ("name",)}


@admin.register(WebhookSource)
class WebhookSourceAdmin(ModelAdmin):
    list_display = ("name", "slug", "provider", "default_destination", "is_active", "updated_at")
    list_filter = ("provider", "is_active")
    search_fields = ("name", "slug")
    prepopulated_fields = {"slug": ("name",)}


class DeliveryAttemptInline(TabularInline):
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
class WebhookEventAdmin(ModelAdmin):
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
    actions = ["replay_events"]

    @admin.action(description="Replay selected events")
    def replay_events(self, request, queryset):
        queued = 0
        for event in queryset:
            replay = replay_event(
                event,
                requested_by=request.user,
                reason="Replayed via admin",
            )
            deliver_webhook_event.delay(replay.event_id)
            queued += 1
        self.message_user(request, f"Queued replay for {queued} event(s).")


@admin.register(DeliveryAttempt)
class DeliveryAttemptAdmin(ModelAdmin):
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
class ReplayRequestAdmin(ModelAdmin):
    list_display = ("event", "requested_by", "status", "processed_at", "created_at")
    list_filter = ("status",)
    search_fields = ("event__idempotency_key", "reason", "error")


@admin.register(AuditLog)
class AuditLogAdmin(ModelAdmin):
    list_display = ("action", "object_type", "object_id", "actor", "created_at")
    list_filter = ("action", "object_type")
    search_fields = ("action", "object_id", "message")
    readonly_fields = ("created_at", "updated_at")
