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

### Your own costumes (slots 4 to 6)

The Fortress of Solitude's Costumes screen has three more slots after the game's three: 4, 5 and 6. They are never locked. Each one is a costume you paint yourself, with no texture names to look up.

#### Where the files are

Next to `superman_returns.exe`, in `textures\Suits`, there is one folder per slot: `Slot4`, `Slot5` and `Slot6`. The builder fills each one from your own disc with Superman's movie costume, ready to edit:

| File | What it is |
|---|---|
| `body.png` | the suit: chest, arms, legs and the S shield |
| `briefs_boots.png` | the briefs, the belt and the boots |
| `cape.png` | the outside of the cape |
| `thumb.png` | the small picture of the slot on the Costumes screen |
| `fullsize.png` | the large picture shown with View Costume |
| `costume.toml` | the slot's name, whether it has the cape and the colour of the cape's inside |

#### Make a costume

1. Open the slot's PNGs in an image editor, paint them and save them as PNG under the same names.
2. To name the costume, open `costume.toml` with Notepad and change the text between the quotes, for example `name = "Black Suit"`. The name shows on the Costumes screen.
3. To show your costume on the Costumes screen, replace `thumb.png` (and `fullsize.png`) with your own picture, for example a screenshot of Superman in it. The game cannot draw these from the textures: they are pictures.
4. In the game, open the Fortress of Solitude, then Costumes, select the slot and press Wear Costume.

#### With or without the cape

On slots 4 to 6 the Costumes screen shows **CAPE: SHOWN**. Press Y (triangle on a PlayStation pad, X on the keyboard) to switch it to **CAPE: HIDDEN**: that costume is worn without the cape. The choice is saved in the slot's `costume.toml` as `cape = true` or `cape = false`, which you can also edit by hand.

A file you delete is taken from the game, so a slot with only `cape.png` changes only the cape. Running a new builder keeps your files and names.

#### The colour of the cape's inside

The inside of the cape does not come from a texture: the game paints it with one colour. Set it in the slot's `costume.toml`, as a hex colour in quotes:

```toml
cape_inside = "#BF0000"
```

`"#BF0000"` is the game's own red; `"#101010"`, for example, gives a black inside for a black suit. Without this line the inside stays the game's red. Slots made by a builder older than 1.0.12 do not have the line: add it by hand.

#### Good to know

- Keep the transparency of `body.png` and `briefs_boots.png` where it is; the game uses it.
- The files in `textures\Suits` are used only while their slot is worn.
