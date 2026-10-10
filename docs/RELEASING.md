# Releasing Mortalfin

## Once: your public identity

Everything you commit becomes public, with the e-mail address on each commit.

1. On GitHub: Settings > Emails > tick **Keep my email addresses private**, and note the `...@users.noreply.github.com` address.
2. In this project:
   ```bash
   git config user.name "mortaljinx"
   git config user.email "<that no-reply address>"
   ```
3. If commits already exist with another address, rewrite them before the first push:
   ```bash
   git rebase -r --root --exec 'git commit --amend --reset-author --no-edit'
   ```
4. Create `~/.mortalfin-private` (outside the project) with one private word per line: your server's name, your
   domain, a nickname. `ps5/tools/security-check.py` looks for them everywhere, and for the values in `config.h`.

## Each release

1. Set the version in `VERSION` (`1.0.0-beta.2`, ...). Every release needs a new one.
2. `python3 ps5/tools/version.py --sync`, then commit everything.
3. `ps5/tools/release.sh`. It stops on anything of yours it finds. It ends with the zip in `dist/release-<version>/`.
4. **Install that zip on your console and try it** before you publish: it is the build your testers get.
5. Tag and push:
   ```bash
   git tag -a 1.0.0-beta.1 -m "Mortalfin 1.0.0-beta.1"
   git push origin main 1.0.0-beta.1
   ```
6. On GitHub: Releases > Draft a new release > choose the tag > tick **This is a pre-release** > attach
   `Mortalfin-<version>.zip` and `SHA256SUMS` > **Publish** (not draft). Or with the GitHub CLI:
   ```bash
   gh release create 1.0.0-beta.1 dist/release-1.0.0-beta.1/Mortalfin-1.0.0-beta.1.zip dist/release-1.0.0-beta.1/SHA256SUMS \
     --prerelease --title "Mortalfin 1.0.0-beta.1" --notes-file CHANGELOG.md
   ```
   Never replace an asset of a published release: its sha256 would no longer match. Make a new version instead.

## The Homebrew Store

The catalog is `blackbearreloaded/ps5-homebrew-catalog` ([rules](https://github.com/blackbearreloaded/ps5-homebrew-catalog/blob/main/docs/submitting.md)).

1. Check that `apps/PPSA99141.json` does not exist there (the title ID must be unique).
2. Fork the catalog with the **mortaljinx** account (the pull request must come from the account that owns the repository).
3. Copy `dist/release-<version>/PPSA99141.json` (written by `release.sh`) to `apps/PPSA99141.json` in your fork.
4. Open a pull request that changes only that file. The "Validate submission" check lists any problem.
5. Later releases: the catalog opens update pull requests itself when it finds a newer GitHub release. Raise `contentVersion` each time (`version.py` does).
