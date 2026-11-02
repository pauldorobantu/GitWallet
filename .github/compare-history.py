# What GitWallet's history says of each address against a public block explorer. Exit 1 on a
# difference. A scan's report is the input; if the explorer cannot be asked, it says so and exits 0.
import collections
import json
import sys
import time
import urllib.error
import urllib.request

API = 'https://blockstream.info/api'
HEADERS = {'User-Agent': 'GitWallet check'}


class CannotAsk(Exception):
    pass


def get(url):
    for attempt in range(6):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=30) as reply:
                return json.load(reply)
        except urllib.error.HTTPError as error:
            if error.code not in (429, 503):
                raise CannotAsk(str(error))
        except Exception:
            pass
        time.sleep(5 * (attempt + 1))
    raise CannotAsk('no answer from ' + url)


def transactions_of(address):
    found = []
    last = None
    while True:
        page = get(f'{API}/address/{address}/txs' + (f'/chain/{last}' if last else ''))
        confirmed = [t for t in page if t['status'].get('confirmed')]
        found += confirmed if last else page
        if len(confirmed) < 25:
            return found
        last = confirmed[-1]['txid']
        time.sleep(0.2)


def main():
    report = json.load(open(sys.argv[1], encoding='utf-8'))
    here = {t['txid']: t for t in report['history']}
    effects = collections.defaultdict(dict)
    for t in report['history']:
        for effect in t['effects']:
            effects[effect['address']][t['txid']] = (int(effect['received']), int(effect['spent']))
    addresses = sorted({row['address'] for row in report['rows'] if row['network'] == 'bitcoin' and row['used']})
    differences = 0
    compared = set()
    try:
        for address in addresses:
            theirs = transactions_of(address)
            time.sleep(0.2)
            # Anyone can pay this seed: what is newer than the scan says nothing.
            newer = {t['txid'] for t in theirs if not t['status'].get('confirmed') or t['status'].get('block_height', 0) > report['historyState'][0]['tip']}
            if {t['txid'] for t in theirs} - newer != set(effects[address]) - newer:
                differences += 1
                print('differs: the list of', address)
            for t in theirs:
                mine = here.get(t['txid'])
                if not mine or t['txid'] not in effects[address] or t['txid'] in newer:
                    continue
                received = sum(v['value'] for v in t['vout'] if v.get('scriptpubkey_address') == address)
                spent = sum(v['prevout']['value'] for v in t['vin'] if v.get('prevout') and v['prevout'].get('scriptpubkey_address') == address)
                status = t['status']
                wrong = (received, spent) != effects[address][t['txid']]
                wrong = wrong or (status.get('confirmed') and (mine['height'] != status['block_height'] or int(mine['time']) != status['block_time']))
                wrong = wrong or (int(mine['fee']) >= 0 and int(mine['fee']) != t['fee'])
                wrong = wrong or mine['vsize'] != (t['weight'] + 3) // 4
                if wrong:
                    differences += 1
                    print('differs:', t['txid'], 'on', address)
                compared.add(t['txid'])
    except CannotAsk as why:
        print('the explorer could not be asked:', why)
        return 1 if differences else 0
    print(f'compared {len(compared)} of {len(here)} transactions on {len(addresses)} addresses, {differences} different')
    return 1 if differences else 0


sys.exit(main())
