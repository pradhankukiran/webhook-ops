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
