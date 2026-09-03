---
title: How do I apply security limits?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I apply security limits?

Set header/body limits, total request deadlines, allow rules, authentication, and rate limits at the narrowest site scope. Header and body deadlines are runtime-wide: they bound total read time and do not reset for each received byte. The existing `io_timeout` remains the idle-operation limit. Test allowed and denied requests; remove the policy to recover. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

~~~yaml
runtime:
  # Safe defaults: 10 seconds for headers and 60 seconds for a Content-Length body.
  request_header_timeout: 10
  request_body_timeout: 60
~~~

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

## Basic-auth credentials

Keep the password file owned by the Laghu service account and mode `0600`. New
records use this fixed, versioned scrypt format:

~~~text
username:scrypt-v1:16384:8:1:<16-byte-random-salt-hex>:<32-byte-derived-key-hex>
~~~

Create a record on a trusted operator workstation; the command prompts for the
password and emits one line for the protected file:

~~~sh
python3 - <<'PY'
import getpass, hashlib, os
username = input("Username: ")
password = getpass.getpass("Password: ").encode()
salt = os.urandom(16)
derived = hashlib.scrypt(password, salt=salt, n=16384, r=8, p=1, dklen=32)
print(f"{username}:scrypt-v1:16384:8:1:{salt.hex()}:{derived.hex()}")
PY
~~~

Laghu still reads legacy `username:sha256:<64-hex>` records for a controlled
migration, but they are deprecated. Replace them manually with new scrypt
records during credential rotation; Laghu never rewrites password files.

Basic credentials require HTTPS. Terminate TLS directly in Laghu, or accept
`X-Forwarded-Proto: https` only from the exact CIDRs of a TLS terminator:

~~~yaml
runtime:
  respect_x_forwarded_proto: on
  trusted_proxy:
    - 10.20.30.40/32
~~~

Do not enable that setting for an untrusted network peer. Requests carrying
Basic credentials over clear HTTP, or forwarded protocol headers from outside
`trusted_proxy`, are denied.

## Authentication admission limits

Protect each Basic-auth scope from password-derivation floods with these
runtime YAML values (the CLI uses the same names with `--` instead of `_`):

~~~yaml
runtime:
  # Defaults shown. All values must be positive integers.
  auth_pre_rate: 4
  auth_pre_burst: 8
  auth_kdf_rate: 4
  auth_kdf_burst: 8
  auth_kdf_concurrency: 2
~~~

`auth_pre_*` is a bounded token bucket keyed by the direct TCP peer and the
matched Basic-auth scope; it runs before decoding credentials. `auth_kdf_*`
is process-global: it limits scrypt starts per second, its startup burst, and
simultaneous scrypt work. Direct peer identity uses only the IPv4 or IPv6
socket address, never forwarded headers. Rejections return `429` with
`preauth_rate` or `kdf_saturated`; invalid credentials remain `401`.

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
