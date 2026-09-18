"""Cross-check docker/docker-compose.yml against docker/Dockerfile.

The two files reference each other by name in half a dozen places and drift
silently. Every mismatch below builds cleanly and fails only when someone runs
the stack -- and since the first build takes 15-25 minutes, discovering it then
is expensive. This catches them in under a second:

  * every build.target names a stage that exists in the Dockerfile
  * context + dockerfile resolve to a real file
  * every depends_on names a real service
  * the healthcheck's binary is actually COPYed into that service's stage
  * EVERY STAGE'S ENTRYPOINT BINARY IS PRESENT IN THAT STAGE, and is one the
    builder actually produces -- copying the wrong binary into a stage builds
    perfectly cleanly and then fails at `docker run` with "executable file not
    found"
  * each client's --server host is a real service and its port matches that
    service's --listen
  * the aggregator publishes its listen port and listens on 0.0.0.0 rather than
    loopback, which no other container could reach

WHAT THIS DOES NOT DO: it is not `docker compose config`. It does not validate
the Compose schema -- unknown keys, deprecated syntax and version-specific
semantics are not checked. It only verifies that the two files agree with each
other. Run `docker compose config` as well when the Compose plugin is available.

Parses the indentation-based YAML subset these files actually use, rather than
depending on a YAML library that may not be installed.

Usage:  python3 tools/check_compose.py      (exit 0 = consistent)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPOSE = os.path.join(ROOT, "docker/docker-compose.yml")
DOCKERFILE = os.path.join(ROOT, "docker/Dockerfile")

def parse(path):
    """Minimal YAML subset: nested maps, block lists, scalars."""
    root = {}
    stack = [(-1, root)]
    for raw in open(path):
        line = raw.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        text = line.strip()
        while stack and indent <= stack[-1][0]:
            stack.pop()
        parent = stack[-1][1]
        if text.startswith("- "):
            parent.setdefault("__list__", []).append(text[2:].strip().strip('"'))
            continue
        if ":" not in text:
            continue
        key, _, value = text.partition(":")
        key, value = key.strip(), value.strip()
        if value:
            # Flow-style list, e.g. test: ["CMD", "status_client", ...]
            if value.startswith("[") and value.endswith("]"):
                inner = value[1:-1].strip()
                parent[key] = [v.strip().strip('"').strip("'")
                               for v in inner.split(",")] if inner else []
            else:
                parent[key] = value.strip('"')
        else:
            child = {}
            parent[key] = child
            stack.append((indent, child))
    return root

def flatten(node):
    """A block list under a key becomes that key's value."""
    if isinstance(node, dict):
        if set(node) == {"__list__"}:
            return node["__list__"]
        return {k: flatten(v) for k, v in node.items() if k != "__list__"}
    return node

compose = flatten(parse(COMPOSE))
df = open(DOCKERFILE).read()

stages = set(re.findall(r"^FROM\s+\S+\s+AS\s+(\S+)", df, re.M))
copies = re.findall(r"^COPY\s+--from=builder\s+(.+?)\s+(/\S+)\s*$", df, re.M)
# stage -> binaries present, by walking the file in order
stage_bins, current = {}, None
for line in df.splitlines():
    m = re.match(r"^FROM\s+(\S+)\s+AS\s+(\S+)", line)
    if m:
        current = m.group(2)
        stage_bins[current] = set(stage_bins.get(m.group(1), set()))
        continue
    m = re.match(r"^COPY\s+--from=builder\s+(.+?)\s+/\S+\s*$", line)
    if m and current:
        for p in m.group(1).split():
            stage_bins[current].add(os.path.basename(p))

# ENTRYPOINT per stage: the binary each image actually runs.
stage_entry, current = {}, None
for line in df.splitlines():
    m = re.match(r"^FROM\s+(\S+)\s+AS\s+(\S+)", line)
    if m:
        current = m.group(2)
        if m.group(1) in stage_entry:
            stage_entry[current] = stage_entry[m.group(1)]
        continue
    m = re.match(r'^ENTRYPOINT\s+\["([^"]+)"', line)
    if m and current:
        stage_entry[current] = m.group(1)

# Every binary the builder copies out must be one Bazel actually produces.
built = set(re.findall(r"bazel-bin/\S+/(\S+)\b", df))

services = compose.get("services", {})
problems, checks = [], 0

def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        problems.append(msg)

print(f"Dockerfile stages: {', '.join(sorted(stages))}")
print(f"compose services : {', '.join(services)}\n")

for name, svc in services.items():
    build = svc.get("build", {})
    target = build.get("target")
    check(target in stages,
          f"{name}: build.target '{target}' is not a stage in the Dockerfile")
    ctx, dockerfile = build.get("context"), build.get("dockerfile")
    resolved = os.path.normpath(os.path.join(os.path.dirname(COMPOSE), ctx or "", dockerfile or ""))
    check(os.path.exists(resolved), f"{name}: dockerfile path does not resolve: {resolved}")

    for dep in (svc.get("depends_on", {}) or {}):
        check(dep in services, f"{name}: depends_on names unknown service '{dep}'")

    hc = svc.get("healthcheck", {})
    if hc:
        test = hc.get("test", [])
        binary = test[1] if len(test) > 1 else None
        check(binary in stage_bins.get(target, set()),
              f"{name}: healthcheck runs '{binary}' but stage '{target}' does not contain it")

    cmd = svc.get("command", [])
    server = next((c for c in cmd if isinstance(c, str) and c.startswith("--server=")), None)
    if server:
        host, _, port = server.split("=", 1)[1].partition(":")
        check(host in services, f"{name}: --server points at '{host}', not a service name")
        agg = services.get(host, {})
        listen = next((c for c in agg.get("command", []) if c.startswith("--listen=")), "")
        check(listen.endswith(":" + port),
              f"{name}: --server port {port} does not match {host}'s {listen}")

# Each image must contain the binary its ENTRYPOINT names -- the classic
# Dockerfile mistake is copying the wrong binary into a stage, which builds
# cleanly and fails at `docker run` with "executable file not found".
for name, svc in services.items():
    target = svc.get("build", {}).get("target")
    entry = stage_entry.get(target)
    check(entry is not None, f"{name}: stage '{target}' defines no ENTRYPOINT")
    if entry:
        check(entry in stage_bins.get(target, set()),
              f"{name}: ENTRYPOINT '{entry}' is not copied into stage '{target}' "
              f"(has: {sorted(stage_bins.get(target, set())) or 'nothing'})")
        check(entry in built,
              f"{name}: ENTRYPOINT '{entry}' is not among the binaries the builder produces")

# The aggregator must publish the port its clients and host mapping expect.
agg = services.get("aggregator", {})
listen = next((c for c in agg.get("command", []) if c.startswith("--listen=")), "")
ports = agg.get("ports", [])
check(any(p.split(":")[-1] == listen.rpartition(":")[2] for p in ports),
      f"aggregator: ports {ports} do not publish the listen port in {listen}")
check(listen.startswith("--listen=0.0.0.0:"),
      "aggregator: must listen on 0.0.0.0 to be reachable from other containers")

print(f"{checks} cross-file checks run")
if problems:
    print(f"\n{len(problems)} PROBLEM(S):")
    for p in problems:
        print("  -", p)
    sys.exit(1)
print("compose and Dockerfile agree")
