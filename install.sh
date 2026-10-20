#!/bin/zsh

# compile
make re
echo

# install
echo "installing to ~/.local/bin/"
mkdir -p ~/.local/bin/ 

cp kd_picker ~/.local/bin/ 

# .desktop entry
echo "create .desktop entry"
eval df_loc="~/.local/share/applications/kd_picker.desktop"
echo "" > $df_loc
echo "[Desktop Entry]" >> $df_loc
echo "Name=kd_picker" >> $df_loc
echo "GenericName=core2k/anki2-kanjidamage supplement" >> $df_loc
echo "Exec=$HOME/.local/bin/kd_picker" >> $df_loc
echo "Type=Application" >> $df_loc
echo "Terminal=false" >> $df_loc

echo "done"
