"""Publish only a complete draft uploaded after successful replacement GUI checks."""
import json
import os
from pathlib import Path
from urllib.parse import quote
from urllib.request import Request, urlopen

repository, tag = os.environ['GITHUB_REPOSITORY'], os.environ['RELEASE_TAG']
base = f'https://api.github.com/repos/{repository}'
headers = {'Authorization': 'Bearer ' + os.environ['GH_TOKEN'], 'Accept': 'application/vnd.github+json'}
with urlopen(Request(base + '/releases/tags/' + quote(tag, safe=''), headers=headers), timeout=30) as response:
    release = json.load(response)
expected = {path.name for path in Path('release-payload').iterdir() if path.is_file()}
assets = release['assets']
if not release['draft'] or len(expected) != 8 or {item['name'] for item in assets} != expected or any(item['state'] != 'uploaded' for item in assets):
    raise SystemExit('Refusing to publish an incomplete or unexpected release payload')
body = json.dumps({'draft': False, 'make_latest': 'false' if release['prerelease'] else 'true'}).encode()
with urlopen(Request(base + '/releases/' + str(release['id']), data=body,
                     headers={**headers, 'Content-Type': 'application/json'}, method='PATCH'), timeout=30) as response:
    published = json.load(response)
if published['draft']:
    raise SystemExit('GitHub did not publish the verified draft')
print(published['html_url'])
