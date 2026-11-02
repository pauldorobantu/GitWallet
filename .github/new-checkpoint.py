# Makes the "checkpoint" of Bitcoin in res/coins.json: the last block of a period well behind the
# tip, as every listed server and two explorers name it. Run by hand before a release; a newer one
# means fewer headers read at a first start. Prints what to paste, or says what disagrees.
import hashlib
import json
import socket
import ssl
import sys
import urllib.request

SERVERS = [s for n in json.load(open('res/coins.json', encoding='utf-8'))['networks'] if n['id'] == 'bitcoin' for s in n['servers']]
PERIOD = 2016


def call(sock_file, sock, number, method, *params):
    sock.sendall(json.dumps({'jsonrpc': '2.0', 'id': number, 'method': method, 'params': list(params)}).encode() + b'\n')
    reply = json.loads(sock_file.readline())
    if reply.get('error'):
        raise RuntimeError(reply['error'])
    return reply['result']


def hash_of(header):
    return hashlib.sha256(hashlib.sha256(header).digest()).digest()[::-1].hex()


def read_from(server, height):
    host, port = server.rsplit(':', 1)
    with socket.create_connection((host, int(port)), timeout=20) as raw:
        with ssl.create_default_context().wrap_socket(raw, server_hostname=host) as sock:
            sock_file = sock.makefile('rb')
            call(sock_file, sock, 1, 'server.version', 'GitWallet checkpoint', ['1.4', '1.4.2'])
            tip = call(sock_file, sock, 2, 'blockchain.headers.subscribe')['height']
            if height is None:
                height = (tip - 1000) // PERIOD * PERIOD - 1
            root = call(sock_file, sock, 3, 'blockchain.block.header', 0, height)['root']
            last = bytes.fromhex(call(sock_file, sock, 4, 'blockchain.block.header', height))
            first = bytes.fromhex(call(sock_file, sock, 5, 'blockchain.block.header', height - PERIOD + 1))
            after = bytes.fromhex(call(sock_file, sock, 6, 'blockchain.block.header', height + 1))
            return height, root, last, first, after


def explorer(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'GitWallet check'}), timeout=30) as reply:
        return reply.read().decode().strip()


height = None
answers = {}
for server in SERVERS:
    try:
        height, root, last, first, after = read_from(server, height)
        answers[server] = (root, hash_of(last), last, first, after)
        print(server, 'says', root, hash_of(last))
    except Exception as why:
        print(server, 'did not answer:', why)
if len(answers) < 5 or len({a[0] for a in answers.values()}) != 1 or len({a[1] for a in answers.values()}) != 1:
    sys.exit('the servers do not agree, or too few answered')
root, name, last, first, after = next(iter(answers.values()))
for url in (f'https://blockstream.info/api/block-height/{height}', f'https://mempool.space/api/block-height/{height}'):
    said = explorer(url)
    print(url, 'says', said)
    if said != name:
        sys.exit('an explorer names another block')
print(json.dumps({'height': height, 'hash': name, 'headersRoot': root, 'bits': last[72:76][::-1].hex(),
                  'time': int.from_bytes(last[68:72], 'little'), 'periodStart': int.from_bytes(first[68:72], 'little'),
                  'nextBits': after[72:76][::-1].hex()}, indent=2))
