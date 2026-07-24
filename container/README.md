= Running in containers

Examples are with the podman container environment

== Building containers

```
podman build --tag=tpx3-app --squash-all --file=tpx3app-container.docker
podman build --tag=tpx3-server --squash-all --file=server-container.docker
```

=== Running containers

```
podman run --rm --network=host --mount=type=bind,source=server_data,destination=/server_data,ro=true -it --name=tpx3-server tpx3-server -i/server_data/event-stream-synch-800Kcps-8TDC.raw -l/server_data/layout-8.json -c8
podman run --rm --network=host --mount=type=bind,source=output,destination=/output,rw=true -it --name=tpx3-app tpx3-app -S -ldebug
```

The --network=host lets the container use the same network namespace as the host.

Instead, it would be possible to create a separate network, or run within the same pod.

== Docker

The same containers work with Docker, with two syntax differences compared to podman:

- the `--mount` source path must be *absolute* (use `$PWD/...` from this directory),
- there is no `rw=true` mount option (read-write is the default; `ro=true` is supported).

=== Building containers

```
docker build --tag=tpx3-app --file=tpx3app-container.docker .
docker build --tag=tpx3-server --file=server-container.docker .
```

(`--squash-all` is podman-only; Docker builds work fine without it.)

=== Running containers

Run from this directory (`TimePixFly/container`). Start `tpx3-server` *first* —
`tpx3-app` contacts the ASI server at startup:

```
docker run --rm --network=host --mount=type=bind,source=$PWD/server_data,destination=/server_data,ro=true -d --name=tpx3-server tpx3-server -i/server_data/event-stream-synch-800Kcps-8TDC.raw -l/server_data/layout-8.json -c8
docker run --rm --network=host --mount=type=bind,source=$PWD/output,destination=/output -d --name=tpx3-app tpx3-app -S -ldebug
```

`-d` runs the containers detached; use `-it` instead to run in the foreground, and
`docker logs -f tpx3-app` to follow the logs of a detached container.

Verify that both are up:

```
curl -s http://127.0.0.1:8080/dashboard   # {"Server":{"SoftwareVersion":"t1"}}
curl -s http://127.0.0.1:8452/state       # {"type":"ProgramState","state":"config"}
```

=== Arguments

|=== | Argument | Meaning

| `tpx3-server -i<file>` | Raw event stream file to replay (one replay per `GET /measurement/start`)
| `tpx3-server -l<file>` | Detector layout JSON (layout-8.json: 8 chips — pixel maps must match this chip count)
| `tpx3-server -c<n>` | Number of chips | `tpx3-app -S` | Server mode: wait for REST commands (`?start=true`) on port
8452 instead of auto-starting | `tpx3-app -l<level>` | Log level (`debug`, `info`, ...)
|===

=== Restarting

If a container name is already taken (running or leftover), remove it first:

```
docker rm -f tpx3-server tpx3-app
```

Note that `tpx3-server` exits when a measurement is aborted mid-replay (its data sender stops on a broken pipe) —
symptom on the tpx3-app side is an `except` state with "ASI server PUT request for /server/destination failed -
Connection refused". Simply start `tpx3-server` again; `tpx3-app` can keep running and reconnects on the next
measurement. To have Docker revive it automatically, replace `--rm` with
`--restart=unless-stopped` (then stop it with `docker stop tpx3-server`).
