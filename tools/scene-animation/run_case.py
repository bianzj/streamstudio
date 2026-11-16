"""Unified scene tools: discover features, check a job, or execute existing code."""
import argparse
import json
import sys
from simulation.catalog import load_catalog, validate_catalog
from simulation.jobs import prepare_job, execute_job


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    features = commands.add_parser('features', help='List implemented features and limitations')
    features.add_argument('--json', action='store_true')
    for name in ('check', 'run'):
        commands.add_parser(name, help='Read-only preflight' if name == 'check' else 'Execute into a new directory').add_argument('job')
    args = parser.parse_args()
    try:
        if args.command == 'features':
            errors = validate_catalog()
            if errors: raise ValueError('; '.join(errors))
            catalog = load_catalog()
            if args.json: print(json.dumps(catalog, ensure_ascii=True, indent=2))
            else:
                for item in catalog['features']:
                    print(f'{item["id"]}: {item["name"]} [{item["execution"]}]\n  {item["summary"]}\n  限制：{item["limitations"]}')
                print('\n未实现：' + '；'.join(catalog['notImplemented']))
            return 0
        plan = prepare_job(args.job)
        result = plan if args.command == 'check' else execute_job(plan)
        print(json.dumps(result, ensure_ascii=True, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as exc:
        print(f'{type(exc).__name__}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    raise SystemExit(main())
