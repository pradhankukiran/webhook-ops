from django.apps import AppConfig


class WebhooksConfig(AppConfig):
    default_auto_field = "django.db.models.BigAutoField"
    name = "webhook_ops.webhooks"
    verbose_name = "Webhook Operations"
