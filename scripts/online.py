#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Co-op addresses (PS_ONLINE=1): the game's own server, the shadNet server and its WebAPI.

From run.py and the launcher of the Windows port of bbport. Messages,
bloodstains and ghosts come from the community game server (the title's, Seamless Hunt's for
Bloodborne); bells and summons go through a shadNet server (Seamless Hunt's by default:
shadPS4's public one, srv.shadps4.net, accepts only shadPS4's own builds since 2026-10-12).

`online.py overrides --user DIR --title-online TEXT` writes DIR/host_overrides.json for this
launch and prints its path for SHADPS4_HTTP_HOST_OVERRIDES_JSON. TEXT is what
`psport --online-overrides --game <dir>` prints for the game (its title's servers, key=value
lines): the game's server -> the community server; its summon route -> the shadNet WebAPI when
that has the summon service. A title without servers gets an empty file, no overrides, so that a
file left by another game's launch does not apply. A player who needs other addresses puts them
in DIR/host_overrides.custom.json, which is then used as it is, for any game.

`online.py local-server --user DIR --title-online TEXT`: the game server of the title's co-op
extension (BBSC's for Bloodborne) to run on this PC, for run.sh: its port, its console log and its
command (one argument per line), or nothing. Only when the extension is on (its switch, PS_BBSC=1)
and its local switch (PS_BBSC_LOCAL_SERVER) is 1, or auto and the community game server does not
answer its status path within 5 s (Seamless Hunt's timed out for players on 2026-10-14 while the
rest of it worked); unset or 0 never. Its data and logs go to DIR/<extension>/; it sends summons
to the shadNet server's WebAPI, so messages and bloodstains are then this PC's only.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request

# The launcher's default (it is built around Bloodborne); a launch takes its title's
# (psport --online-overrides).
COMMUNITY_SERVER = 'http://play.seamlesshunt.com'
SHADNET_SERVER = 'play.seamlesshunt.com:31313'
# Saved launcher settings that still hold these earlier defaults get the current ones.
OLD_DEFAULTS = {
    'online_server': ('srv.shadps4.net:31313',),
    'online_community': ('https://thehuntersdream.com',),
}


def title_online(text):
    """psport --online-overrides' key=value lines as a dict; empty for a title without servers."""
    return dict(line.split('=', 1) for line in str(text or '').splitlines() if '=' in line)


def community_address(value, default=COMMUNITY_SERVER):
    """The game server as scheme://host (the game picks the ports), the default's when unset.
    Without a scheme: https, but the default's own for its host (Seamless Hunt serves plain http)."""
    value = str(value or '').strip() or default
    scheme, _sep, rest = value.rpartition('://')
    host = rest.split('/')[0].split(':')[0]
    default_scheme, _sep, default_host = default.partition('://')
    return f'{scheme or (default_scheme if host == default_host else "https")}://{host}'


def webapi_address(server):
    """The shadNet WebAPI of a server address host:port (the server's default port 31315)."""
    server = str(server or '').strip() or SHADNET_SERVER
    host = server.rsplit(':', 1)[0] if server.count(':') == 1 else server
    return f'http://{host}:31315'


def webapi_setting(webapi, server, community):
    """The WebAPI field, unless it holds the game server's address (a common mix-up): then the
    shadNet server's own WebAPI."""
    webapi = str(webapi or '').strip()
    community_host = community_address(community).split('://', 1)[1]
    if webapi and community_host in webapi:
        webapi = ''
    return webapi or webapi_address(server)


def summon_service(webapi, route):
    """True when a shadNet WebAPI answers the title's summon calls: the route exists there
    (an empty request is refused with 400), where other servers answer 404."""
    request = urllib.request.Request(webapi.rstrip('/') + route + '/create', data=b'',
                                     method='POST')
    try:
        urllib.request.urlopen(request, timeout=4).close()
        return True
    except urllib.error.HTTPError as error:
        return error.code != 404
    except (OSError, ValueError):
        return False


def write_overrides(user_dir, title, community, webapi):
    """The host overrides file for this launch; returns its path."""
    user_dir = Path(user_dir)
    user_dir.mkdir(parents=True, exist_ok=True)
    custom = user_dir / 'host_overrides.custom.json'
    if custom.exists():
        print(f'Co-op: addresses from {custom}', file=sys.stderr)
        return custom
    overrides = {}
    if title.get('game_server'):
        host = community.split('://', 1)[1]
        overrides[title['game_server']] = community
        route, port = title.get('summon_route'), title.get('community_api_port')
        if route and port and summon_service(webapi, route):
            overrides.update({f'{scheme}://{host}:{port}{route}': webapi
                              for scheme in ('https', 'http')})
            print(f'Co-op: summon signs through {webapi}', file=sys.stderr)
        elif route and port:
            print(f'Co-op: {webapi} has no summon service; summon signs through {community}',
                  file=sys.stderr)
    else:
        print('Online: the title names no servers of its own; no host overrides', file=sys.stderr)
    path = user_dir / 'host_overrides.json'
    path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    return path


def check(server, webapi, community):
    """(name, address, ok, detail) for the game server, the shadNet server and its WebAPI."""
    results = []
    try:
        with urllib.request.urlopen(community + ':20443/bb-eu/ss.info', timeout=6) as reply:
            reply.read(64)
        results.append(('game', community, True, ''))
    except (OSError, ValueError) as error:
        results.append(('game', community, False, str(error)))
    host, _sep, port = server.rpartition(':')
    try:
        socket.create_connection((host or server, int(port or 31313)), timeout=4).close()
        results.append(('shadnet', server, True, ''))
    except (OSError, ValueError) as error:
        results.append(('shadnet', server, False, str(error)))
    try:
        with urllib.request.urlopen(webapi.rstrip('/') + '/status', timeout=4) as reply:
            ok = b'"ok":true' in reply.read(512).replace(b' ', b'')
        results.append(('webapi', webapi, ok, '' if ok else 'not a shadNet server'))
    except (OSError, ValueError) as error:
        results.append(('webapi', webapi, False, str(error)))
    return results


def local_server(title, user_dir, community, server):
    """[port, console log, script, arguments...] for the title extension's game server on this PC,
    or [] (see the module's description)."""
    script, local_switch = title.get('local_server'), title.get('local_switch')
    extension_switch = title.get('extension_switch')
    if not script or not local_switch or not extension_switch:
        return []
    if os.environ.get(extension_switch, '').strip() != '1':
        return []
    mode = os.environ.get(local_switch, '').strip().lower()
    if mode in ('', '0', 'off', 'no'):
        return []
    if mode not in ('1', 'on', 'yes', 'auto'):
        print(f'Co-op: {local_switch}={mode} is not 1, auto or 0: no game server on this PC',
              file=sys.stderr)
        return []
    try:
        port = urllib.parse.urlsplit(title.get('game_server', '')).port
    except ValueError:
        port = None
    if not port:
        print('Co-op: the title names no game server port: no game server on this PC',
              file=sys.stderr)
        return []
    if mode == 'auto':
        host = community.split('://', 1)[1]
        if host in ('127.0.0.1', 'localhost'):
            return []
        status = f'{community}:{port}{title.get("status_path") or "/"}'
        try:
            with urllib.request.urlopen(status, timeout=5) as reply:
                reply.read(64)
            return []
        except (OSError, ValueError) as error:
            print(f'Co-op: {status} does not answer ({error}): the game server runs on this PC',
                  file=sys.stderr)
    shadnet = server.rsplit(':', 1)[0] if server.count(':') == 1 else server
    data = Path(user_dir) / title.get('extension', 'extension')
    (data / 'logs').mkdir(parents=True, exist_ok=True)
    return [str(port), str(data / 'logs' / 'gameserver-console.log'), script,
            '--shadnet', shadnet, '--host', '127.0.0.1', '--port', str(port),
            '--data', str(data / 'online-data.json'), '--log-dir', str(data / 'logs'),
            *shlex.split(title.get('local_server_args', ''))]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    overrides = sub.add_parser('overrides')
    overrides.add_argument('--user', required=True)
    overrides.add_argument('--title-online', default='')
    sub.add_parser('check')
    local = sub.add_parser('local-server')
    local.add_argument('--user', required=True)
    local.add_argument('--title-online', default='')
    a = parser.parse_args()
    server = os.environ.get('PS_SHADNET_SERVER') or SHADNET_SERVER
    title = title_online(getattr(a, 'title_online', ''))
    community = community_address(os.environ.get('PS_COMMUNITY_SERVER'),
                                  title.get('community_server') or COMMUNITY_SERVER)
    webapi = webapi_setting(os.environ.get('PS_SHADNET_WEBAPI'), server, community)
    if a.command == 'local-server':
        for line in local_server(title, a.user, community, server):
            print(line)
        return
    if a.command == 'overrides':
        print(write_overrides(a.user, title, community, webapi))
    else:
        for name, address, ok, detail in check(server, webapi, community):
            print(f'{name}: {address}: {"reachable" if ok else "not reachable"} {detail}'.rstrip())


if __name__ == '__main__':
    main()
