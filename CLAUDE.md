This repository is an independent macOS (Apple silicon) port of FEX. It is not the
upstream FEX-Emu/FEX project and is not affiliated with it.

Rules for anyone, human or AI, working here:

- AI-assisted code is allowed in this repository.
- Nothing from this repository may be submitted to FEX-Emu/FEX. Upstream does not accept
  AI-assisted contributions. Never open a pull request, issue or patch against upstream
  from this work.
- Pull requests are opened against this repository only, never against the fork parent.
  GitHub defaults to the parent, so set the base repository explicitly.
- Mac only. Do not add iOS or Android branches.
- Use the method macOS provides. Keep a workaround only when no native method exists, and
  say why in a comment.
- Every behaviour change needs a test that runs on Apple silicon.
- Keep upstream's licence and copyright notices intact.

Where the port lives:

- `main` carries the port (merged from `macos/v1` at the owner's request for the 1.0
  review). It is no longer a mirror of upstream. Commit to `main` only when the owner
  asks.
- `macos/v1` is the integration branch for further work. Work packages branch from it as
  `macos/wp-<id>-<short-name>`; only the integrator merges into it.
- Upstream FEX-Emu/FEX is tracked through fetch-only remotes whose push addresses are
  disabled (`upstream-readonly`, which `Scripts/macos_sync_upstream.sh` adds and checks;
  the owner's working clone also has `upstream`, likewise disabled). Remotes are local
  configuration, so check `git remote -v` in a new clone. The script merges new upstream
  commits onto `macos/v1`; see `docs/macOS-upstream-sync.md`.
- Upstream's own workflows carry a port change, `if: github.repository == 'FEX-Emu/FEX'`
  on every job, so they never run here. Keep it on every sync. Never register a
  self-hosted runner on this public repository.

Where to start reading:

- `docs/macOS-port-status.md`: what works, on which machine it was shown, the known
  limits, the open items and how to run the tests.
- `docs/macOS-v1.0-plan.md`: the 1.0 exit criteria and work packages, and the "Result"
  section, criterion by criterion, with evidence and limits.
- `docs/macOS-embedding-interface.md`: the `fexmac` C interface a host program uses.
- `docs/macOS-upstream-sync.md`: how upstream is merged.
