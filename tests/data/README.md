# References of the print encoder

`make test` draws six synthetic pages (`tests/test_encoder.c`: text and
lines, gray, heavy page; A4 and Letter), encodes them with `src/lilo2.c` as
the application does, and checks them (`tests/test_encoder.py`):

- `build/lilo2check` finds the stream well formed, with the right paper, size
  and number of bands;
- the share of black dots and every dot it decodes are the expected ones;
- the stream is, byte for byte, the reference one.

A page weighs from tens of kilobytes to megabytes of data, so only the
SHA-256 of each stream is kept, in `lilo2.sha256` (the format of
`sha256sum`).

The test is offline: it only writes and decodes files, and sends nothing to a
printer. A reference says that the output has not changed, not that a printer
accepts it.

## While `lilo2.sha256` does not exist

The first references have to be written on Linux. Until the file is
committed, `make test` runs all the other checks, prints a warning that the
byte-for-byte comparison is skipped, and still succeeds.

## Writing the references

Do this for the first references, and again only after an intended change of
the output: a new test page, or a change to the encoder. Nothing here asks for
a print.

On Linux, with a C compiler, make and Python 3.9 or later (PAPPL and libusb
are not needed):

```
make references
make test
git add tests/data/lilo2.sha256
```

`make references` writes nothing if a page fails the other checks. Build from
a commit whose output is known to be good, and commit the file together with
the change that explains it.

Without a Linux machine, use the "Package" workflow of GitHub Actions: while
the file does not exist, the log of its step "Build and test the package"
ends the encoder test with the lines of `lilo2.sha256`. Copy them into the
file. The amd64 and arm64 jobs must give the same lines. Once the file
exists, a changed stream fails the test, and the log shows the new SHA-256
of each page that changed.

When the test fails, the streams of the build are in `build/encoder/`.
