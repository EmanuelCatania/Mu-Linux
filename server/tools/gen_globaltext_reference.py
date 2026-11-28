"""Genera Data/Text/Reference/<idioma>/GlobalText.txt desde el Text.bmd del cliente.

El server no lee estos archivos: son la referencia del editor de textos del
panel (el texto original de cada fila, para mostrarlo y para chequear que la
traducción traiga los mismos %d/%s).  Las traducciones van en
Data/Text/<idioma>/GlobalText.txt.

Uso (desde la raíz del repo):
    python server/tools/gen_globaltext_reference.py
"""

import os
import sys

ROWS = 1000
ROW_SIZE = 300
BUX = (0xFC, 0xCF, 0xAB)
LANGS = {'eng': 'Eng', 'spn': 'Spn', 'por': 'Por'}

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCE = os.path.join(ROOT, 'client', 'bin', 'Client', 'Content', 'text', '{}', 'Text.bmd')
TARGET = os.path.join(ROOT, 'server', 'MuServer', 'Data', 'Text', 'Reference', '{}', 'GlobalText.txt')


def decode_row(raw):
    raw = raw.split(b'\0', 1)[0]
    # Como el cliente (NormalizeTextFileInPlace): lo que ya es UTF-8 se deja y
    # el resto es Windows-1252.
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError:
        return raw.decode('cp1252', errors='replace')


def main():
    for folder, lang in LANGS.items():
        data = bytearray(open(SOURCE.format(folder), 'rb').read())
        for i in range(len(data)):
            data[i] ^= BUX[i % 3]
        lines = [
            '//Textos originales del Text.bmd del cliente (%s). Los genera' % lang,
            '//server/tools/gen_globaltext_reference.py; el server no los lee. Son la',
            '//referencia del editor de textos del panel: las traducciones van en',
            '//Data/Text/%s/GlobalText.txt.' % lang,
            '//Fila\tTexto',
        ]
        skipped = 0
        for row in range(ROWS):
            text = decode_row(bytes(data[row * ROW_SIZE:(row + 1) * ROW_SIZE]))
            if text == '':
                continue
            # El formato del archivo no admite comillas ni saltos de línea: esas
            # filas no se pueden traducir desde el panel.
            if any(c in text for c in '"\r\n\t') or any(ord(c) < 32 for c in text):
                skipped += 1
                continue
            lines.append('%d\t"%s"' % (row, text))
        lines.append('end')
        path = TARGET.format(lang)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\r\n') as out:
            out.write('\n'.join(lines) + '\n')
        print('%s: %d filas (%d sin poder traducirse)' % (path, len(lines) - 6, skipped))


if __name__ == '__main__':
    sys.exit(main())
