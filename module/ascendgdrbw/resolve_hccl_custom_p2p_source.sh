#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BASE_HCCL_REPO="${REPO_ROOT}/hccl"
HCCL_TAG="${HCCL_CUSTOM_P2P_TAG:-v8.5.0-beta.1}"
WORKTREE_DIR="${REPO_ROOT}/hccl-${HCCL_TAG}"

if ! git -C "${BASE_HCCL_REPO}" rev-parse --git-dir >/dev/null 2>&1; then
    echo "failed to find a valid hccl git repository at ${BASE_HCCL_REPO}" >&2
    exit 1
fi

if ! git -C "${BASE_HCCL_REPO}" rev-parse -q --verify "refs/tags/${HCCL_TAG}^{commit}" >/dev/null 2>&1; then
    echo "failed to find hccl tag ${HCCL_TAG} in ${BASE_HCCL_REPO}" >&2
    exit 1
fi

TARGET_COMMIT="$(git -C "${BASE_HCCL_REPO}" rev-parse "${HCCL_TAG}^{commit}")"

if git -C "${WORKTREE_DIR}" rev-parse --git-dir >/dev/null 2>&1; then
    CURRENT_COMMIT="$(git -C "${WORKTREE_DIR}" rev-parse HEAD)"
    if [ "${CURRENT_COMMIT}" != "${TARGET_COMMIT}" ]; then
        git -C "${WORKTREE_DIR}" checkout --detach "${HCCL_TAG}" >/dev/null 2>&1
    fi
else
    git -C "${BASE_HCCL_REPO}" worktree add --detach "${WORKTREE_DIR}" "${HCCL_TAG}" >/dev/null
fi

echo "${WORKTREE_DIR}"
