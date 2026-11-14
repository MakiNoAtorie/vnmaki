# Localization

A game can have more than one language. The scripts are written in one language. The other languages are `.po` files. This guide is for game authors. The rules for contributors are in `docs/CONVENTIONS.md` (Localization). The template of `vnmaki new` has English only. The Japanese add-on (`vnmaki new <name> --lang ja`) makes it a working example of two languages, and the label `content` shows the switch.

## Languages in project.toml

```toml
languages = ["en", "ja"]
```

- The first language is the one that the scripts are written in (the source language). It has no `.po` file. The default is `["en"]`.
- A language code has only letters, digits, `-` and `_`: it is a file name. `vnmaki extract-strings` (for the languages after the first) and the font import refuse any other code.
- These tables of `project.toml` have one entry for each language. Every key must be in `languages`:

| Table | Value | Meaning |
|-------|-------|---------|
| `[text_direction]` | `ar = "rtl"` | `"ltr"` (default) or `"rtl"`. Right to left reverses the order of the children of an `hbox`. |
| `[vertical]` | `ja = true` | The default `say` and `nvl` screens set the text in columns that run down, the first column at the right. |
| `[styles]` | `ja = "styles/ja_look.lua"` | A file of `style(...)` calls that runs over the game's styles. Without an entry, `styles/<code>.lua` is used if it exists. |
| `[fonts.fallback]` | `ja = ["fonts/jp.otf"]` | Fonts that supply the characters that the text font does not have, in the order to try. |

## Make the translation files

```
vnmaki extract-strings my_game
```

The command reads the scripts and writes:

- `translations/template.pot`: every string of the scripts, with no translations.
- `translations/<code>.po` for each language after the first. A file that exists keeps its translations.

It prints one line for each file (`translations/ja.po: 36 strings, 0 fuzzy, 0 obsolete`). It works out all files before it writes one, so a `.po` file that cannot be read (exit code 1, `vnmaki: translations/ja.po:5: ...`) changes none. A second run on the same scripts changes nothing. The exit codes are 0, 1 (an error) and 2 (usage).

### What is extracted

Only string literals that are not empty:

- the text of `say(who, text)` and `narrate(text)`,
- the options of a `menu { ... }` table written in the call,
- the first string of a `text { ... }` or `button { ... }` node in a `screen` of `scripts/`.

A text that the script builds (`say(who, "Hello " .. name)`) or takes from a variable is not extracted. `vnmaki check` gives the warning `non-literal-string` for it. The default screens of the engine are written inside the engine, so their text (Save, Load, Preferences, ...) is not in the files. To translate one, write the screen again in `scripts/`.

### The files

Each entry of a `.po` file has a key, the source text and the translation:

```po
#. speaker: eileen
#: scripts/content.lua:11
msgctxt "content/52af9330"
msgid "This is the content demo: a language, captions, a 3D model and a plugin."
msgstr "これはコンテンツのデモです。…"
```

- The key (`msgctxt`) is `<scope>/<hash>`. The scope is the label name, or `screen/<name>` for a screen. The hash has 8 hex digits of FNV-1a (32 bit) over the speaker, a NUL byte and the text.
- The speaker is the first argument of `say` as the script writes it (`eileen` in `say(eileen, "...")`, a literal for `say("Alice", "...")`). `#. speaker:` and `#:` (file and line) are notes of the tool.
- The same text in one label with the same speaker is one entry that has a reference for each line.
- Write the translation in `msgstr`. Text tags (`{color=#f00}...{/color}`) go into the translation as they are, and they must parse. `vnmaki check` gives `bad-translation-tag` if they do not.
- A string is written with the C escapes (`\n`, `\t`, `\"`, `\\`, `\xHH`, ...). There are no plural forms.

### When the scripts change

A new run updates the files:

- An entry with the same key and text keeps its translation, its translator comments and its flags.
- If a line changed, its key changes (the hash). The old translation moves to the new key at the same position in the label, flagged `fuzzy`. A fuzzy entry is not used: the source text shows until you check the translation and take the flag off.
- An entry that has a translation and no new place becomes obsolete (`#~`) and stays at the end of the file. It comes back if its text returns. An entry with no translation that has gone is dropped.
- The header of the old file stays.

## Choose the language

