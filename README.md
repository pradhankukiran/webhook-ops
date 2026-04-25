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
- delivery attempts, retries, dead-letter state, replay-ready event storage
- Django Admin management views
- Docker Compose runtime with Postgres, Redis, web, worker, and beat services

Next phases:

- replay API and dashboard actions
- private delivery agent for local and internal services
- dashboard views for event search and delivery inspection
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

## Verification

```bash
uv run ruff check .
uv run python manage.py check
uv run pytest
docker compose config
```
