from dataclasses import dataclass

from django.conf import settings
from django.contrib.auth.hashers import make_password
from django.utils import timezone

from .models import Agent, AuditLog

STATUS_ONLINE = "online"
STATUS_HEARTBEAT = "heartbeat"
STATUS_OFFLINE = "offline"
VALID_STATUS_EVENTS = {STATUS_ONLINE, STATUS_HEARTBEAT, STATUS_OFFLINE}


@dataclass(frozen=True)
class AgentEnrollment:
    agent: Agent
    tunnel_endpoint: str
    secret: str
    allowed_targets: list[str]

    @property
    def command(self) -> str:
        parts = [
            "agent/build/webhookops-agent",
            "join",
            "--tunnel",
            self.tunnel_endpoint,
            "--id",
            self.agent.slug,
            "--secret",
            self.secret,
        ]
        for target in self.allowed_targets:
            parts.extend(["--allow", target])
        return " ".join(parts)

    @property
    def config(self) -> str:
        targets = "\n".join(f"allow={target}" for target in self.allowed_targets)
        return "\n".join(
            [
                f"tunnel={self.tunnel_endpoint}",
                f"id={self.agent.slug}",
                f"secret={self.secret}",
                targets,
            ]
        ).strip()


def normalized_allowed_targets(agent: Agent) -> list[str]:
    targets = agent.allowed_targets if isinstance(agent.allowed_targets, list) else []
    return [str(target).strip() for target in targets if str(target).strip()]


def build_agent_enrollment(agent: Agent) -> AgentEnrollment:
    secret = settings.WEBHOOKOPS_TUNNEL_SECRET
    agent.shared_secret_hash = make_password(secret)
    agent.save(update_fields=["shared_secret_hash", "updated_at"])
    AuditLog.objects.create(
        action="agent.enrollment_generated",
        object_type="Agent",
        object_id=str(agent.id),
        message=f"Generated enrollment command for {agent.slug}",
        metadata={"agent": agent.slug},
    )
    return AgentEnrollment(
        agent=agent,
        tunnel_endpoint=settings.WEBHOOKOPS_PUBLIC_TUNNEL_ENDPOINT,
        secret=secret,
        allowed_targets=normalized_allowed_targets(agent),
    )


def record_agent_status(agent_slug: str, event: str, metadata: dict | None = None) -> Agent:
    if event not in VALID_STATUS_EVENTS:
        raise ValueError(f"unsupported agent status event: {event}")

    now = timezone.now()
    agent, _created = Agent.objects.get_or_create(
        slug=agent_slug,
        defaults={"name": agent_slug},
    )

    if agent.is_active and agent.status != Agent.Status.DISABLED:
        agent.status = Agent.Status.OFFLINE if event == STATUS_OFFLINE else Agent.Status.ONLINE
    agent.last_seen_at = now
    agent.save(update_fields=["status", "last_seen_at", "updated_at"])

    AuditLog.objects.create(
        action=f"agent.{event}",
        object_type="Agent",
        object_id=str(agent.id),
        message=f"Agent {agent.slug} reported {event}",
        metadata={"agent": agent.slug, **(metadata or {})},
    )
    return agent