- `vnmaki run <folder> --lang ja` plays in that language. The code must be in `languages`, or the command stops with exit code 1. The choice is also saved in the preferences.
- `prefs.language = "ja"` in a script, or the preference that the player sets. The empty string is the source language. In a window, the language changes at once: the lines, the options of a menu, the backlog and the screens show the new text. A script that sets `prefs.language` at a line shows and speaks that line in the new language.
- The default `preferences` screen has a button for each language when the game has more than one. A button of your own returns `"@language:ja"`.
- `languages()` in a script lists the codes, the source language first.
- A language change is not a step of the story. It does not change the saves: a save made in one language loads in another.

A translation is found by the label that runs (the scope), the speaker and the source text. For a `say`, the name of the character can differ from the token in the script (`eileen` and `Eileen`): the lookup compares them without case and ignores everything but letters and digits. A line with no entry, a fuzzy or empty entry, and an obsolete entry show the source text. A `vnmaki run --headless` transcript shows the translated `say`, `narrate` and menu lines; the story is the same in each language.

## Fonts, pictures and voices

- **Fonts.** `vnmaki import` and `vnmaki build windows` make a subset of each `.ttf` and `.otf` file for each language (`fonts/default.ttf` becomes `fonts/default.ja.ttf`). The subset has the ASCII characters, the characters of the title, of every string literal of the scripts and, for a language after the first, of its `msgstr` lines. In a game that runs from loose files (`vnmaki run <folder>`) the whole font is used. The fonts of `[fonts.fallback]` and the dyslexia-friendly font (`fonts/dyslexia.otf` or `.ttf`) use the same rule. A font that you list under `[build.fonts] keep` in `project.toml` gets no subset and goes in the pack as it is (see `docs/build.md`).
- **Pictures.** `images/<code>/<name>` is used before `images/<name>` in that language. Use it for a picture that has text.
- **Voices.** `voice("eileen/hello")` plays `audio/voice/<code>/eileen/hello` before `audio/voice/eileen/hello` in that language.

A picture or voice that is already shown or playing does not change when the language changes.

## The template

`vnmaki new <name>` makes a game with `languages = ["en"]`. It has no Japanese file, and the label `content` skips its language menu (`if #languages() > 1`), so the sample has no option that does nothing. The default `preferences` screen shows no language buttons for one language either.

### Add Japanese

Japanese is an add-on, the folder `templates/addons/ja/`. It adds:

- `translations/ja.po`: the output of `vnmaki extract-strings` with the narration of `warm`, the menu option of the content demo and every line of the label `content` translated.
- `fonts/NotoSansJP.ttf` and `licenses/NotoSansJP-OFL.txt`: Noto Sans JP (SIL OFL). `fonts/default.ttf` (Noto Sans) has no Japanese characters, so this font supplies them, with the vertical forms.
- `ASSETS.ja.txt`: the source and license of these files.
- In `project.toml`: `"ja"` in `languages`, `[vertical] ja = true` and `[fonts.fallback] ja = ["fonts/NotoSansJP.ttf"]`.

Use it when you make the game: `vnmaki new my_game --lang ja` (the launcher has a check box for each add-on next to the New button). To add it to a game that exists, copy `fonts/`, `licenses/`, `translations/` and `ASSETS.ja.txt` of the add-on into the game and add the three entries above to `project.toml` by hand. Then run `vnmaki extract-strings` to make `ja.po` fit your own story: the sample translation fits the sample story only.

### Add a language add-on

An add-on is a folder `templates/addons/<code>/` with an `addon.toml` and the files to copy. `<code>` is the language code (letters, digits, `-` and `_`) and the name that `--lang` takes. To add French:

1. Make `templates/addons/fr/` with the files, with the paths that they have in a game: `translations/fr.po`, `fonts/...` and `licenses/...` if a font is needed, and an assets file `ASSETS.fr.txt` that says where each file comes from and its license.
2. Write `templates/addons/fr/addon.toml`:

```toml
language = "fr"

[fonts.fallback]
fr = ["fonts/MyFrench.ttf"]
```

   `language` goes to the end of `languages` in `project.toml`. The keys under each table go into the same table of `project.toml`, and the table is added at the end if the game has none. Comments are not copied. The file `addon.toml` itself is not copied.
3. `vnmaki new my_game --lang fr` and the launcher find the add-on by its folder name. They need no other change. A game that has a file of the add-on already is an error and nothing is written.

## Tools and checks

- `vnmaki check` reads `translations/*.po`: a file that does not parse is the error `po-syntax`, and a translation with bad text tags is `bad-translation-tag` (only translations that are in use: not fuzzy, not obsolete, not empty).
- A dev build has a "Translations" window in the overlay (F12). It lists the entries of the label that the story is in, in the language that is shown. An edit is written to the `.po` file and shows at once.

## Not in the engine

Plural forms.
