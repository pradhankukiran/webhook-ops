<div align="center">

# WebhookOps

**A single-user, self-hosted webhook operations platform for reliable ingestion, delivery, retry, and replay of third-party webhooks.**

[![CI](https://img.shields.io/github/actions/workflow/status/pradhankukiran/webhook-ops/ci.yml?branch=main&style=flat-square&label=CI&logo=github)](https://github.com/pradhankukiran/webhook-ops/actions/workflows/ci.yml)
[![Python](https://img.shields.io/badge/python-3.12+-3776AB.svg?style=flat-square&logo=python&logoColor=white)](https://www.python.org/)
[![Django](https://img.shields.io/badge/django-5.2-092E20.svg?style=flat-square&logo=django&logoColor=white)](https://www.djangoproject.com/)
[![DRF](https://img.shields.io/badge/DRF-3.15-A30000.svg?style=flat-square)](https://www.django-rest-framework.org/)
[![Celery](https://img.shields.io/badge/celery-5.4-37814A.svg?style=flat-square&logo=celery&logoColor=white)](https://docs.celeryq.dev/)
[![PostgreSQL](https://img.shields.io/badge/postgres-16-336791.svg?style=flat-square&logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![Redis](https://img.shields.io/badge/redis-7-DC382D.svg?style=flat-square&logo=redis&logoColor=white)](https://redis.io/)
[![C++](https://img.shields.io/badge/C++-20-00599C.svg?style=flat-square&logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![Docker](https://img.shields.io/badge/docker-ready-2496ED.svg?style=flat-square&logo=docker&logoColor=white)](https://www.docker.com/)
[![uv](https://img.shields.io/badge/uv-managed-261230.svg?style=flat-square&logo=astral&logoColor=white)](https://github.com/astral-sh/uv)
[![Ruff](https://img.shields.io/badge/code%20style-ruff-D7FF64.svg?style=flat-square&logo=ruff&logoColor=black)](https://docs.astral.sh/ruff/)

</div>

WebhookOps receives webhooks from providers like Stripe, GitHub, Shopify, and Slack,
persists every event before delivery, deduplicates redeliveries, and forwards each event
to a public HTTPS endpoint or a private app reachable through a connected reverse-tunnel
agent — with retries, dead-lettering, and replay built in.

---

## Features

- **Provider-aware ingestion** with HMAC signature verification for Generic, GitHub, Shopify, Slack, and Stripe sources
- **Idempotent storage** — every raw payload is persisted before delivery, deduplicated on the provider's event id
- **Public HTTP delivery** with configurable timeouts, TLS verification, and forwarded headers
- **Private agent delivery** through a custom C++ reverse tunnel for apps behind NAT or firewalls
- **Per-agent HMAC authentication** — each enrolled agent has its own secret, verified centrally by Django
- **Retries with exponential backoff** capped at one hour, automatic dead-lettering after `max_attempts`
- **Replay** events from the Django admin (bulk action) or via REST API; replay status is tracked end-to-end
- **Audit log** of every state transition (ingested, delivered, failed, dead-lettered, replayed, agent auth)
- **Operator UI** via Django Admin and a typed REST API documented through OpenAPI / Swagger
- **One-command deploy** with Docker Compose (web, worker, beat, tunnel, Postgres, Redis)

## Architecture

```text
                  +------------------+
   provider --->  |  Django ingest   |  --->  PostgreSQL (events, attempts, audit)
                  +--------+---------+
                           |
                           v
                  +------------------+
                  |  Celery worker   |  <---  Redis (broker + beat scheduler)
                  +--------+---------+
                           |
            +--------------+--------------+
            |                             |
            v                             v
     public HTTPS URL              C++ tunnel proxy
                                          |
                                          v
                                   webhookops-agent
                                          |
                                          v
                                  private internal app
```

## Tech Stack

| Layer            | Components                                                                                |
| ---------------- | ----------------------------------------------------------------------------------------- |
| Control plane    | Python 3.12, Django 5.2, Django REST Framework 3.15, drf-spectacular, django-filter       |
| Async pipeline   | Celery 5.4 (worker + beat), Redis 7                                                       |
| Datastore        | PostgreSQL 16 (SQLite for local-only development)                                         |
| Reverse tunnel   | C++20, OpenSSL, CMake 3.20+, custom binary frame protocol with HMAC-SHA256 challenge auth |
| Tooling          | `uv` (deps), `ruff` (lint), `pytest` + `pytest-django` (tests), Docker & Docker Compose   |
| API surface      | OpenAPI 3 schema at `/api/schema/`, Swagger UI at `/api/docs/`                            |

## Quick Start (Docker)

The fastest way to run the full stack — Postgres, Redis, web, worker, beat, and tunnel —
is via Docker Compose.

```bash
docker compose up --build
```

In a second terminal, create an admin user:

```bash
docker compose exec web python manage.py createsuperuser
```

Open the admin: <http://localhost:8000/admin/>

| Service | Port | Purpose                                       |
| ------- | ---- | --------------------------------------------- |
| web     | 8000 | Django (admin, API, ingestion endpoint)       |
| db      | 5432 | PostgreSQL 16                                 |
| redis   | 6379 | Celery broker + result backend                |
| tunnel  | 9700 | Reverse-tunnel control port (agents dial in)  |
| tunnel  | 9080 | Internal HTTP proxy used by the Celery worker |

## Local Development

Native Python workflow with [`uv`](https://github.com/astral-sh/uv):

```bash
uv sync --dev                                  # install deps
uv run python manage.py migrate                # apply migrations
uv run python manage.py createsuperuser        # create admin user
uv run python manage.py runserver              # start web
uv run celery -A webhook_ops worker -l info    # in another terminal
uv run celery -A webhook_ops beat   -l info    # in another terminal (retry scheduler)
```

Build the native tunnel binaries:

```bash
cmake -S agent -B agent/build
cmake --build agent/build
```

## Sending Your First Webhook

In Django Admin:

1. Create a `Destination` with mode **Public HTTP** and a URL you control.
2. Create a `Webhook source` with slug `generic`.
3. Assign the destination as the source's default destination.

Send a test webhook:

```bash
curl -X POST http://localhost:8000/in/generic/ \
  -H 'Content-Type: application/json' \
  -H 'X-Webhook-Id: evt_001' \
  -d '{"event": "invoice.paid", "invoice_id": "inv_001"}'
```

The event is stored, queued, delivered by Celery, and visible in Django Admin
along with every delivery attempt.

## Private Agent Delivery

Private delivery lets WebhookOps reach an app that is only routable from a connected
agent machine — for example a service on a developer's laptop or behind a corporate NAT.

```text
WebhookOps worker -> tunnel proxy -> connected agent -> localhost / internal app
```

In Django Admin:

1. Create an `Agent`.
2. Set its `Allowed targets`, for example `["localhost:8000"]`.
3. `POST /api/agents/<id>/enrollment/` to generate a per-agent run command.

The enrollment response contains the **only** plaintext copy of that agent's tunnel
secret. Generating a new enrollment command rotates the agent secret.

Run the tunnel service on the WebhookOps host:

```bash
agent/build/webhookops-tunnel run --config agent/configs/tunnel.conf.example
```

Run the agent near the private app:

```bash
agent/build/webhookops-agent join \
  --tunnel YOUR_WEBHOOKOPS_HOST:9700 \
  --id dev-machine \
  --secret AGENT_SPECIFIC_SECRET_FROM_ENROLLMENT \
  --allow localhost:8000
```

The tunnel calls Django to authenticate the agent's HMAC proof before accepting the
connection (`POST /internal/tunnel/agent-auth/`) and posts heartbeat / online / offline
events back (`POST /internal/tunnel/agent-status/`) to keep the `Agent` row in sync.

Then create a Django Admin `Destination`:

```text
Mode:  Private Agent
URL:   http://localhost:8000/webhooks/stripe
Agent: dev-machine
```

When a webhook is delivered to that destination, the Celery worker posts through
`WEBHOOKOPS_PRIVATE_PROXY_URL`. With Docker Compose this defaults to
`http://tunnel:9080`; for native local development it defaults to
`http://127.0.0.1:9080`.

## Configuration

All settings are read from environment variables (or a `.env` file) via `django-environ`.
A starter file lives at [`.env.example`](./.env.example).

| Variable                              | Purpose                                                          |
| ------------------------------------- | ---------------------------------------------------------------- |
| `SECRET_KEY`                          | Django secret key                                                |
| `DEBUG`                               | Toggle Django debug mode                                         |
| `ALLOWED_HOSTS`, `CSRF_TRUSTED_ORIGINS` | Standard Django host / CSRF settings                           |
| `DATABASE_URL`                        | Postgres URL (defaults to local SQLite if unset)                 |
| `REDIS_URL`                           | Redis URL used by Celery broker and result backend               |
| `CELERY_BROKER_URL`, `CELERY_RESULT_BACKEND` | Override the broker / backend independently if needed     |
| `WEBHOOKOPS_PRIVATE_PROXY_URL`        | URL of the C++ tunnel's HTTP proxy (worker uses this)            |
| `WEBHOOKOPS_PUBLIC_TUNNEL_ENDPOINT`   | Public `host:port` advertised in agent enrollment commands       |
| `WEBHOOKOPS_TUNNEL_AUTH_TOKEN`        | Bearer token the tunnel presents on `/internal/tunnel/agent-auth/`   |
| `WEBHOOKOPS_TUNNEL_STATUS_TOKEN`      | Bearer token the tunnel presents on `/internal/tunnel/agent-status/` |

## Project Structure

```text
webhook-ops/
├── agent/                       # C++ reverse tunnel (binaries: webhookops-tunnel, webhookops-agent)
│   ├── CMakeLists.txt
│   ├── Dockerfile
│   ├── configs/                 # example tunnel.conf and agent.conf
│   └── src/
│       ├── agent/               # private-side connector
│       ├── tunnel/              # public-side controller + HTTP proxy
│       └── common/              # shared protocol, channel, crypto, config, log
├── webhook_ops/                 # Django project
│   ├── settings.py
│   ├── urls.py
│   ├── celery.py
│   └── webhooks/                # the single business app
│       ├── models.py            # Agent, Destination, WebhookSource, WebhookEvent, ...
│       ├── ingestion.py         # idempotent event ingestion
│       ├── signatures.py        # per-provider HMAC verifiers
│       ├── delivery.py          # public + private-agent delivery, retry, dead-letter
│       ├── tasks.py             # Celery tasks (deliver + retry sweep)
│       ├── agents.py            # enrollment + per-agent HMAC auth
│       ├── replay.py            # replay flow
│       ├── api.py               # DRF ViewSets
│       ├── views.py             # public ingestion + tunnel callbacks
│       ├── admin.py             # Django Admin (incl. bulk replay action)
│       └── tests/
├── docker-compose.yml
├── Dockerfile
├── pyproject.toml
└── .env.example
```

## Testing & Quality

```bash
uv run ruff check .                    # lint
uv run python manage.py check          # Django system check
uv run pytest                          # full test suite
cmake -S agent -B agent/build && \
  cmake --build agent/build            # build native tunnel binaries
docker compose config                  # validate compose stack
```

CI runs the same steps on every push and pull request — see
[`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Roadmap

WebhookOps starts as a single-user product. Multi-tenant organizations, billing, team
roles, and plan limits are intentionally out of scope until the core delivery path is
solid.

**Implemented**

- Django control plane and PostgreSQL data model
- Webhook ingestion endpoint at `/in/<source-slug>/`
- Provider-aware idempotency extraction and signature verification
- Public HTTP delivery and private-agent delivery
- Agent enrollment, per-agent tunnel authentication, online / offline / heartbeat sync
- Delivery attempts, retries, dead-letter state, end-to-end replay (admin bulk action + REST API)
- Django Admin management views
- Docker Compose runtime with Postgres, Redis, web, worker, beat, and tunnel services

**Next phases**

- Dashboard views for event search and delivery inspection
- Agent secret rotation and revocation UI
- Production deployment hardening
