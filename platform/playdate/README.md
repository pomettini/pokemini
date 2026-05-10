# PokeMini for Playdate

A port of JustBurn's [PokéMini emulator](https://sourceforge.net/projects/pokemini/)
to the Panic® [Playdate®](https://play.date/). Plays
[Pokémon® Mini](https://en.wikipedia.org/wiki/Pok%C3%A9mon_Mini)
cartridges (and homebrew) on Playdate at full speed in regular play.

**ROMs are not included.** The bundle ships with FreeBIOS only.

## Install

1. Download `PokeMini.pdx` from this page.
2. Sideload via [play.date/account](https://play.date/account/sideload/).

## Adding ROMs

Drop `.min` files into `/Shared/Emulation/pm/games/` on your Playdate. The folder is created on first launch.

Return to the picker mid-game from Playdate's system menu.

## Controls

| Playdate           | Pokémon Mini |
| ---                | ---          |
| D-pad              | D-pad        |
| A                  | A            |
| B                  | B            |
| Crank angle zone   | C            |
| Shake the device   | Shake        |

Undock the crank and rotate it into the 60°-180° zone to hold Pokémon Mini's
**C** button. Move it outside that zone, or dock it, to release C.

## System Menu

- **ROM Picker** returns to the ROM list.
- **LCD Mode**: `Soft` is the default smoothed display mode; `Fast` uses
  raw 2-shade LCD output for performance testing.
- **Scale**: `3x` is the default stable integer-scale view; `3.5x` is
  available as a larger experimental view.

## Known limitations

- No EEPROM save on device.
- No save-state UI yet.
- Heavy sprite scenes run ~7% slow.

## Credits

- **PokéMini core** — [JustBurn](https://sourceforge.net/projects/pokemini/) (2009–2015).
- **Playdate port** — [Giorgio Pomettini](https://www.giorgiopomettini.eu/).
- **Graphics assets** — [Noemi Frulio](https://noemifrulio.itch.io/).
- **FreeBIOS** — Team Pokeme (2009), freeware.

Source code, roadmap and dev notes:
[github.com/pomettini/pokemini](https://github.com/pomettini/pokemini).

## License

[GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) — see `LICENSE` inside the `.pdx` bundle.

## Legal

PokeMini for Playdate is a fan project. It is **not affiliated with,
endorsed by, or sponsored by Nintendo® Co., Ltd., Game Freak® Inc.,
or The Pokémon Company®.** Pokémon® and Pokémon® Mini are trademarks
of Nintendo, Game Freak, and The Pokémon Company; all rights belong
to their respective owners. Playdate® is a registered trademark of
Panic® Inc., with whom this project is also unaffiliated.

---

This Playdate port was developed with assistance from generative AI,
specifically [Claude Code](https://claude.com/claude-code). Use of
generative AI in the upstream PokeMini emulator has not been
disclosed.
