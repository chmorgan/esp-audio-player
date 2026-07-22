# WAV regression fixtures

These files are vendored unchanged from upstream test suites. Tests must read
them from disk; they must not rewrite or normalize them. The immutable source
URL and SHA-256 digest below identify the exact bytes being tested.

## `bug1301226.wav`

- Source repository: [mozilla/gecko-dev](https://github.com/mozilla/gecko-dev)
- Commit: [`0b3dd924370fbf1dfc8314849d6323ce5e934af9`](https://github.com/mozilla/gecko-dev/commit/0b3dd924370fbf1dfc8314849d6323ce5e934af9)
- Immutable source: [raw WAV](https://raw.githubusercontent.com/mozilla/gecko-dev/0b3dd924370fbf1dfc8314849d6323ce5e934af9/dom/media/test/bug1301226.wav)
- SHA-256: `2148387876d61f47be3a298f719e7e66f8ba04da13828110f50e0d35e8cc4722`
- License: Mozilla Public License 2.0; see
  [`MOZILLA-ROOT-LICENSE.txt`](MOZILLA-ROOT-LICENSE.txt) and
  [`MOZILLA-PUBLIC-LICENSE-2.0.txt`](MOZILLA-PUBLIC-LICENSE-2.0.txt).

This is the regression input for [Mozilla bug
1301226](https://bugzilla.mozilla.org/show_bug.cgi?id=1301226). The 240-byte
file declares both the RIFF size and the `data` chunk size as `0xffffffff`.
Mozilla used it to ensure that progressive playback did not overflow the
declared duration. For this library's bounded host parser, the expected result
is a clean invalid/truncated-input error: it must not wrap an integer, seek or
read out of bounds, or loop indefinitely.

The Mozilla fixtures have no file-local license notices. The pinned gecko-dev
root `LICENSE` points to the codebase licensing document, which identifies the
MPL 2.0 as the primary project license. That root notice and the complete MPL
2.0 text are retained here.

## `1khz_sine_48k_mono_region_marker.wav`

- Source repository: [briandorsey/wavrw](https://github.com/briandorsey/wavrw)
- Commit: [`14f957fa41357763e5c0eaecf52b80c4c840603b`](https://github.com/briandorsey/wavrw/commit/14f957fa41357763e5c0eaecf52b80c4c840603b)
- Immutable source: [raw WAV](https://raw.githubusercontent.com/briandorsey/wavrw/14f957fa41357763e5c0eaecf52b80c4c840603b/test_wavs/1khz_sine_48k_mono_region_marker.wav)
- SHA-256: `98b5ff729f2d255f21aac7e95474e339111cbf57e7bb732d64643be9ff468894`
- License: Apache License 2.0; see
  [`WAVRW-APACHE-2.0.txt`](WAVRW-APACHE-2.0.txt).

This is a valid 24-bit, 48 kHz mono PCM file. It contains `bext` and `junk`
chunks before `data`, then `cue `, `smpl`, and `LIST` chunks after `data`. The
`data` chunk contains 1,440 bytes, or 480 mono frames. The expected result is
successful parsing and decoding of exactly those 480 frames without treating
the trailing metadata as audio.

## `r11025_u8_c1_trunc.wav`

- Source repository: [mozilla/gecko-dev](https://github.com/mozilla/gecko-dev)
- Commit: [`d65563489706c1133751d96d4a44f577e158a279`](https://github.com/mozilla/gecko-dev/commit/d65563489706c1133751d96d4a44f577e158a279)
- Immutable source: [raw WAV](https://raw.githubusercontent.com/mozilla/gecko-dev/d65563489706c1133751d96d4a44f577e158a279/dom/media/test/r11025_u8_c1_trunc.wav)
- SHA-256: `05f4657a6b3212f6a1364012cde67063c660011d514863aa7faf795333766780`
- License: Mozilla Public License 2.0; see
  [`MOZILLA-ROOT-LICENSE.txt`](MOZILLA-ROOT-LICENSE.txt) and
  [`MOZILLA-PUBLIC-LICENSE-2.0.txt`](MOZILLA-PUBLIC-LICENSE-2.0.txt).

This is format-1 PCM: unsigned 8-bit mono at 11,025 Hz. The 20,000-byte file
declares a 44,100-byte `data` payload but contains only 19,956 payload bytes,
and its RIFF size declares a 44,144-byte file. The expected result is successful
PCM identification followed by a truncated-input result during decoding, not
successful completion, an out-of-bounds read, or an infinite loop.

## `test-8000Hz-le-3ch-5S-24bit-inconsistent.wav`

- Source repository: [scipy/scipy](https://github.com/scipy/scipy)
- Commit: [`14af6145d0402a9cac45ef7db8dcad78f023517e`](https://github.com/scipy/scipy/commit/14af6145d0402a9cac45ef7db8dcad78f023517e)
- Immutable source: [raw WAV](https://raw.githubusercontent.com/scipy/scipy/14af6145d0402a9cac45ef7db8dcad78f023517e/scipy/io/tests/data/test-8000Hz-le-3ch-5S-24bit-inconsistent.wav)
- SHA-256: `b76320ae2de1e892d00de92bc0884304e686e3a394cc7ca7533d2929bbcea4d5`
- License: BSD 3-Clause; see
  [`SCIPY-BSD-3-CLAUSE.txt`](SCIPY-BSD-3-CLAUSE.txt).

The header describes three-channel, 24-bit PCM at 8 kHz. Its byte rate is the
expected 72,000 bytes per second, but its block alignment is 4 instead of 9.
The expected result is rejection of the contradictory format geometry before
decoding.

## Odd-sized non-data chunk gap

`bug1301226-odd.wav` is deliberately not included. Although it contains
odd-sized chunks and pad bytes, it also declares `0xffffffff` RIFF and `data`
sizes, so it is not a clean valid-file acceptance case. No existing valid file
with an odd-sized non-data chunk before `data` was verified in the inspected
SciPy, CPython, wavrw, or wavefile corpora. Coverage for that case should wait
for a redistributable field file or use a fixture explicitly labeled as
synthetic.

## Integrity check

From the repository root:

```sh
shasum -a 256 test/host/fixtures/wav/*.wav
```

The output must match the four digests documented above.
