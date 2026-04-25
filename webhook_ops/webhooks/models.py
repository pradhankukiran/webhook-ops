from django.conf import settings
from django.db import models
from django.db.models import Q
from django.utils import timezone


class TimestampedModel(models.Model):
    created_at = models.DateTimeField(auto_now_add=True)
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        abstract = True


class Agent(TimestampedModel):
    class Status(models.TextChoices):
        OFFLINE = "offline", "Offline"
        ONLINE = "online", "Online"
        DISABLED = "disabled", "Disabled"

    name = models.CharField(max_length=120)
    slug = models.SlugField(unique=True)
    status = models.CharField(max_length=20, choices=Status.choices, default=Status.OFFLINE)
    shared_secret_hash = models.CharField(max_length=255, blank=True)
    last_seen_at = models.DateTimeField(null=True, blank=True)
    is_active = models.BooleanField(default=True)

    class Meta:
        ordering = ["name"]

    def __str__(self) -> str:
        return self.name


class Destination(TimestampedModel):
    class DeliveryMode(models.TextChoices):
        PUBLIC_HTTP = "public_http", "Public HTTP"
        PRIVATE_AGENT = "private_agent", "Private Agent"

    name = models.CharField(max_length=120)
    slug = models.SlugField(unique=True)
    mode = models.CharField(
        max_length=30,
        choices=DeliveryMode.choices,
        default=DeliveryMode.PUBLIC_HTTP,
    )
    url = models.URLField()
    agent = models.ForeignKey(
        Agent,
        null=True,
        blank=True,
        on_delete=models.SET_NULL,
        related_name="destinations",
    )
    timeout_seconds = models.PositiveSmallIntegerField(default=15)
    verify_tls = models.BooleanField(default=True)
    forward_headers = models.JSONField(default=dict, blank=True)
    is_active = models.BooleanField(default=True)

    class Meta:
        ordering = ["name"]

    def __str__(self) -> str:
        return self.name


class WebhookSource(TimestampedModel):
    class Provider(models.TextChoices):
        GENERIC = "generic", "Generic"
        STRIPE = "stripe", "Stripe"
        GITHUB = "github", "GitHub"
        SHOPIFY = "shopify", "Shopify"
        SLACK = "slack", "Slack"

    name = models.CharField(max_length=120)
    slug = models.SlugField(unique=True)
    provider = models.CharField(
        max_length=30,
        choices=Provider.choices,
        default=Provider.GENERIC,
    )
    signing_secret = models.CharField(max_length=255, blank=True)
    default_destination = models.ForeignKey(
        Destination,
        null=True,
        blank=True,
        on_delete=models.SET_NULL,
        related_name="sources",
    )
    is_active = models.BooleanField(default=True)

    class Meta:
        ordering = ["name"]

    def __str__(self) -> str:
        return self.name


class WebhookEvent(TimestampedModel):
    class Status(models.TextChoices):
        RECEIVED = "received", "Received"
        QUEUED = "queued", "Queued"
        DELIVERING = "delivering", "Delivering"
        DELIVERED = "delivered", "Delivered"
        FAILED = "failed", "Failed"
        DEAD_LETTERED = "dead_lettered", "Dead Lettered"

    source = models.ForeignKey(WebhookSource, on_delete=models.PROTECT, related_name="events")
    destination = models.ForeignKey(
        Destination,
        null=True,
        blank=True,
        on_delete=models.SET_NULL,
        related_name="events",
    )
    provider_event_id = models.CharField(max_length=255, blank=True)
    event_type = models.CharField(max_length=255, blank=True)
    idempotency_key = models.CharField(max_length=255, null=True, blank=True)
    status = models.CharField(max_length=30, choices=Status.choices, default=Status.RECEIVED)
    headers = models.JSONField(default=dict, blank=True)
    payload = models.JSONField(default=dict, blank=True)
    raw_body = models.TextField(blank=True)
    body_sha256 = models.CharField(max_length=64, db_index=True)
    attempt_count = models.PositiveSmallIntegerField(default=0)
    max_attempts = models.PositiveSmallIntegerField(default=6)
    next_attempt_at = models.DateTimeField(default=timezone.now)
    last_error = models.TextField(blank=True)

    class Meta:
        ordering = ["-created_at"]
        constraints = [
            models.UniqueConstraint(
                fields=["source", "idempotency_key"],
                condition=Q(idempotency_key__isnull=False),
                name="unique_event_idempotency_key_per_source",
            )
        ]
        indexes = [
            models.Index(fields=["status", "next_attempt_at"]),
            models.Index(fields=["source", "-created_at"]),
            models.Index(fields=["idempotency_key"]),
        ]

    def __str__(self) -> str:
        return f"{self.source.slug}:{self.idempotency_key or self.body_sha256[:12]}"


