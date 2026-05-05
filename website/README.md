# VoiceStick Website

Vue + Vite source for the VoiceStick homepage and Sparkle appcast. The site uses
`vue-i18n` for Simplified Chinese and English, and picks Chinese automatically
when the browser language starts with `zh`.

Suggested GitHub Pages URL for this repository:

```text
https://78.github.io/voicestick/
```

The macOS app checks the generated root-level appcast:

```text
https://78.github.io/voicestick/appcast.xml
```

## Release Flow

1. Generate Sparkle keys once and keep the private key out of git:

   ```bash
   cd desktop
   swift package resolve
   find .build -name generate_keys -type f -perm -111 -print -quit
   .build/artifacts/sparkle/Sparkle/bin/generate_keys --account voicestick
   ```

2. Put the public key into the app before a release build:

   ```bash
   SPARKLE_PUBLIC_ED_KEY="..." scripts/build-macos.sh --release
   ```

3. Create the DMG:

   ```bash
   scripts/make-dmg.sh
   ```

4. Upload these files to GitHub Release `v0.1.0`:

   ```text
   build/VoiceStick-0.1.0.dmg
   build/VoiceStick-0.1.0.zip
   ```

5. Update `website/public/appcast.xml`:

   - `url`: GitHub Release URL for the ZIP, not the DMG
   - `sparkle:edSignature`: content from `build/VoiceStick-0.1.0.signature`
   - `length`: byte size from `wc -c build/VoiceStick-0.1.0.zip`

The homepage download button points at the latest GitHub Release so users can install the notarized DMG.

## Develop

```bash
cd website
npm install
npm run dev
```

## Build

```bash
cd website
npm run build
```

The generated `dist/` directory is deployed to GitHub Pages. `public/appcast.xml` is copied to `dist/appcast.xml`.

## GitHub Actions

- `.github/workflows/release.yml` runs on `v*` tags or manual dispatch. It builds the macOS app, signs and notarizes the DMG, uploads DMG/ZIP/signature assets to the matching GitHub Release, rewrites `public/appcast.xml`, builds the Vue site, and deploys `website/dist` to GitHub Pages.
- `.github/workflows/deploy-website.yml` runs when `website/**` changes on `main`. Before deploying, it reads the latest GitHub Release and regenerates `public/appcast.xml` from the latest ZIP asset and signature, so website-only deploys do not overwrite the update feed with a stale placeholder.

Required repository secrets for release builds:

```text
SPARKLE_PUBLIC_ED_KEY
SPARKLE_PRIVATE_ED_KEY
MACOS_CERTIFICATE_P12
MACOS_CERTIFICATE_PASSWORD
APPLE_ID
APPLE_TEAM_ID
APPLE_APP_SPECIFIC_PASSWORD
```
