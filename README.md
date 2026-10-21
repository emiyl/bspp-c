# BSPP-C

This project is a C port of the [Butterscotch Preprocessor](https://github.com/ButterscotchRunner/ButterscotchPreprocessor).

## Usage

The project exposes two main ways to use it:

1. As a reusable C library via the static library target (`bspplib`)
2. As a command-line tool built from the CLI target (`bspp`)

### CLI usage

Build the project:

```bash
cmake -S . -B build
cmake --build build
```

Then run the preprocessor:

```bash
./build/cli/bspp --help
./build/cli/bspp --target=ps2 --output ./output/ /path/to/data.win
```

Common flags:
- `-o, --output=<path>`: output directory
- `--target=(ps2|ps3)`: target platform
- `--atlas-size=<pixels>`: atlas size for PS2 output
- `--force-4bpp=<regex>`: force 4bpp for matching images
- `--debug-output=<path>`: extract debug PNGs
- `-v, --verbose`: show processing progress and file output paths, and runtime stats
- `-h, --help`: show usage

### Library usage

The library exposes the public API from `src/processor_api.h` (or the equivalent public header used by the project). Typical usage is to construct an `Options` struct and call `process(&options)`.

Example:

```c
#include "processor_api.h"

int main(void) {
    Options options = {
        .input = "./data.win",
        .output = "./output",
        .atlas_size = 512,
        .ps3 = false,
        .verbose = false,
        .patterns = NULL,
        .pattern_count = 0
    };

    process(&options);
    return 0;
}
```

## Building

This project builds with CMake and requires a C11-capable compiler.

Required system libraries:
- libpng
- libbz2 / BZip2 development files
- Vorbisfile development package (`vorbisfile` via pkg-config)
- PCRE2 development package (`libpcre2-8` via pkg-config)
- zlib (typically provided by the operating system or build environment)
- pkg-config

Common install commands:
- macOS (Homebrew): `brew install cmake pkg-config libpng bzip2 libvorbis pcre2`
- Ubuntu/Debian: `sudo apt install build-essential cmake pkg-config libpng-dev libbz2-dev libvorbis-dev libpcre2-dev`

The project uses the following CMake packages:
- `PNG`
- `BZip2`
- `PkgConfig`
- `vorbisfile`
- `libpcre2-8`

After installing the dependencies, configure and build with:

```bash
cmake -S . -B build
cmake --build build
```