#!/usr/bin/env python3
"""Select a shared work config and keep one version across a release batch."""
import argparse
import datetime as dt
import os
from pathlib import Path
import re
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
TARGETS = ('53xx', '60xx', '807x', '95xx')
VERSION_LINE = re.compile(r'^CONFIG_VERSION_NUMBER="(\d{8})-r([1-9]\d*)"$', re.M)


def versions(configs):
    found = []
    for name, text in configs.items():
        matches = list(VERSION_LINE.finditer(text))
        if len(matches) != 1 or text.count('CONFIG_VERSION_NUMBER=') != 1:
            raise ValueError(f'{name}: require exactly one YYYYMMDD-rN version')
        date, revision = matches[0].groups()
        dt.datetime.strptime(date, '%Y%m%d')
        found.append((date, int(revision)))
    return found


def release_version(configs, today, reuse=False):
    current = versions(configs)
    if any(date > today for date, _ in current):
        raise ValueError('config version is later than the current Shanghai date')
    if reuse:
        if len(set(current)) != 1 or current[0][0] != today:
            raise ValueError('reuse requires all shared configs to agree on today\'s release')
        return f'{today}-r{current[0][1]}'
    revision = max((rev for date, rev in current if date == today), default=0) + 1
    return f'{today}-r{revision}'


def with_version(text, version):
    text, count = VERSION_LINE.subn(f'CONFIG_VERSION_NUMBER="{version}"', text)
    if count != 1:
        raise ValueError('missing or duplicate version')
    for option in ('CONFIG_IMAGEOPT', 'CONFIG_VERSIONOPT'):
        text = re.sub(rf'^(?:{option}=.*|# {option} is not set)\n?', '', text, flags=re.M)
        text += f'{option}=y\n'
    return text


def atomic_write(path, text):
    temporary = path.with_name(path.name + '.qcb-release.tmp')
    with temporary.open('x') as stream:
        stream.write(text)
    if path.exists():
        temporary.chmod(path.stat().st_mode & 0o777)
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=TARGETS, required=True)
    parser.add_argument('--reuse-release', action='store_true',
                        help='reuse today\'s synchronized version for another target in this batch')
    args = parser.parse_args()
    paths = {target: WORK / f'config-{target}.txt' for target in TARGETS}
    configs = {target: path.read_text() for target, path in paths.items()}
    today = dt.datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y%m%d')
    version = release_version(configs, today, args.reuse_release)
    updated = {target: with_version(text, version) for target, text in configs.items()}
    # Validate all inputs before writing any of the release configurations.
    for target, text in updated.items():
        if f'CONFIG_TARGET_' not in text or f'ipq{target}=y' not in text:
            raise ValueError(f'{paths[target]}: target does not match filename')
    for target, path in paths.items():
        if updated[target] != configs[target]:
            atomic_write(path, updated[target])
    atomic_write(ROOT / '.config', updated[args.target])
    print(f'{version} source={paths[args.target]} destination={ROOT / ".config"}')


if __name__ == '__main__':
    main()
