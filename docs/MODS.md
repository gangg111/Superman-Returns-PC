### Texture mods

You can replace any texture of the game, for example Superman's suit, with your own image. No programming is needed, only an image editor.

#### How it works

The game does not know textures by a readable name. Every texture has its own code, made from its content and size, for example:

`EA4A8BC906AA9D06_1024x512_f20`

The game loads your image only when the file has **exactly that name**. This is why mods made for other emulators or packs with other names do nothing: the game does not know which texture they belong to. So first you let the game tell you the names (step 1), then you put your image under that name (step 2).

#### Step 1: get the names of the textures

1. Open the folder with `superman_returns.exe`.
2. Open `superman_returns.toml` in that folder with Notepad.
3. Add this line at the end and save the file:

   ```
   sr_texture_dump_dir = "C:/sr_dump"
   ```

4. Start the game and play until you see what you want to change (for Superman's suit: until Superman is on screen). While you play, the game saves every texture it shows into `C:\sr_dump`, once each. The game runs slower while it does this; that is normal.
5. Close the game. `C:\sr_dump` now holds many `.dds` files. Open them in a program that reads DDS, for example [Paint.NET](https://www.getpaint.net/) or [GIMP](https://www.gimp.org/), and find the one you want to change. Write down its name.
6. Open `superman_returns.toml` again and delete the line from step 3, so the game runs at full speed again.

#### Step 2: put your mod in

1. Open the texture you found, change it as you like and save it as **PNG with the same name**: only the ending changes from `.dds` to `.png`.

   `EA4A8BC906AA9D06_1024x512_f20.dds` becomes `EA4A8BC906AA9D06_1024x512_f20.png`

2. Put the PNG into the `textures` folder next to `superman_returns.exe`. If the folder is not there, create it. You may sort your mods into subfolders inside `textures`.
3. Start the game. The new texture is used right away.

A PNG from another mod works too: rename it to the name you found in step 1.

#### Good to know

- **Size:** the image may be larger than the original (up to 16384 x 16384), for example twice as large for a sharper suit. Keep the same proportions, or the texture will look stretched.
- **Transparency** in the PNG is kept.
- **DDS also works.** If the same name exists as `.dds` and as `.png`, the game uses the `.dds`.
- **Updates keep your mods.** When you run a new builder over your game folder, the files you added to `textures` stay.
- **Turning mods off:** move your files out of `textures` and start the game again. Leave the other files in `textures` where they are: they are the remastered textures from the builder.

#### My mod does not show up

Open the newest file in the `logs` folder next to `superman_returns.exe` and search for `textures:`.

- `textures: 12 replacement(s) in ...` tells how many files the game found in `textures`. If your file is not counted, it is in the wrong folder or does not end in `.png` or `.dds`.
- `textures: EA4A8BC906AA9D06_1024x512_f20 replaced by ...` means your file is in use.
- `textures: ... not used: ...` means the game found your file but could not read it; the end of the line says why (for example `not a readable PNG`). Save it again as a normal PNG.
- If your name appears in none of these lines, it does not match the texture: compare it with the name in `C:\sr_dump` letter by letter.

If it still does not work, open an [issue](https://github.com/gangg111/Superman-Returns-PC/issues) and attach that log file.
