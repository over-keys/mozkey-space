# mozkey-space 1.1.0 integration validation

This is an experimental integration of upstream `v1.1.0`
(`7f8af6b852907aab93d1551530407a434c6f044c`) into mozkey-space `v1.0.1`
(`b53e7150d980c1142a119e8cc61f23392671cc45`). It is not a tested release.

## Persisted configuration compatibility

The existing mozkey-space Space-conversion delay occupies protobuf field 1050.
Upstream 1.1.0 independently assigned 1050 to `allow_zenz_emoji_changes`.
This integration keeps the existing delay at 1050 and uses 1051 for the emoji
flag. Do not copy binary configuration databases between upstream Mozkey 1.1.0
and this fork. This does not implement migration from upstream profiles.

The Python wire tests compile the actual current config.proto and verify old
absent/zero/1000-ms wire fixtures, unchanged emoji defaults, and independence of
the new emoji flag from the delay. Run in an isolated environment with
`grpcio-tools` installed:

```
python tools/tests/test_config_wire_compatibility.py
```

C++ regression tests in `config_handler_test.cc` additionally cover the actual
ConfigHandler file reload/save path. These need the normal Bazel build.

## Remaining platform gates

Run Windows and macOS build/tests, including Ctrl+Delete persistence (skipped
on Linux), independent Space/live delays, candidate selection, deferred display,
cancel, submit/undo, stale asynchronous replies, and installer upgrades.
Confirm settings and user dictionaries survive upgrades. Keep an existing
installer and profile backup for rollback. No publish/push/install is implied.

## Space-oriented product defaults

Only absent fields receive these values; explicit saved preferences survive:
- live conversion: off
- real-time conversion: off
- direct commit on punctuation: off
- Zenz correction: on; normal Space-conversion delay: 0 ms
- normal candidate display: on; deferred/hide-Mozc display: off
- emoji changes: prohibited

The independent live-typing debounce remains 700 ms and is inactive while live
conversion is off. Feedback/context and safety limits otherwise retain their
existing defaults. Explicit Reset to defaults selects the new Space defaults.

## Local verification results

Linux with the supported checked-in sample dictionary: 11 C++ test targets,
819 cases, 766 passed and 53 platform-specific skips, zero failures. The daily
release dictionary was not available locally and its source configuration was
restored after testing. Windows/macOS tests remain CI gates, not local passes.

The upstream direct-commit test was adapted to the fork's ordinary-conversion
contract: rejected Zenz learns exactly the visible Mozc candidate once, not the
cancelled hiragana; accepted Zenz with history learning disabled learns zero
such candidates. All eight punctuation/macro/failure variants pass.

Additional checks: 54 Python unit tests (release 22, runtime lock 11, runtime
integration 2, offline audit 8, version 2, wire compatibility 3, dictionary
pinning 6), 27 installer preflight fixtures, all nine protocol schemas compile,
network-symbol guard, translation generation/XML and whitespace checks pass.

The nested Bazel Linux sandbox could not access random devices reliably in this
cloud executor, so build actions used Bazel local execution within the existing
execution sandbox. TLS remained verified using the existing system Java trust
store. No OS trust store or security policy was modified.
