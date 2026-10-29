# Third-party notices

Project code and original demo applications are distributed under the MIT
license in [LICENSE](LICENSE), credited to RavensClawX and contributors.

## DejaVu font atlas

`assets/font_atlas.r8`, `assets/font_atlas.png`, and the generated glyph
metrics in `launcher/src/font_metrics.c` derive from DejaVu Sans and DejaVu
Sans Bold. Bitstream's copyright and permission notice is reproduced in
[assets/FONT_LICENSE.txt](assets/FONT_LICENSE.txt). DejaVu changes are public
domain. Font source files are not bundled. Regenerate with
`python3 tools/make_font_atlas.py` using system-installed DejaVu fonts.

## Project lineage and external dependencies

The launcher design and earlier host tooling are by RavensClawX. The host
includes adaptations of the project's earlier `sm64-console` tooling.
Legacy compatibility adapters remain available for external checkouts; those
projects, their games, assets, and licenses are not bundled here.

The external `ns2jb-kurumuz` session runner, compiler toolchains, Vulkan tools,
Python dependencies, and system libraries are installed separately and retain
their own licenses. No console firmware, keys, ROMs, game data, third-party
application packages, or game artwork is included in this source release.
