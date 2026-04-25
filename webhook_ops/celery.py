"""Celery application for asynchronous webhook delivery."""

import os

from celery import Celery

os.environ.setdefault("DJANGO_SETTINGS_MODULE", "webhook_ops.settings")

app = Celery("webhook_ops")
app.config_from_object("django.conf:settings", namespace="CELERY")
app.autodiscover_tasks()
