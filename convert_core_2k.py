import argparse
import csv
import html
import re
import sys
from pathlib import Path

KANJI_RANGES = (
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
    (0x20000, 0x2FA1F),
)


def is_kanji_character(character: str) -> bool:
    codepoint = ord(character)
    return any(start <= codepoint <= end for start, end in KANJI_RANGES)


def is_kana_character(character: str) -> bool:
    codepoint = ord(character)
    return (
        0x3041 <= codepoint <= 0x3096
        or 0x30A1 <= codepoint <= 0x30F6
        or codepoint == 0x30FC
    )


def is_single_kanji_only(word: str) -> bool:
    return len(word) == 1 and is_kanji_character(word)


def is_valid_written_form(word: str) -> bool:
    if not word or is_single_kanji_only(word):
        return False
    has_kanji = False
    for character in word:
        if is_kanji_character(character):
            has_kanji = True
        elif not is_kana_character(character):
            return False
    return has_kanji


def is_kana_reading(reading: str) -> bool:
    return bool(reading) and all(is_kana_character(character) for character in reading)


def strip_html(value: str) -> str:
    value = re.sub(r"<[^>]*>", "", value)
    return html.unescape(value).replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


parser = argparse.ArgumentParser(
    description=(
        "convert CORE2k CSV to TSV, allowing words with kanji plus "
        "hiragana/katakana while excluding single-kanji-only entries."
    )
)
parser.add_argument("input_csv", help="path to CSV ")
parser.add_argument(
    "output_tsv",
    nargs="?",
    default="vocab.tsv",
    help="output TSV path",
)
args = parser.parse_args()

input_path = Path(args.input_csv).expanduser()
output_path = Path(args.output_tsv).expanduser()
if not input_path.is_file():
    print(f"input CSV does not exist: {input_path}", file=sys.stderr)
    exit()

input_rows = 0
kept_rows = 0
skipped_shape = 0
skipped_written = 0
skipped_single_kanji = 0
skipped_reading = 0
seen_rows = set()

with input_path.open("r", encoding="utf-8-sig", newline="") as input_file:
    reader = csv.reader(input_file)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.writer(output_file, delimiter="\t", lineterminator="\n")
        for row in reader:
            input_rows += 1
            if len(row) < 3:
                skipped_shape += 1
                continue
            written = strip_html(row[0])
            reading = strip_html(row[1])
            meaning = strip_html(row[2])
            if input_rows == 1 and written.lower() in {"word", "vocabulary", "expression", "表記"}:
                continue
            if is_single_kanji_only(written):
                skipped_single_kanji += 1
                continue
            if not is_valid_written_form(written):
                skipped_written += 1
                continue
            if not is_kana_reading(reading):
                skipped_reading += 1
                continue
            if not meaning:
                meaning = "(meaning unavailable)"
            item = (written, reading, meaning)
            if item in seen_rows:
                continue
            seen_rows.add(item)
            writer.writerow(item)
            kept_rows += 1

print(f"rows read: {input_rows}")
print(f"mixed-script vocabulary rows written: {kept_rows}")
print(f"skipped rows without 3 columns: {skipped_shape}")
print(f"skipped single-kanji-only entries: {skipped_single_kanji}")
print(f"skipped unsupported written forms: {skipped_written}")
print(f"skipped non-kana readings: {skipped_reading}")
