"""Auto-login middleware: silently log every visitor in as the first superuser.

This is a deliberate security trade-off for a public demo deploy. Disable by
removing this middleware from `MIDDLEWARE` in settings.
"""

from __future__ import annotations

from django.contrib.auth import get_user_model, login
from django.utils.deprecation import MiddlewareMixin

_BACKEND = "django.contrib.auth.backends.ModelBackend"


class AutoLoginAsSuperuserMiddleware(MiddlewareMixin):
    def process_request(self, request):
        if request.user.is_authenticated:
            return

        User = get_user_model()
        admin = (
            User.objects.filter(is_superuser=True, is_active=True)
            .order_by("id")
            .first()
        )
        if admin is None:
            return

        admin.backend = _BACKEND
        login(request, admin)
