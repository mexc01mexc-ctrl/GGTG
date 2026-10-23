#!/usr/bin/env python3
"""Regenerates docs/FILE_INDEX.md: every GGTG source file of the desktop
(macOS / Windows) and Android apps with its size and the first lines of its
header comment, so a person or an AI can find the right file without reading
the whole tree.

    python3 tools/update_file_index.py

Run it after adding, renaming or re-describing a GGTG file. The description is
taken from the comment at the top of the file, so keep that comment current.
"""
import os
import re

GGTG = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
DESKTOP = os.path.join(GGTG, 'desktop/nicegram-desktop/Telegram/SourceFiles')
ANDROID = os.path.join(
    GGTG, 'android/Telegram-GGTG/TMessagesProj/src/main/java/app/ggtg')
OUT = os.path.join(GGTG, 'docs/FILE_INDEX.md')

# Desktop files that are GGTG's own (the rest of SourceFiles is Telegram's).
DESKTOP_PREFIXES = (
    'ggtg', 'sms_', 'bulk_accounts', 'process_box', 'process_manager',
    'scraped_members', 'session_duplicate', 'proxy_pool', 'views_campaign',
    'account_spam_status')
SKIP = ('This file is part of Telegram', 'LEGAL', 'For license',
        'https://github.com/telegram', 'Copyright')


def describe(path, limit=260):
    try:
        lines = open(path, encoding='utf-8', errors='ignore').read().splitlines()
    except OSError:
        return ''
    out = []
    started = False
    for line in lines[:90]:
        s = line.strip()
        if any(k in s for k in SKIP):
            continue
        if s.startswith('//') or (started and s.startswith('*')):
            text = s.lstrip('/').lstrip('*').strip()
            if text.startswith(('ng:', 'ggtg:')):
                text = text.split(':', 1)[1].strip()
            if text:
                out.append(text)
                started = True
            if len(' '.join(out)) > limit:
                break
        elif started:
            break
    text = ' '.join(out)
    text = re.sub(r'\s+', ' ', text).replace('|', '/')
    return (text[:limit] + '…') if len(text) > limit else text


def rows(root, accept):
    result = []
    for folder, _, files in sorted(os.walk(root)):
        for name in sorted(files):
            path = os.path.join(folder, name)
            if not accept(name):
                continue
            rel = os.path.relpath(path, root)
            size = max(1, os.path.getsize(path) // 1024)
            result.append(f'| `{rel}` | {size} KB | {describe(path)} |')
    return result


def main():
    desktop = rows(DESKTOP, lambda n: n.endswith(('.h', '.cpp', '.mm', '.style'))
                   and n.startswith(DESKTOP_PREFIXES))
    android = rows(ANDROID, lambda n: n.endswith(('.kt', '.java')))
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('# Указатель файлов GGTG\n\n')
        f.write('Сгенерировано `tools/update_file_index.py` из вступительных '
                'комментариев файлов. Не править руками: поправьте комментарий '
                'в файле и запустите скрипт.\n\n')
        f.write('## Desktop (macOS и Windows): '
                '`desktop/nicegram-desktop/Telegram/SourceFiles/`\n\n')
        f.write('| Файл | Размер | Что внутри |\n|---|---|---|\n')
        f.write('\n'.join(desktop) + '\n\n')
        f.write('## Android: '
                '`android/Telegram-GGTG/TMessagesProj/src/main/java/app/ggtg/`\n\n')
        f.write('| Файл | Размер | Что внутри |\n|---|---|---|\n')
        f.write('\n'.join(android) + '\n')
    print(f'{OUT}: {len(desktop)} desktop + {len(android)} android files')


if __name__ == '__main__':
    main()
