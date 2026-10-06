# Known issues

Accepted problems that can turn a full `tools/verify.sh` run red for a reason
other than a regression. The definition of done (`AGENTS.md`) tolerates a flaky
failure only when it is listed here **and** the test passes alone under
`ctest -R`; a newly accepted flake is added here in the same commit.

## How to use this file

When a test fails during a full run, rerun it alone with its exact name and
follow the procedure in [`docs/TESTING.md`](docs/TESTING.md). If it passes alone
and is listed below, the failure is the known flake — note it and move on; there
is nothing to debug. If it fails alone, or is not listed here, it is a
regression: debug it, do not add it just to make a run green.

## Known flaky tests

No accepted flakes right now. When one appears, add it here in the same commit
with its source, symptom, cause and lone-run check, following the procedure
above.

## Not a flake

A failure that is not listed above — or that also fails alone — is a regression.
Do not add it here to silence it.
