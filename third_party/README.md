# Third-party dependencies

The west workspace checks out
[Space Inventor libparam](https://github.com/spaceinventor/libparam) at
`third_party/libparam`. It remains an independent upstream Git project pinned
by the K-FSW west manifest to
`c296dfb6055a3c360f44dcbbd6ad108e98c76640`; its source is not vendored into
this repository. Upstream uses the MIT license. Run `west update libparam` from
the workspace root to populate or refresh the checkout.