class DeliveryAttempt(TimestampedModel):
    class Status(models.TextChoices):
        PENDING = "pending", "Pending"
        SUCCEEDED = "succeeded", "Succeeded"
        FAILED = "failed", "Failed"

    event = models.ForeignKey(WebhookEvent, on_delete=models.CASCADE, related_name="attempts")
    destination = models.ForeignKey(
        Destination,
        null=True,
        blank=True,
        on_delete=models.SET_NULL,
        related_name="attempts",
    )
    attempt_number = models.PositiveSmallIntegerField()
    status = models.CharField(max_length=20, choices=Status.choices, default=Status.PENDING)
    request_url = models.URLField()
    request_headers = models.JSONField(default=dict, blank=True)
    response_status_code = models.PositiveSmallIntegerField(null=True, blank=True)
    response_headers = models.JSONField(default=dict, blank=True)
    response_body = models.TextField(blank=True)
    error = models.TextField(blank=True)
    started_at = models.DateTimeField(default=timezone.now)
    finished_at = models.DateTimeField(null=True, blank=True)
    duration_ms = models.PositiveIntegerField(null=True, blank=True)

    class Meta:
        ordering = ["-created_at"]
        indexes = [
            models.Index(fields=["event", "attempt_number"]),
            models.Index(fields=["status", "-created_at"]),
        ]

    def __str__(self) -> str:
        return f"{self.event_id} attempt {self.attempt_number}"


class ReplayRequest(TimestampedModel):
    class Status(models.TextChoices):
        PENDING = "pending", "Pending"
        QUEUED = "queued", "Queued"
        COMPLETED = "completed", "Completed"
        FAILED = "failed", "Failed"

    event = models.ForeignKey(
        WebhookEvent,
        on_delete=models.CASCADE,
        related_name="replay_requests",
    )
    requested_by = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        null=True,
        blank=True,
        on_delete=models.SET_NULL,
        related_name="webhook_replay_requests",
    )
    reason = models.TextField(blank=True)
    status = models.CharField(max_length=20, choices=Status.choices, default=Status.PENDING)
    processed_at = models.DateTimeField(null=True, blank=True)
    error = models.TextField(blank=True)

    class Meta:
        ordering = ["-created_at"]

    def __str__(self) -> str:
        return f"Replay {self.id} for event {self.event_id}"


class AuditLog(TimestampedModel):
    actor = models.ForeignKey(
        settings.AUTH_USER_MODEL,
        null=True,
        blank=True,
        on_delete=models.SET_NULL,
        related_name="webhook_audit_logs",
    )
    action = models.CharField(max_length=120)
    object_type = models.CharField(max_length=120, blank=True)
    object_id = models.CharField(max_length=120, blank=True)
    message = models.TextField(blank=True)
    metadata = models.JSONField(default=dict, blank=True)

    class Meta:
        ordering = ["-created_at"]
        indexes = [
            models.Index(fields=["action", "-created_at"]),
            models.Index(fields=["object_type", "object_id"]),
        ]

    def __str__(self) -> str:
        return self.action
