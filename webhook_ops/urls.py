"""URL routes for WebhookOps."""

from django.conf import settings
from django.contrib import admin
from django.db import connection
from django.http import JsonResponse
from django.shortcuts import render
from django.urls import include, path
from django.views.generic import TemplateView
from django.views.generic.base import RedirectView
from drf_spectacular.views import SpectacularAPIView, SpectacularSwaggerView


def _check_database():
    try:
        connection.ensure_connection()
        return True, ""
    except Exception as exc:  # noqa: BLE001
        return False, str(exc)


def _check_redis():
    try:
        import redis as redis_lib

        client = redis_lib.from_url(settings.REDIS_URL, socket_connect_timeout=2)
        client.ping()
        return True, ""
    except Exception as exc:  # noqa: BLE001
        return False, str(exc)


def health_check(request):
    db_ok, db_error = _check_database()
    redis_ok, redis_error = _check_redis()
    overall_ok = db_ok and redis_ok

    checks = [
        {"name": "App", "ok": True, "detail": "Django web is responding"},
        {"name": "Database", "ok": db_ok, "detail": db_error or "PostgreSQL connection healthy"},
        {"name": "Redis", "ok": redis_ok, "detail": redis_error or "Celery broker reachable"},
    ]

    accepts_html = "text/html" in request.headers.get("Accept", "")
    if accepts_html:
        return render(
            request,
            "health.html",
            {"checks": checks, "overall_ok": overall_ok},
            status=200 if overall_ok else 503,
        )

    return JsonResponse(
        {
            "status": "ok" if overall_ok else "error",
            "checks": {c["name"].lower(): c["ok"] for c in checks},
        },
        status=200 if overall_ok else 503,
    )


urlpatterns = [
    path("", TemplateView.as_view(template_name="landing.html"), name="landing"),
    path(
        "favicon.ico",
        RedirectView.as_view(url="/static/webhook_ops/favicon.svg", permanent=True),
    ),
    path("admin/", admin.site.urls),
    path("healthz/", health_check, name="health-check"),
    path("api/schema/", SpectacularAPIView.as_view(), name="schema"),
    path("api/docs/", SpectacularSwaggerView.as_view(url_name="schema"), name="swagger-ui"),
    path("", include("webhook_ops.webhooks.urls")),
]
