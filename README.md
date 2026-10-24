# Spiration Python Supports

[简体中文](README.zh_Hans.md) · [English](#)

## Quick Start

> Notice: We're using Python 3.11.9 and it may be changed at any time.

Learning more on [Spiration Documents](https://flamence.github.io/spiration.docs/en/).

## Python dependencies

An extension may declare PyPI dependencies in its `extension.json`:

```json
{
    "python": {
        "requirements": ["openai>=3.37,<4"]
    }
}
```

Before importing the extension entry, the loader installs these requirements with the
same CPython into the extension's private data directory
(`<data directory>/<extension id>/python/site-packages`) via
`python -m pip install --target <site-packages> -r <requirements.txt>`, then prepends
that directory to `sys.path`. The result is cached: `python/state.txt` records the
interpreter version, architecture and requirements and is reused until one of them
changes.

- Requirements must be plain PEP 508 specifiers **with a version constraint**;
  pip options, URLs and local paths are rejected.
- The first install requires network access to the configured PyPI index (the usual
  `PIP_*` environment variables apply). A failed installation aborts loading the
  extension and logs the tail of the pip output.
- `SPIRATION_PYTHON_DISABLE_PIP=1` skips automatic installation for offline or
  packaged deployments; dependencies must then already exist at the path above.
- `SPIRATION_PYTHON_PIP_TIMEOUT=<seconds>` overrides the pip timeout (default 300).
- The pip subprocess runs without a console window and without holding the GIL, so
  the host UI thread is only blocked during the first installation.

## Extension capabilities

Scripts receive an injected `spiration` module. Besides logging, paths, events,
services and the optional `spiration.ui` interface, it exposes the optional
`spiration.agent` interface (`supported()`, `register_provider(name, provider)`,
`unregister_provider(name)`) for contributing chat-completion providers to the
built-in agent.

## Contributing

For details, see [Contributing to Spiration](https://flamence.github.io/spiration.docs/en/contributing/).

## License

This repository is licensed under the [Apache License 2.0](LICENSE).
