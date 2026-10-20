# kd_picker

this program filters your existing anki2 kanjidamage deck for cards in the 'young' or 'mature' state and 
matches them to eligible vocabulary words using a supplied vocabulary file.

use `kd_picker.conf` to specify file paths.

run `./vocab.sh` to download and filter the CORE 2.3k deck.

required libraries -- `libsqlite3-dev libx11-dev libxft-dev fonts-noto-cjk`

# build

```
make
./kanji_word_picker
```

# controls

- `1`: show reading
- `2`: show english translation
- `n/space/enter`: next word
- `r`: hide answers
- `q/esc`: quit.
