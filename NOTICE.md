# Origin of the code and licenses

The source files are distributed under the GNU General Public License,
version 2 or (at your option) any later version; see `LICENSE`. The
`sp211-printer-app` binary is distributed under version 3 or later (see
below).

| Files | Origin | License |
|---|---|---|
| `src/lilo2.c`, `src/lilo2.h`, `src/dither_matrix.h` | copied unchanged from [Creative8428/ricoh-sp211-driver](https://github.com/Creative8428/ricoh-sp211-driver), © 2026 Alexandru Hrisca | GPL-2.0-or-later |
| `third_party/jbigkit/` | JBIG-KIT 2.2 by Markus Kuhn, as found in the same repository. `jbig.c` carries one change made there (marked `RICOH_FLUSH`, dated and described in its header); the other files are unmodified. | GPL-2.0-or-later (`third_party/jbigkit/COPYING`) |
| `src/sp211-printer-app.c`, `src/sp211_status.c`, `src/sp211_status.h`, `src/sp211_usb.c`, `src/sp211_usb.h`, `tools/`, `tests/`, `docs/`, `packaging/`, `Makefile` | written for this project | GPL-2.0-or-later |
| `src/sp211-printer-app.c`: paper sizes, media types, margins | taken from the PPD of the same repository, © 2026 Alexandru Hrisca | GPL-2.0-or-later |

Libraries linked at build time and not included here: PAPPL (Apache-2.0),
libcups (Apache-2.0 with an exception that permits linking with GPL-2.0
software) and libusb (LGPL-2.1-or-later).

`src/sp211-printer-app.c` is linked with PAPPL, whose Apache License 2.0 has
no such exception. The Apache License 2.0 is compatible with version 3 of the GNU GPL
only, so the `sp211-printer-app` binary is distributed under
GPL-3.0-or-later; see `LICENSE.GPL-3`. Every source file keeps its own
license, which allows version 3.

No code or file from Ricoh is included. The status protocol described in
`docs/STATUS_PROTOCOL.md` was worked out for interoperability, from USB
traffic observed on a printer and from the behaviour of Ricoh's software.

RICOH is a trademark of Ricoh Company, Ltd. This project is not affiliated
with or endorsed by Ricoh.
