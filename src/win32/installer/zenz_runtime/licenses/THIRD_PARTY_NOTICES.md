# Third-party notices

This product includes third-party software and model files.

## Zenz v3.2 small GGUF

- Repository: Miwa-Keita/zenz-v3.2-small-gguf
- Source: Hugging Face
- Original model file: ggml-model-Q5_K_M.gguf
- Included file: models/zenz-v3.2-small-Q5_K_M.gguf
- License: Apache License 2.0
- Source repository URL: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf
- Pinned source file URL: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/blob/c67e03e07d215c869f591b274c1631170d3e11fe/ggml-model-Q5_K_M.gguf
- Source revision: c67e03e07d215c869f591b274c1631170d3e11fe
- Model SHA-256: `29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673`
- License metadata at this revision: https://huggingface.co/Miwa-Keita/zenz-v3.2-small-gguf/raw/c67e03e07d215c869f591b274c1631170d3e11fe/README.md
- The current model card has separate, newer license metadata. These notices document the unchanged bytes from the pinned upload.
- Notes: Distributed without modification except for file placement, file naming, and packaging into the MSI installer.

## llama.cpp / ggml runtime

- Project: ggml-org/llama.cpp
- Source repository URL: https://github.com/ggml-org/llama.cpp
- Source tag: v0.7.0
- Source commit: 620d2225cc436ab0b001e3200e590e3b286ef9ab
- Mozkey tokenizer compatibility patch SHA256: 6DB5C11B2DA8415D6B37200EC6AA4F3FDCDDE8EFB1E7A71375FE331A7B0E829A
- Patched src/llama-vocab.cpp SHA256: 2CEF3BDD758D9F2EB4CB80C1FC149C75461E9E32AF06E03D67D99E36FF69BBED
- Source assets:
  - x64/llama-server.exe
  - arm64/llama-server.exe
- Installed file name: llama-server.exe
- Runtime policy: CPU-only; ggml/llama are statically linked into llama-server.exe. Windows x64 is an isolated MSVC Release build with the tracked tokenizer and dual-liveness patches (SHA256 35B953C97F870CBD7426DC865783BE36090F841C273AE779BD098E9F080F3735). Windows ARM64 is a ClangCL build with the tokenizer patch, GGML_NATIVE=OFF, GGML_CPU_ARM_ARCH=armv8-a (SHA256 6BDE0FAF466261C8233298ECB2BDF01A78AAEBE9729C03BA4EA95F045F772737). Source is pinned to upstream v0.7.0; MSI installed-lifecycle validation remains pending.
- Microsoft Visual C++ runtime DLLs remain external dependencies and are supplied by the architecture-specific MSI runtime payload.
- License: MIT License

## Privacy note

Zenz live correction is designed to run locally. The bundled runtime is started as a local process and the HTTP inference endpoint is expected to bind to 127.0.0.1 only. User input is not intentionally sent to external servers by this feature.
Zenz feedback learning can be disabled in the settings UI. Left context is sanitized before being included in prompts; sensitive-looking context such as URLs, email addresses, file paths, tokens, and long numeric identifiers is rejected before prompt construction.
