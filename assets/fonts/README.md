# Menu fonts

`menu_font.bin` holds signed distance field atlases of two Noto Sans styles, used by the in-game
settings menu (`src/host/ui_gl.c`): Condensed Black Italic for titles and items, Condensed
SemiBold for help text. Noto Sans is licensed under the SIL Open Font License 1.1 (`OFL.txt`).

Rebuild it with `tools/font/make_menu_font.py` (needs Pillow and numpy):

```bash
python3 tools/font/make_menu_font.py assets/fonts/menu_font.bin \
  NotoSans-CondensedBlackItalic.ttf NotoSans-CondensedSemiBold.ttf
```
