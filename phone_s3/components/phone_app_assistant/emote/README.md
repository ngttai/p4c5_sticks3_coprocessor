# Emote pack

Eye animations for the AI Assistant app, in EAF format (360 x 360, 4-bit, Huffman): the 21 files of the
`eaf_360x360` folder of Espressif's [Emote Pack.zip](https://dl.espressif.com/AE/Emote%20Pack.zip), linked from
[espressif2022/esp_emote_gen_player](https://github.com/espressif2022/esp_emote_gen_player), that the 22 emotion
names of the Xiaozhi protocol use (`assistant_emote.cpp` has the table). The other 10 of the pack (badminton,
music, mute, ...) are not used. Espressif publishes the pack as open source sample material of
`esp_emote_gen_player`, a component under Apache-2.0; the pack itself has no license file.

A memory card can replace any of them: `/sdcard/appdata/assistant/emote/<emotion>.eaf` (for example `happy.eaf`), or
`pack_<name>.eaf` as named in the pack, so a folder of the pack (`eaf_466x466`, ...) can be copied as it is.
A card set that lacks a name (a set made for the 21 emotions of the protocol has no `idle`, shown while listening)
plays the card's `neutral` instead, never a built-in face mixed into another character; the built-in set plays only
when the card has no `neutral` either.

Sizes: 360 x 360 is decoded straight into the face. Bigger ones, up to 480 x 480, are decoded aside and shrunk to
360 once per frame (nearest pixel); scaling them in LVGL instead is done in software on every redraw. Measured with
a 400 x 400 set: core 0 about 80% (LVGL scaling), 43% (shrunk after decoding), 28-30% for the same set made 360 x 360.
Smaller ones are enlarged by LVGL.
