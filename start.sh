#!/bin/sh
set -eu

ROLE="${WEBHOOKOPS_ROLE:-web}"
PORT="${PORT:-8000}"

case "$ROLE" in
  web)
    python manage.py migrate --noinput
    exec gunicorn webhook_ops.wsgi:application --bind "0.0.0.0:${PORT}"
    ;;
  worker)
    exec celery -A webhook_ops worker --loglevel=info --concurrency=2
    ;;
  beat)
    exec celery -A webhook_ops beat --loglevel=info
    ;;
  *)
    echo "Unknown WEBHOOKOPS_ROLE: ${ROLE} (expected web|worker|beat)" >&2
    exit 1
    ;;
esac
