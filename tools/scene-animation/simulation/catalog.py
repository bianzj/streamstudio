"""One machine-readable inventory of implemented features and their limits."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CATALOG = Path(__file__).with_name('capabilities.json')


def load_catalog():
    return json.loads(CATALOG.read_text(encoding='utf-8'))


def validate_catalog():
    errors = []
    ids = set()
    for feature in load_catalog()['features']:
        if feature['id'] in ids:
            errors.append('Duplicate feature: ' + feature['id'])
        ids.add(feature['id'])
        for key in ('sources', 'checks'):
            for relative in feature[key]:
                path = (ROOT / relative).resolve()
                if not path.is_relative_to(ROOT) or not path.is_file():
                    errors.append('Missing/outside repository: ' + relative)
    return errors
