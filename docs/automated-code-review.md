# Automated pull request review

SpareNode uses CodeRabbit to provide advisory automated review on pull requests.
It complements human review and the GitHub Actions `Quality gate`; it is not an
approval authority and must not be allowed to bypass protected-branch rules.

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

Configure a branch ruleset for `main` in GitHub repository settings:

1. Require a pull request before merging.
2. Require the CI `Quality gate` status check to pass.
3. Block force pushes and branch deletion.
4. Do not add CodeRabbit to the ruleset bypass list.
5. Do not treat CodeRabbit review as a replacement for required CI or human
   approval.

The committed CodeRabbit configuration sets `request_changes_workflow: false`,
so the bot posts review feedback without running its automatic approval workflow.
Branch protection remains the enforcement boundary that prevents the bot from
merging or pushing directly to `main`.

## Review lifecycle

- Opening a non-draft pull request triggers a full review automatically.
- Every subsequent push triggers an incremental review.
- Draft and explicitly WIP pull requests are skipped until they are ready.
- Pull requests created by dependency and GitHub Actions bots are skipped.
- CodeRabbit waits for GitHub checks and may include CI failures in its feedback;
  CI still reports and enforces its own result independently.

Useful PR comments:

- `@coderabbitai review` requests an incremental review.
- `@coderabbitai full review` requests a fresh review of the complete change.
- `@coderabbitai pause` pauses reviews during a series of updates.
- `@coderabbitai resume` resumes automatic reviews.

## Verify the integration

CodeRabbit reads `.coderabbit.yaml` from the pull request branch, so the SN-006
pull request can verify its own configuration. After pushing the branch, install
the App before opening or refreshing the pull request, then confirm that:

1. CodeRabbit posts review feedback without being manually requested.
2. A new commit produces an incremental review.
3. The independent CI `Quality gate` still runs and remains required.
4. CodeRabbit has no branch-protection bypass permission and cannot merge the
   pull request directly.
