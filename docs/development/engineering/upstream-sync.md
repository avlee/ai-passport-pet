<p align="right">
  <strong>English</strong> · <a href="upstream-sync.zh_CN.md">简体中文</a>
</p>

# Upstream sync

This repository is a **downstream, not a fork**: it ships its own firmware and
never opens a pull request against the official repository. What it does need is
to pick up official changes whenever they land.

## The rule that keeps syncing cheap

`main` is a **mirror of the official `main`** and carries no local commit. Every
custom feature lives on its own `feature/*` branch, and building or flashing
happens from the feature branch, never from `main`.

That one rule is what makes a sync cheap. Because `main` never diverges,
advancing it is a fast-forward, and a fast-forward cannot conflict. Conflicts
can only appear in the branch that carries your work, and only for the few files
both sides touched.

## Syncing

    ./tools/sync-upstream.sh                  # sync main, then rebase the current branch onto it
    ./tools/sync-upstream.sh --check          # report only, change nothing
    ./tools/sync-upstream.sh --no-integrate   # sync main, leave the current branch alone

In order, the script:

1. fetches the official remote and reports how far behind `main` is;
2. fast-forwards `main`, refusing to continue if `main` has local commits;
3. pushes `main` to `origin`;
4. rebases the branch you are on onto the new `main` (`--merge` to merge instead);
5. prints the force-push command for that branch, unless `--push-branch` is given.

Then run the gate, and check the device if the firmware changed:

    ./tools/validate.sh --static
    ./tools/validate.sh --firmware

## Why no CI job does this

The repository ships `.github/workflows/sync-main.yml`, which syncs a *fork*. It
cannot work here, and the reason is worth recording.

GitHub only reads `schedule` and `workflow_dispatch` workflows from the **default
branch**, so this file has to exist on `main` for the workflow to run at all. But
it ships with `if: github.event.repository.fork`, so it is inert on a repository
that is not a fork — and deleting that one line means committing to `main`. At
that point `main` is no longer a mirror, and the hard reset this workflow
performs would erase the very edit that made it run. Any other local file on
`main` — a README, a workflow — breaks the same invariant and is erased the same
way.

So the sync is driven from outside `main`. If it must be automated anyway, the
shape that works is to hold the workflow on a non-default branch: point the
default branch at a custom branch carrying an un-gated copy, and let that copy
reset `main`. That trades a non-obvious repository setting for hands-off
syncing. The script is the simpler half of that trade, and the one this
repository uses.

## Official remotes

GitHub is the primary source. The Gitee repository is a mirror and lags behind
it — as of 2026-09-19 it is seven commits back and holds nothing GitHub lacks.
Sync from GitHub; keep Gitee as a fallback.

    git remote add upstream https://github.com/FoloToy/ai-passport.git   # primary
    git remote add gitee    https://gitee.com/folotoy/ai-passport.git    # mirror

The script looks for a remote whose URL points at the official repository,
preferring one named `upstream`. `UPSTREAM_REMOTE` and `UPSTREAM_BRANCH`
override that choice.

## What conflicts, and how

The overlap is small and stable. Against the 2026-09-18 official update, both
sides had changed eight files:

- `.gitignore`, `tools/validate.sh`
- `docs/development/README{,.zh_CN}.md`
- `docs/development/engineering/build-and-test{,.zh_CN}.md`
- `docs/development/engineering/firmware-layout{,.zh_CN}.md`

Only two of them actually conflicted: `.gitignore` and `tools/validate.sh`, both
because each side appended a block to the same region, so the resolution is to
keep both halves. The documentation files overlapped but merged on their own,
because the edits landed in different parts of each file.

A rebase stops at the first conflict, so a sync is a handful of small merges
rather than one large one. When the script stops, resolve, `git add`, and
`git rebase --continue`.

## Adaptations this branch carries

Changes the official tree needs before it builds and passes its own gate on
macOS. Each is a candidate to drop once upstream fixes it:

- **`tools/validate.sh` probes for a section-GC link flag.** Upstream's demo
  runtime tests pass `-Wl,--gc-sections`, which only GNU ld accepts; the Mach-O
  linker rejects it outright. Removing the flag is not a fix either, because
  those tests rely on unused functions being discarded before the link. The
  probe uses `--gc-sections` where it is accepted and `-Wl,-dead_strip`
  otherwise.

## If you decide to develop on `main` anyway

The mirror rule is what buys the conflict-free fast-forward, so developing on
`main` gives it up. Concretely:

- the script refuses to run, since `main` is no longer an ancestor of the
  official branch (`git log --oneline upstream/main..main` names the offending
  commits);
- syncing becomes a `git merge upstream/main` with conflicts resolved on `main`,
  at every official release;
- the `if: fork` guard stops being harmless and becomes a trap: if the
  repository ever becomes a fork, that workflow hard-resets `main` to upstream
  and erases the local history.

If a single line that *is* the product is what you want, the safer shape is to
keep `main` a mirror and name one long-lived branch the release line. It builds
and flashes exactly as `main` would, and it still rebases cleanly after a sync.

## Related

- `docs/development/ci/CI-sync-main.{md,zh_CN.md}` — the upstream fork-sync
  workflow this repository cannot use.
- `docs/fork-guide.{md,zh_CN.md}` — the upstream convention `main` follows.
