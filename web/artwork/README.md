# ESP Serial2WiFi icon

The favicon combines a terminal prompt and radio waves in the application's teal
and white. `favicon.svg` is the editable source; the committed ICO contains 32 px
and 16 px images. It replaces the upstream JeeLabs logo without changing its route.

Rebuild after editing with ImageMagick's SVG support:

```sh
magick -background none web/artwork/favicon.svg -define icon:auto-resize=32,16 web/custom-pages/favicon.ico
```

Normal application builds use the committed ICO and do not require ImageMagick.
The icon uses no external image, font or runtime dependency.
