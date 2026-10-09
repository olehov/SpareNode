# Authentication transport and network exposure

SpareNode derives security-sensitive request metadata from the accepted TCP peer and an explicit
deployment mode. The server never treats forwarding headers from an arbitrary client as proof of
HTTPS or client identity.

This policy protects the future login and session-cookie flow. Native TLS, certificate issuance,
and reverse-proxy lifecycle management are outside SpareNode v0.1; public deployments terminate TLS
at an operator-managed reverse proxy.

## Deployment modes

`transport_mode` selects one of three boundaries:

| Mode | Intended exposure | Accepted direct peer | Effective scheme and client |
|---|---|---|---|
| `loopback_http` | Local development or a same-host client | IPv4 `127.0.0.0/8` or IPv6 `::1` | Plain HTTP; client is the socket peer |
| `trusted_lan_http` | A network whose clients and links the operator explicitly trusts | Any numeric IP peer | Plain HTTP; client is the socket peer |
| `trusted_proxy_https` | HTTPS through one reverse proxy | The exact numeric `trusted_proxy` address | HTTPS and client address from validated proxy headers |

`loopback_http` is the default. It requires a loopback `bind` address, and the default bind is
`127.0.0.1`. `trusted_lan_http` is an explicit acceptance of plaintext credentials and session
traffic on the selected network. It should not be used across an untrusted Wi-Fi network, routed
network, or the public Internet.

Public Internet access requires `trusted_proxy_https`. The browser connects to the proxy over
HTTPS, and the proxy forwards requests over a restricted backend connection to SpareNode. The
backend listener must not be exposed as a second public entry point.

## Trusted proxy contract

Proxy mode requires a single numeric IPv4 or IPv6 `trusted_proxy` value. SpareNode compares the
accepted socket peer with that address as a parsed numeric address, so equivalent IPv6 spellings
match. Connections from every other peer are closed before request processing.

Each request from the trusted peer must contain exactly one of each header:

- `X-Forwarded-Proto: https`
- `X-Forwarded-For: <one numeric IPv4 or IPv6 address>`

Missing, repeated, comma-separated, hostname, or otherwise malformed values are rejected. A
forwarded scheme other than `https` is rejected with `403 Forbidden`; malformed proxy metadata is
rejected with `400 Bad Request`. In direct modes these headers are ignored and cannot change the
effective client or scheme.

The proxy must replace client-supplied `X-Forwarded-Proto` and `X-Forwarded-For` fields rather than
append to them. It must send one sanitized value for each field. Restrict the backend with host
firewall rules, a private interface, a container network, or a loopback listener when the proxy is
on the same host.

TLS protects only the client-to-proxy connection. Authentication, authorization, CSRF/origin
checks, session hardening, and login throttling remain independently required. The same validated
transport context applies to login, authenticated reads, downloads, uploads, directory creation,
and deletion.

## Configuration examples

Local access uses the defaults explicitly shown here:

```conf
server {
    bind "127.0.0.1";
    transport_mode "loopback_http";

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

An operator who accepts plaintext traffic on a trusted LAN can opt in:

```conf
server {
    bind "192.0.2.10";
    transport_mode "trusted_lan_http";

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

A same-host TLS proxy uses loopback for both the listener and trusted peer:

```conf
server {
    bind "127.0.0.1";
    transport_mode "trusted_proxy_https";
    trusted_proxy "127.0.0.1";

    location "/api/Documents" {
        path "/srv/documents";
    }
}
```

If the proxy runs on another host or an isolated container network, bind SpareNode to that private
interface and set `trusted_proxy` to the proxy's numeric backend address. Wildcard binds (`0.0.0.0`
and `::`) are rejected in proxy mode because they make accidental backend exposure too easy.

For example, an nginx TLS virtual host can replace both forwarded fields before proxying to a
same-host SpareNode listener:

```nginx
server {
    listen 443 ssl;
    server_name files.example.com;

    # Configure certificate and key paths according to the nginx installation.
    ssl_certificate     /etc/ssl/files.example.com/fullchain.pem;
    ssl_certificate_key /etc/ssl/files.example.com/private.key;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header X-Forwarded-For $remote_addr;
    }
}
```

The backend configuration for that topology uses `bind "127.0.0.1"`,
`transport_mode "trusted_proxy_https"`, and `trusted_proxy "127.0.0.1"`. The operator must also
redirect or reject public port 80 traffic and verify that port 8080 is unreachable from other
hosts. Certificate provisioning, redirect policy, and proxy updates remain the operator's
responsibility.

## Effective request context

After parsing a complete request, the connection handler attaches an immutable transport context:

- `peer_address`: the direct TCP peer;
- `client_address`: the effective client after the trust policy;
- `secure`: whether the client-facing request was validated as HTTPS;
- `trusted_proxy`: whether the effective metadata came from the configured proxy.

Authentication audit logs use the effective client address only after this validation. Session
cookie generation must derive its transport decision only from `secure`, never directly from a
forwarding header. A cookie created for a request with `secure == true` must include the `Secure`
attribute. A direct plaintext mode has `secure == false`; its cookie omits `Secure` so localhost or
an explicitly trusted LAN remains usable, but the operator accepts that the cookie, credentials,
metadata, and file contents are visible and modifiable in transit. Plaintext modes are therefore
unsupported for public Internet access. A future native TLS mode must provide the same context
without trusting forwarding headers.

The remaining cookie attributes and the cookie serialization tests belong to SN-107, which consumes
this validated context. SN-108 supplies the authoritative effective-HTTPS decision and prevents a
client from enabling `Secure` semantics by spoofing a header.

## Platform behavior

The policy is identical on Windows and Linux. Both platforms parse numeric IPv4 and IPv6 addresses
through their native socket APIs, recognize IPv4 `127.0.0.0/8` and IPv6 `::1` as loopback, compare
trusted addresses by their binary value, and apply the same forwarding-header rules. Platform
differences are limited to the underlying address-parsing API and do not change configuration or
request semantics. CI builds and exercises the policy on both operating systems.

## Startup failures

SpareNode rejects unsafe combinations before opening the listener:

- a non-loopback bind in `loopback_http` mode;
- a missing, invalid, or repeated proxy address in `trusted_proxy_https` mode;
- `trusted_proxy` in either direct mode;
- a wildcard bind in proxy mode;
- an unsupported or repeated transport mode.

These are fatal configuration errors. SpareNode does not silently downgrade HTTPS assumptions or
fall back to trusting request headers.
