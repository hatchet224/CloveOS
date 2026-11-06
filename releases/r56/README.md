# R56 normal TGB Dual and Ports Collection candidate

TGB Dual can now be selected for ordinary single-player GB/GBC games. It uses
the existing pinned core with dedicated configuration and link emulation disabled.
Normal battery saves, checkpoints, fresh-process resume and Netplay save seeding
are tested with the actual ARM64 core and production RetroArch using generated
cartridges. Existing Gambatte configuration and checkpoints retain their identity.

Highlight a game, open its list options, choose Game core, select TGB Dual, then
use Reset game once after changing cores. Save inside the game before starting
Pokemon Trade/Battle. Older core-specific checkpoints are retained.

Ports Collection is controlled by Apps -> Package Manager -> Verified. Its games
appear under Games -> Ports, with Download games (PortMaster) at the top. Legacy
visibility preferences migrate once; Package Manager then controls availability.
Installed ports and their data remain in place. English and Spanish are supported.
The temporary controller mapping lives in RAM; the existing platform hook is
backed up and refreshed without replacing the installed game catalog or state.

R54's accepted brightness correction and R55's pairing, cancellation and retained
diagnostics are preserved. Software and emulated-device tests do not establish a
successful physical Z28/Miyoo trade. Physical normal-core and trade tests remain
pending. This source supplement does not claim a public binary release.

No ROMs, BIOS, personal saves, artwork, card captures or credentials are included.
Existing dependency licenses and third-party notices remain in force.
