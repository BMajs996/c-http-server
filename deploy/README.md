# Run as a systemd service

The service runs as `c-http-server`, uses port 8080 and loopback by default, and
loads absolute paths. These are example installation commands for a Linux host
with systemd. Installation changes system files and should be done after local
testing; the project does not install or enable a service automatically.

## Build and install

Run from the repository root. Install OpenSSL 3 development headers before building.
Create the account once; reuse it if it already exists:

```sh
make
sudo useradd --system --user-group --home-dir /srv/c-http-server \
  --no-create-home --shell /usr/sbin/nologin c-http-server
sudo install -d -m 0755 /srv/c-http-server /srv/c-http-server/public
sudo install -d -o root -g c-http-server -m 0750 /etc/c-http-server
sudo install -m 0755 http_server /usr/local/bin/c-http-server
sudo cp -R public/. /srv/c-http-server/public/
sudo chown -R root:root /srv/c-http-server/public
sudo find /srv/c-http-server/public -type d -exec chmod 0755 {} +
sudo find /srv/c-http-server/public -type f -exec chmod 0644 {} +
sudo install -o root -g c-http-server -m 0640 deploy/server.conf.example \
  /etc/c-http-server/server.conf
sudo install -m 0644 deploy/c-http-server.service /etc/systemd/system/c-http-server.service
```

Edit `/etc/c-http-server/server.conf` for the target host. Numeric IPv4 and IPv6
addresses are supported. IPv6 listeners are IPv6-only; one process creates one
listener. Hostnames and scope suffixes such as `%eth0` are not accepted. Binding
`0.0.0.0` or `::` exposes the listener on all interfaces of that family, including
public health, readiness, metrics, and demonstration APIs. Configure network
access and HTTPS for the intended deployment. The service has no capability to
bind privileged ports; use an unprivileged port such as 8080.

## Credentials and certificates

Generate credentials with the tools described in the main README. Keep client
credential files on the client. Publish the combined server credential file with
owner `c-http-server`, mode 0600, outside the document root:

```sh
sudo install -o c-http-server -g c-http-server -m 0600 .secrets/server.credentials \
  /etc/c-http-server/server.credentials.new
sudo mv /etc/c-http-server/server.credentials.new /etc/c-http-server/server.credentials
```

Set `auth_credentials_file = /etc/c-http-server/server.credentials`. The
credential loader requires effective-user ownership, no group/other permissions,
a single hard link, and no final symlink. An empty configured credential file
revokes all IDs; an empty configuration setting keeps protected routes unavailable.

For HTTPS, place the certificate and key in a readable private directory under
`/etc/c-http-server/`, and configure both paths. The service account must be able
to read them; private keys should have restrictive permissions. Validate under
that account before starting. TLS certificate changes require a restart.

## Validate, start, and inspect

```sh
sudo -u c-http-server /usr/local/bin/c-http-server --check-config /etc/c-http-server/server.conf
sudo systemd-analyze verify /etc/systemd/system/c-http-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now c-http-server
systemctl status c-http-server
journalctl -u c-http-server -f
curl --fail http://127.0.0.1:8080/ready
```

For HTTPS probes, use the matching hostname and trusted CA, e.g.
`curl --fail --cacert /path/to/ca.pem https://localhost:8080/ready`.

`--check-config` validates syntax, address format, document-root access, TLS
loading, credential loading, and log destination access without a listener,
worker threads, notifications, or creating log files. It does not check port
availability or guarantee later allocations, filesystem access, or startup.
The environment variable `C_HTTP_ACCESS_LOG` overrides the configured log path
for validation and runtime alike. Validation outside systemd does not reproduce
its filesystem restrictions; service startup performs validation again inside
the configured execution environment.

The server sends `READY=1` only after initialization completes. `Type=notify`
keeps systemd startup pending until that message arrives. It sends `STOPPING=1`
when draining begins. `NOTIFY_SOCKET` is optional for manual runs; when supplied,
an invalid/unreachable/full notification socket prevents startup from announcing
readiness. Stopping notifications are best effort and do not delay shutdown.

## Reload and stop

```sh
sudo systemctl reload c-http-server
sudo systemctl stop c-http-server
```

Reload sends SIGHUP for authentication credentials only. The reload command
returns after delivering the signal; check the journal or `c_http_auth_reloads_total`
and `c_http_auth_reload_failures_total` to confirm the worker finished. Failed
reloads retain the previous credentials. Other configuration and TLS changes
require a restart. Replay protection resets on process restart.

On stop, readiness becomes false, the listener closes, idle/incomplete requests
close, and active responses drain for `shutdown_ms`. Fresh readiness probes then
usually fail to connect; a probe actually dispatched while unready returns 503.
`/health` remains the basic health endpoint. Readiness indicates initialization
and draining, not disk responsiveness, credential availability, or backend health.

The example uses a five-second drain deadline and a fifteen-second systemd stop
timeout, allowing bounded log flushing and normal worker cleanup. A stuck
filesystem operation can delay worker join beyond the socket deadline; systemd's
outer timeout then terminates the process. Adjust the unit timeout if you change
the drain period. `Restart=on-failure` restarts crashes, not intentional clean stops.

## Service restrictions

The service has a read-only system filesystem view and hidden home directories.
Install assets and secrets in the paths above. Logs go to the journal through a
bounded nonblocking writer, including when stderr is a Unix stream socket. For
file logging, create a dedicated writable directory and add an appropriate
`ReadWritePaths=` override; the supplied service does not grant one.

`LimitNOFILE=8192` allows headroom above the example's 1,024 connections for open
files, worker jobs, and control descriptors. Raise it together with resource
settings when needed. Restart rate limits prevent rapid repeated failure loops.
Socket activation, watchdog notifications, multiple listeners, and full
configuration reload are not implemented in this phase.
