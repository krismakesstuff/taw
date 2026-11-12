# Third-party software

taw is licensed under the GNU Affero General Public License v3 (see [LICENSE](LICENSE)). It is
built with the following, each under its own licence.

| Component | Used for | Licence | Shipped in |
| --- | --- | --- | --- |
| [JUCE](https://juce.com) 9 | audio, plugins, windows, file formats | AGPLv3 (JUCE's open-source option) | every build |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) (through JUCE) | hosting VST3 plugins | MIT | every build |
| [Steinberg ASIO SDK](https://www.steinberg.net/developers/) 2.3.4 | ASIO audio devices | GPLv3 (the SDK's open-source option) | Windows |
| [FFmpeg](https://ffmpeg.org) 9.0.2, [Gyan Doshi's shared "full" build](https://github.com/GyanD/codexffmpeg) | video (decoding, `bv` encoding) | GPLv3 (this build enables GPL components) | Windows, as DLLs next to taw.exe |
| [QR Code generator](https://github.com/nayuki/QR-Code-generator) 1.8.0, by Project Nayuki | the phone link's address as a QR code | MIT | every build |

JUCE itself contains third-party code (FLAC, Ogg Vorbis, zlib, libpng, libjpeg and others),
listed with their licences in JUCE's `LICENSE.md` and in the source files under
`modules/` of the JUCE release taw builds against.

## FFmpeg

The Windows download includes FFmpeg's DLLs (`avformat`, `avcodec`, `avutil`, `swscale`,
`swresample`), unmodified from Gyan Doshi's build, with FFmpeg's licence (`FFMPEG-LICENSE.txt`)
and the build's README (`FFMPEG-README.txt`, which lists the external libraries it was built
with and their versions). Each GitHub release attaches FFmpeg's source for that version
(`ffmpeg-<version>-source.tar.*`). The sources of the external libraries are linked from the
build's README and from <https://www.gyan.dev/ffmpeg/builds/>.

On macOS taw doesn't ship FFmpeg: it uses the one you install (`brew install ffmpeg`), if any.

## Trademarks

VST and ASIO are trademarks of Steinberg Media Technologies GmbH. taw is not affiliated with or
endorsed by Steinberg.
