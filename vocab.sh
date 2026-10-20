#!/bin/zsh

curl -L 'https://raw.githubusercontent.com/lucidvisionsnet/tango/master/lists/core_2k.csv' -o core_2k.csv
python3 convert_core_2k.py core_2k.csv vocab.tsv
rm -f core_2k.csv
