# Third-party notices

The Apache License 2.0 in `LICENSE` covers the source in this repository that
carries an `SPDX-License-Identifier: Apache-2.0` header. The components below
keep their own licenses and are **not** relicensed by this repository.

## SIL Open Font License 1.1 — the note font

`components/app_hello/fonts/MosaicoNotesCN-GB.ttf` is a Modified Version of
Adobe Source Han Sans and is licensed under the SIL Open Font License 1.1.
The complete license text is in `components/app_hello/fonts/OFL.txt`, and the
copyright notice and the list of modifications are in
`components/app_hello/fonts/NOTICE`.

Because OFL 1.1 section 5 requires the font to be distributed entirely under
that license, this font is **not** covered by the Apache License 2.0 in
`LICENSE`.

The font is compiled into the firmware image, so the obligation travels with
every image this repository builds.

## Bitstream Vera and Arev fonts — the DejaVu family

`components/app_hello/ui/fonts/DejaVuSans.ttf` and
`components/app_hello/ui/fonts/DejaVuSans-Bold.ttf` are licensed under the
Bitstream Vera Fonts Copyright and the Arev Fonts Copyright. The full text is
in `components/app_hello/ui/fonts/LICENSE`.

The family names carry neither "Bitstream" nor "Vera", as the license requires
of a modified font.

## GlobalSign Root CA

`certs/globalsign-root-ca.pem` is a public root certificate used as a TLS
trust anchor. It is a public document and carries no license obligation.

## Components fetched at build time

Components resolved by ESP-IDF from the Component Registry, and Python
packages installed by the build, keep their own licenses. `dependencies.lock`
records the resolved versions; each resolved component carries its own license
and notice in `managed_components/`.
