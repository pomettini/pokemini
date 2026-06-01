# PokeMini for Playdate

A port of JustBurn's [PokéMini emulator](https://sourceforge.net/projects/pokemini/)
to the Panic® [Playdate®](https://play.date/). Plays
[Pokémon® Mini](https://en.wikipedia.org/wiki/Pok%C3%A9mon_Mini)
cartridges (and homebrew). Most titles are very playable, fast-paced
action games can run a few fps below native speed.

**ROMs are not included.** The bundle ships with FreeBIOS only.

## Install

1. Download `PokeMini.pdx` from this page.
2. Sideload via [play.date/account](https://play.date/account/sideload/).

## Adding ROMs

Drop `.min` files into `/Shared/Emulation/pm/games/` on your Playdate. The folder is created on first launch.

Return to the picker mid-game from Playdate's system menu.

## Saves

In-game saves (the cartridge's EEPROM) are written to `/Shared/Emulation/pm/saves/<rom_name>.eep` when you switch ROMs or quit the app. Saves persist across restarts and survive sideloading a new build.

## Controls

| Playdate              | Pokémon Mini |
| ---                   | ---          |
| D-pad                 | D-pad        |
| A                     | A            |
| B                     | B            |
| Crank, in 60°-180°    | C            |
| Shake the device      | Shake        |

## Options (found in Playdate system menu)

- **LCD Mode**: `Fast` (default) is the best-performing mode but greys
  will flicker. `Soft` smooths the greys to mimic the original screen at a
  small fps cost.
- **Scale**: `3.75x` (default) fills the screen vertically but stretches.
  `3x` is pixel-perfect with a black border.

## Credits

- **PokéMini core**: [JustBurn](https://sourceforge.net/projects/pokemini/) (2009–2015).
- **Playdate port**: [Giorgio Pomettini](https://www.giorgiopomettini.eu/).
- **Graphics assets**: [Noemi Frulio](https://noemifrulio.itch.io/).
- **FreeBIOS**: Team Pokeme (2009), freeware.

Source code, roadmap and dev notes:
[github.com/pomettini/pokemini](https://github.com/pomettini/pokemini).

## License

[GPLv3](https://www.gnu.org/licenses/gpl-3.0.html). See `LICENSE` inside the `.pdx` bundle.

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
