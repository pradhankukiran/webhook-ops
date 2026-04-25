# WebhookOps

WebhookOps is a single-user webhook operations platform for reliable delivery,
inspection, retry, and replay of third-party webhooks.

The first production slice focuses on one operator running their own instance:

- receive webhooks from services such as Stripe, GitHub, Shopify, or Slack
- persist raw events before delivery
- deduplicate repeated provider deliveries
- send events to public HTTPS destinations or a connected private agent
- retry failed deliveries and move exhausted events to a dead-letter state
- inspect delivery attempts and replay events safely

## Architecture

```text
provider webhook
      |
      v
WebhookOps Django app
      |
      +--> PostgreSQL event store
      +--> Celery delivery worker
      |
      v
public destination or private tunnel agent
```

## Initial Scope

WebhookOps starts as a single-user product. Multi-tenant organizations, billing,
team roles, and plan limits are intentionally out of scope until the core
delivery path is solid.

## Current Phase

Implemented:

- Django control plane
- PostgreSQL-compatible data model
- webhook ingestion endpoint at `/in/<source-slug>/`
- provider-aware idempotency extraction
- signature verification for generic, GitHub, Shopify, Slack, and Stripe-style requests
- public HTTP delivery through Celery
- private agent delivery through a reverse tunnel proxy
- agent enrollment command generation
- tunnel-driven agent online/heartbeat/offline status sync
- delivery attempts, retries, dead-letter state, replay-ready event storage
- Django Admin management views
- Docker Compose runtime with Postgres, Redis, web, worker, beat, and tunnel services

Next phases:

- dashboard views for event search and delivery inspection
- per-agent tunnel authentication instead of a shared tunnel secret
- production deployment hardening

## Local Development

Install dependencies:

```bash
uv sync --dev
```

Apply migrations:

```bash
uv run python manage.py migrate
```

Create an admin user:

```bash
uv run python manage.py createsuperuser
```

Run Django:

```bash
uv run python manage.py runserver
```

Run a worker in another terminal:

```bash
uv run celery -A webhook_ops worker --loglevel=info
```

Run the retry scheduler in another terminal:

```bash
uv run celery -A webhook_ops beat --loglevel=info
```

## Docker Runtime

Start the full stack:

```bash
docker compose up --build
```

Then create an admin user:

```bash
docker compose exec web python manage.py createsuperuser
```

Open:

```text
http://localhost:8000/admin/
```

## First Webhook

In Django Admin:

1. Create a `Destination` with mode `Public HTTP` and a URL you control.
2. Create a `Webhook source` with slug `generic`.
3. Assign the destination as the source's default destination.

Send a test webhook:

```bash
curl -X POST http://localhost:8000/in/generic/ \
  -H 'Content-Type: application/json' \
  -H 'X-Webhook-Id: evt_001' \
  -d '{"event": "invoice.paid", "invoice_id": "inv_001"}'
```

The event will be stored, queued, delivered by Celery, and visible in Django
Admin with its delivery attempts.

## Private Agent Delivery

Private delivery lets WebhookOps deliver webhooks to an app that is only reachable
from a connected agent machine.

```text
WebhookOps worker -> tunnel proxy -> connected agent -> localhost/internal app
```

In Django Admin:

1. Create an `Agent`.
2. Set its `Allowed targets`, for example `["localhost:8000"]`.
3. Use the API action `POST /api/agents/<id>/enrollment/` to generate the run command.

Build the native tunnel binaries:

```bash
cmake -S agent -B agent/build
cmake --build agent/build
```

Run the tunnel service on the WebhookOps host:

```bash
agent/build/webhookops-tunnel run --config agent/configs/tunnel.conf.example
```

Run the agent near the private app:

```bash
agent/build/webhookops-agent join \
  --tunnel YOUR_WEBHOOKOPS_HOST:9700 \
  --id dev-machine \
  --secret dev-secret \
  --allow localhost:8000
```

The tunnel posts status events to:

```text
/internal/tunnel/agent-status/
```

Those events update the agent's `status` and `last_seen_at` fields in Django.

Then create a Django Admin `Destination`:

```text
Mode: Private Agent
URL:  http://localhost:8000/webhooks/stripe
Agent: dev-machine
```

When a webhook is delivered to that destination, the Celery worker posts through
`WEBHOOKOPS_PRIVATE_PROXY_URL`. With Docker Compose this defaults to
`http://tunnel:9080`; for native local development it defaults to
`http://127.0.0.1:9080`.

## Verification

```bash
uv run ruff check .
uv run python manage.py check
uv run pytest
cmake -S agent -B agent/build
cmake --build agent/build
docker compose config
```
