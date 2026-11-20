# Foxie Litch branding assets

The app header uses [the fox source image](source_fox.png). The launcher icons are generated from [the app icon master](../../app/assets/branding/foxie-app-icon-master.png). Brand usage is covered by the [license information](../../README.md#licenses).

Run the generator from the repository root with Pillow installed. Header lettering uses Gurmukhi MN Bold on macOS.

```sh
python3 tools/branding/render_brand_assets.py --repo . --source tools/branding/source_fox.png
```

To regenerate only the iOS and Android launcher sizes from the checked-in master, run `python3 tools/branding/render_brand_assets.py --repo . --icon-only`.
