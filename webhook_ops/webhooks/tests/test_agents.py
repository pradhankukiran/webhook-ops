import pytest
from django.contrib.auth import get_user_model
from django.contrib.auth.hashers import check_password
from django.urls import reverse

from webhook_ops.webhooks.models import Agent, AuditLog


@pytest.mark.django_db
def test_agent_enrollment_returns_run_command_and_hashes_secret(client, settings):
    settings.WEBHOOKOPS_PUBLIC_TUNNEL_ENDPOINT = "webhookops.example.test:9700"
    settings.WEBHOOKOPS_TUNNEL_SECRET = "secret-value"
    user = get_user_model().objects.create_superuser(
        username="admin",
        email="admin@example.test",
        password="password",
    )
    agent = Agent.objects.create(
        name="Dev machine",
        slug="dev-machine",
        allowed_targets=["localhost:8000", "10.0.0.5:9000"],
    )
    client.force_login(user)

    response = client.post(reverse("agents-enrollment", kwargs={"pk": agent.id}))

    agent.refresh_from_db()
    assert response.status_code == 200
    assert response.data["agent_id"] == "dev-machine"
    assert "--tunnel webhookops.example.test:9700" in response.data["command"]
    assert "--id dev-machine" in response.data["command"]
    assert "--allow localhost:8000" in response.data["command"]
    assert "secret=secret-value" in response.data["config"]
    assert check_password("secret-value", agent.shared_secret_hash)
    assert AuditLog.objects.filter(action="agent.enrollment_generated").exists()


@pytest.mark.django_db
def test_tunnel_status_endpoint_updates_existing_agent(client, settings):
    settings.WEBHOOKOPS_TUNNEL_STATUS_TOKEN = "status-token"
    agent = Agent.objects.create(name="Dev machine", slug="dev-machine")

    response = client.post(
        reverse("tunnel-agent-status"),
        data={"agent_id": "dev-machine", "event": "online", "metadata": {"peer": "127.0.0.1"}},
        content_type="application/json",
        HTTP_X_WEBHOOKOPS_TUNNEL_TOKEN="status-token",
    )

    agent.refresh_from_db()
    assert response.status_code == 200
    assert agent.status == Agent.Status.ONLINE
    assert agent.last_seen_at is not None
    assert AuditLog.objects.filter(action="agent.online", object_id=str(agent.id)).exists()


@pytest.mark.django_db
def test_tunnel_status_endpoint_marks_agent_offline(client, settings):
    settings.WEBHOOKOPS_TUNNEL_STATUS_TOKEN = "status-token"
    agent = Agent.objects.create(
        name="Dev machine",
        slug="dev-machine",
        status=Agent.Status.ONLINE,
    )

    response = client.post(
        reverse("tunnel-agent-status"),
        data={"agent_id": "dev-machine", "event": "offline"},
        content_type="application/json",
        HTTP_X_WEBHOOKOPS_TUNNEL_TOKEN="status-token",
    )

    agent.refresh_from_db()
    assert response.status_code == 200
    assert agent.status == Agent.Status.OFFLINE
    assert AuditLog.objects.filter(action="agent.offline", object_id=str(agent.id)).exists()


@pytest.mark.django_db
def test_tunnel_status_endpoint_rejects_invalid_token(client, settings):
    settings.WEBHOOKOPS_TUNNEL_STATUS_TOKEN = "status-token"

    response = client.post(
        reverse("tunnel-agent-status"),
        data={"agent_id": "dev-machine", "event": "online"},
        content_type="application/json",
        HTTP_X_WEBHOOKOPS_TUNNEL_TOKEN="wrong",
    )

    assert response.status_code == 401
    assert Agent.objects.count() == 0
