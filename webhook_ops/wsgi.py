"""WSGI entrypoint for WebhookOps."""

import os

from django.core.wsgi import get_wsgi_application

os.environ.setdefault("DJANGO_SETTINGS_MODULE", "webhook_ops.settings")

application = get_wsgi_application()
