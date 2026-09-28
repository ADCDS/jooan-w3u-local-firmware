#!/usr/bin/env python3
"""Manage the camera's PTZ presets over its ONVIF service.

NVRs such as Frigate recall presets but cannot save them, and the camera's
Web UI no longer drives the head. This is the owner's tool for that: list,
save the current position, recall, delete, and nudge to aim before saving.

    tools/onvif_presets.py camera.local list
    tools/onvif_presets.py camera.local nudge left
    tools/onvif_presets.py camera.local save "Front gate"
    tools/onvif_presets.py camera.local goto "Front gate"
    tools/onvif_presets.py camera.local delete 3

The password is read from JOAN_PASSWORD or prompted for; only its WS-Security
digest is sent. The camera speaks plain HTTP, so use this on the camera's own
network. Standard library only.
"""
import argparse, base64, datetime, getpass, hashlib, html, http.client, os, re, sys

PTZ = 'http://www.onvif.org/ver20/ptz/wsdl'


def call(host, password, operation, body=''):
    nonce = os.urandom(16)
    created = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + password.encode()).digest()).decode()
    envelope = (
        '<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"'
        f' xmlns:p="{PTZ}"><s:Header><Security xmlns="http://docs.oasis-open.org/wss/2004/01/'
        'oasis-200401-wss-wssecurity-secext-1.0.xsd"><UsernameToken><Username>admin</Username>'
        f'<Password>{digest}</Password><Nonce>{base64.b64encode(nonce).decode()}</Nonce>'
        '<Created xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">'
        f'{created}</Created></UsernameToken></Security></s:Header><s:Body><p:{operation}>'
        f'<p:ProfileToken>ch0</p:ProfileToken>{body}</p:{operation}></s:Body></s:Envelope>')
    connection = http.client.HTTPConnection(host, timeout=30)
    connection.request('POST', '/onvif/ptz', envelope.encode(), {'Content-Type': 'application/soap+xml; charset=utf-8'})
    response = connection.getresponse()
    reply = response.read().decode('utf-8', 'replace')
    if response.status != 200:
        reason = re.search(r'<[\w:]*Text[^>]*>([^<]*)<', reply)
        sys.exit(f'{operation}: {html.unescape(reason.group(1)) if reason else f"HTTP {response.status}"}')
    return reply


def presets(host, password):
    reply = call(host, password, 'GetPresets')
    return [(token, html.unescape(name)) for token, name in
            re.findall(r'<[\w:]*Preset token="(\d+)"><[\w:]*Name>([^<]*)</', reply)]


def token_for(host, password, which):
    if which.isdigit():
        return which
    for token, name in presets(host, password):
        if name.lower() == which.lower():
            return token
    sys.exit(f'no preset named {which!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('host', help='camera address, optionally host:port')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list', help='list presets')
    save = sub.add_parser('save', help='save the current position as a preset')
    save.add_argument('name')
    save.add_argument('--token', help='re-save this existing slot instead of a new one')
    for name in ('goto', 'delete'):
        sub.add_parser(name, help=f'{name} a preset by token or name').add_argument('preset')
    sub.add_parser('nudge', help='move one coarse step').add_argument('direction', choices=('left', 'right', 'up', 'down'))
    args = parser.parse_args()
    password = os.environ.get('JOAN_PASSWORD') or getpass.getpass('Camera admin password: ')

    if args.command == 'list':
        for token, name in presets(args.host, password):
            print(f'{token}  {name}')
    elif args.command == 'save':
        token = f'<p:PresetToken>{html.escape(args.token)}</p:PresetToken>' if args.token else ''
        reply = call(args.host, password, 'SetPreset', f'<p:PresetName>{html.escape(args.name)}</p:PresetName>{token}')
        print('saved as', re.search(r'PresetToken>(\d+)<', reply).group(1))
    elif args.command == 'goto':
        call(args.host, password, 'GotoPreset', f'<p:PresetToken>{token_for(args.host, password, args.preset)}</p:PresetToken>')
        print('moving')
    elif args.command == 'delete':
        call(args.host, password, 'RemovePreset', f'<p:PresetToken>{token_for(args.host, password, args.preset)}</p:PresetToken>')
        print('deleted')
    else:
        x, y = {'left': (-1, 0), 'right': (1, 0), 'up': (0, 1), 'down': (0, -1)}[args.direction]
        call(args.host, password, 'ContinuousMove', f'<p:Velocity><PanTilt xmlns="http://www.onvif.org/ver10/schema" x="{x}" y="{y}"/></p:Velocity>')


if __name__ == '__main__':
    main()
