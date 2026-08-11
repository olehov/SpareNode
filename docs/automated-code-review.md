# Automated pull request review

SpareNode uses CodeRabbit to provide advisory automated review on pull requests.
It complements maintainer review and the GitHub Actions `Quality gate`; it is
not an approval authority or a security enforcement boundary.

Repository-specific behaviour is defined in [`.coderabbit.yaml`](../.coderabbit.yaml).
The configuration enables reviews for new pull requests and incremental reviews
for every new push. It focuses review on modern C++ correctness, resource and
memory safety, filesystem and network security, concurrency, portability,
testing, CMake, and GitHub Actions security.

## Install the GitHub App

Installation is a one-time repository-owner action:

1. Open the [CodeRabbit GitHub setup guide](https://docs.coderabbit.ai/platforms/github-com).
2. Install and authorize the CodeRabbit GitHub App.
3. Choose **Only select repositories** and select `olehov/SpareNode`. Do not
   grant access to every repository.
4. Review the requested permissions before confirming the installation.
5. Keep repository configuration in `.coderabbit.yaml`; do not add API keys or
   other CodeRabbit secrets to this repository.

CodeRabbit's GitHub App permission set is managed by CodeRabbit. If it later
requests additional permissions, review the reason before approving the change.

## Protect `main`

The active branch ruleset for `main` is configured as follows:

1. Require a pull request before merging.
2. Require the CI `Quality gate` status check to pass and the branch to be up
   to date before merging.
3. Restrict updates to repository administrators in the bypass list, with the
   bypass mode set to **For pull requests only**.
4. Restrict branch deletion and block force pushes.
5. Keep CodeRabbit out of the ruleset bypass list.

The committed CodeRabbit configuration sets `request_changes_workflow: false`,
so the bot posts review feedback without running its automatic approval
workflow. The repository has one maintainer, so the ruleset requires zero
approving reviews; a second account controlled by the same person would not
provide independent review. Restricting updates, while granting only repository
administrators pull-request-only bypass, prevents CodeRabbit from merging or
pushing directly to `main`. The pull-request rule and independent `Quality gate`
remain the enforcement boundary.

## Configuration trust boundary

CodeRabbit reads `.coderabbit.yaml` from the pull request branch. An untrusted
change can weaken or disable automated review, exclude paths, or remove review
instructions. For this reason, CodeRabbit findings must never be treated as
proof of security coverage. The maintainer must inspect changes to
`.coderabbit.yaml`, and merge decisions must continue to rely on the protected
branch rules, the independent CI result, and maintainer review.

If SpareNode gains independent reviewers or moves to a GitHub organization,
protect this file with required CODEOWNERS approval or move the mandatory
CodeRabbit baseline to non-overridable organization-level global overrides.

## Review lifecycle

- Opening a non-draft pull request triggers a full review automatically.
- Every subsequent push triggers an incremental review.
- Draft pull requests and titles containing `WIP` or `DO NOT MERGE` are skipped.
- Pull requests authored by `dependabot[bot]` or `github-actions[bot]` are
  skipped.
- CodeRabbit waits for GitHub checks and may include CI failures in its feedback;
  CI still reports and enforces its own result independently.

Useful PR comments:

- `@coderabbitai review` requests an incremental review.
- `@coderabbitai full review` requests a fresh review of the complete change.
- `@coderabbitai pause` pauses reviews during a series of updates.
- `@coderabbitai resume` resumes automatic reviews.

## Verify the integration

After opening or updating a pull request, verify that:

1. CodeRabbit posts review feedback without a manual request.
2. A new commit produces an incremental review.
3. The independent CI `Quality gate` runs and remains required.
4. The active `main` ruleset still restricts updates, requires pull requests,
   and blocks force pushes and deletion.
5. Only repository administrators have **For pull requests only** bypass and
   CodeRabbit has no bypass permission, so the bot cannot update `main`.
6. Automated security findings remain advisory because pull-request changes can
   modify `.coderabbit.yaml`.
