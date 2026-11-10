#!/bin/sh
# Puts the browser build (build-web/web/) on the public relay's computer, served at https://netkart64.duckdns.org/.
#
# nginx (tools/web-host/nginx.conf) serves the files from ~/netkart64-web/html on the Pi; the Pi's Traefik adds
# HTTPS and sends WebSocket requests for the same name to the relay, so the page's default relay address,
# wss://netkart64.duckdns.org/, is its own server. mk64.o2r is never copied: the page asks the player for theirs.
#
# Usage: tools/web-deploy.sh   (after cmake --build build-web)
set -eu

PI=${PI:-pi@192.168.8.190}
SSH="ssh -o HostKeyAlias=raspberrypi.local"
REMOTE_DIR=netkart64-web
NAME=netkart64.duckdns.org

cd "$(dirname "$0")/.."
test -f build-web/web/index.wasm || { echo "build-web/web/ has no build: cmake --build build-web" >&2; exit 1; }

$SSH "$PI" "mkdir -p $REMOTE_DIR/html"
rsync -az --delete --exclude mk64.o2r -e "$SSH" build-web/web/ "$PI:$REMOTE_DIR/html/"
rsync -az -e "$SSH" tools/web-host/nginx.conf "$PI:$REMOTE_DIR/nginx.conf"

# The relay's router matches the bare host name; this one's longer rule wins for everything but WebSockets.
$SSH "$PI" sh -s <<EOF
docker rm -f netkart64-web >/dev/null 2>&1 || true
docker run -d --name netkart64-web --network proxy --restart unless-stopped \\
  -v \$HOME/$REMOTE_DIR/html:/usr/share/nginx/html:ro \\
  -v \$HOME/$REMOTE_DIR/nginx.conf:/etc/nginx/conf.d/default.conf:ro \\
  --label traefik.enable=true \\
  --label 'traefik.http.routers.netkart64-web.rule=Host(\`$NAME\`) && !HeaderRegexp(\`Upgrade\`, \`(?i)websocket\`)' \\
  --label traefik.http.routers.netkart64-web.entrypoints=websecure \\
  --label traefik.http.routers.netkart64-web.tls.certresolver=le \\
  --label traefik.http.services.netkart64-web.loadbalancer.server.port=80 \\
  nginx:alpine
EOF
echo "Deployed: https://$NAME/"
