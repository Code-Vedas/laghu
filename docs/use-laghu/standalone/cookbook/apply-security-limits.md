---
title: How do I apply security limits?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I apply security limits?

Set header/body limits, allow rules, authentication, and rate limits at the narrowest site scope. Test allowed and denied requests; remove the policy to recover. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

~~~yaml
sites:
  - host: shop.example.test
    laghu:
      request_header_limit: 32k
      request_body_limit: 1m
      allow: [10.0.0.0/8]
      rate_limit: 20
      rate_burst: 40
~~~

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
