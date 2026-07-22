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
