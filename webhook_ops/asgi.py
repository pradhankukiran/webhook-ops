"""ASGI entrypoint for WebhookOps."""

import os

from django.core.asgi import get_asgi_application

os.environ.setdefault("DJANGO_SETTINGS_MODULE", "webhook_ops.settings")

application = get_asgi_application()
