# Trademarks

Inseglet is an independent, community-developed open-source extension. It is **not affiliated with,
authorized, sponsored, certified, or endorsed by** any of the trademark holders named below. Third-party
names are used only to describe interoperability and the standards Inseglet targets — nominative use, not a
claim of association.

## Names used in this project

- **REAPER** and **Cockos** are trademarks of **Cockos Incorporated**. Inseglet is a third-party extension
  for REAPER; it is not a Cockos product.
- **Dolby** and **Dolby Atmos** are trademarks of **Dolby Laboratories, Inc.**
- **VST** is a trademark of **Steinberg Media Technologies GmbH**. **Nuendo** and **Cubase** are Steinberg
  products.
- **Pro Tools** is a trademark of **Avid Technology, Inc.**
- **IEM Plug-in Suite** (Institute of Electronic Music and Acoustics) and **SPARTA / COMPASS** are the
  property of their respective authors. Inseglet interoperates with these tools when installed; it does not
  bundle, modify, or redistribute them.

All other product and company names mentioned may be trademarks of their respective owners.

## Scope of ADM / Dolby Atmos support — please read

Inseglet can author an **ITU-R BS.2076 Audio Definition Model (ADM)** Broadcast-Wave file, and offers a
conformance mode that shapes and validates that file against the **publicly documented Dolby Atmos Master
ADM Profile** constraints (channel/bed limits, coordinate mode, metadata restrictions, sample rate, and so
on).

This is an **independent implementation of a published profile**. Specifically:

- Inseglet is **not a certified Dolby product**, and produces **no certification**.
- Conformance is **self-validated against the published rules**. Inseglet does not guarantee that any
  specific certified renderer or Dolby tool will ingest a given file; verifying ingest with your own
  certified toolchain remains your responsibility.
- "Dolby Atmos Master ADM Profile" is used **descriptively**, to name the target the conformance mode aims
  at — not to imply endorsement, partnership, or certification by Dolby Laboratories.

If you represent a trademark holder and have a concern about how a name is used here, please open a
confidential contact via `SECURITY.md` or the maintainer address in `MAINTAINERS.md`, and we will address it
promptly.
