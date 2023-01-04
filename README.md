
# GLideN64

*A next generation* ***Graphics Plugin*** *for* ***N64*** *emulators.*

---

## About

Modified version of GLideN64 that allows for scene ripping from N64 games.

Features 34 different rip modes, vertex color, fog information, and CSV export support.

Developed on RMG 0.2.5 and Blender 3.3.1. Included python script should work on 2.80+.

Continuous ripping support is planned to be added for animation ripping. Pull requests are welcome.

How to use
---

1. Download/build and replace existing GLideN64 video plugin with the one in this repo.
2. 'Enable Scene Ripping' support in GLide settings tab
3. Enable 'Use texture pack' in Texture enchancement tab, keep Enchancement option set to 'None'
4. Enable and set a hotkey for 'Toggle Textures Dump' option under hotkeys tab.
5. Start a game, go into the area you want to rip.
6. Press toggle texture dump hotkey, make sure you get a confirmation it is on.
7. Enter GLideN64's debugger by pressing Scroll Lock then Insert key buttons.
8. Press Home button to trigger a dump. You'll get a confirmation it worked.
9. n64_scene.glr file should have been created in your designated texture_dump folder
10. Use the included Blender python script to import into Blender.
