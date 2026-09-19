#!/usr/bin/env bash
# 把官方 FoloToy/ai-passport 的更新同步进来。
#
# 本仓库的约定: main 永远是官方 main 的纯镜像, 不承载任何本地提交; 自定义固件功能
# 一律放在 feature/* 分支。所以 main 只允许快进, 官方改动进来之后再把当前分支跟上。
#
# 为什么不用 CI 自动同步: GitHub 的 schedule / workflow_dispatch 只认默认分支上的
# workflow 文件, 而要让官方那份 .github/workflows/sync-main.yml 在非 fork 仓库里生效,
# 就得删掉它的 `if: fork` 判断 —— 可这个改动本身得留在 main 上, main 就不再是纯镜像,
# 同步动作的硬重置会立刻把它抹掉。死循环。所以这条同步只能从 main 之外驱动。
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${repo_root}"

local_branch="main"
target_remote="origin"
upstream_branch="${UPSTREAM_BRANCH:-main}"
upstream_remote="${UPSTREAM_REMOTE:-}"

check_only=0
push_main=1
integration="rebase" # rebase | merge | none
push_branch=0

usage() {
    cat >&2 <<'EOF'
Usage: tools/sync-upstream.sh [options]

Sync the official main into the local main (fast-forward only), then bring the
current branch up to date. See docs/development/engineering/upstream-sync.md.

Options:
  --check          report what the official branch has, change nothing
  --no-push        update the local main but do not push it
  --merge          integrate with merge instead of rebase (default: rebase)
  --no-integrate   stop after main is synced, leave the current branch alone
  --push-branch    push the current branch once integrated (force-with-lease on rebase)
  -h, --help       show this help

Environment:
  UPSTREAM_REMOTE  official remote name (default: auto-detect, normally upstream)
  UPSTREAM_BRANCH  official branch name (default: main)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check) check_only=1 ;;
        --no-push) push_main=0 ;;
        --merge) integration="merge" ;;
        --no-integrate) integration="none" ;;
        --push-branch) push_branch=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: unknown option $1" >&2
            usage
            exit 2
            ;;
    esac
    shift
done

# 官方远端默认叫 upstream; 没有就按 URL 找, 但只认指向官方仓库的, 别把用户自己的
# fork 认成上游 (所以不匹配裸的 "ai-passport")。
if [[ -z "${upstream_remote}" ]]; then
    if git remote get-url upstream >/dev/null 2>&1; then
        upstream_remote="upstream"
    else
        for name in $(git remote); do
            url="$(git remote get-url "${name}" 2>/dev/null || true)"
            case "${url}" in
                *FoloToy/ai-passport* | *folotoy/ai-passport*)
                    upstream_remote="${name}"
                    break
                    ;;
            esac
        done
    fi
fi

if [[ -z "${upstream_remote}" ]]; then
    cat >&2 <<'EOF'
ERROR: no remote points at the official repository.

Add one first (GitHub is the primary source; the Gitee mirror lags behind):
  git remote add upstream https://github.com/FoloToy/ai-passport.git
EOF
    exit 1
fi

upstream_ref="${upstream_remote}/${upstream_branch}"

if ! git fetch --prune --tags "${upstream_remote}"; then
    echo "ERROR: 拉取 ${upstream_remote} 失败（网络问题？）。还没改动任何东西。" >&2
    exit 1
fi

if ! git rev-parse --verify --quiet "${upstream_ref}" >/dev/null; then
    echo "ERROR: ${upstream_ref} does not exist on ${upstream_remote}." >&2
    exit 1
fi

# origin 拉不动不该挡住同步, 只是比较结果会用到它。
if ! git fetch --prune "${target_remote}" >/dev/null 2>&1; then
    echo "警告：拉取 ${target_remote} 失败（离线？），跳过与它的比较。" >&2
fi

head_before="$(git rev-parse --short "${local_branch}")"
upstream_head="$(git rev-parse --short "${upstream_ref}")"
behind="$(git rev-list --count "${local_branch}..${upstream_ref}")"

echo "官方远端   ${upstream_remote}  $(git remote get-url "${upstream_remote}")"
echo "官方分支   ${upstream_ref} = ${upstream_head}"
echo "本地 main  ${local_branch} = ${head_before}"

if [[ -n "$(git rev-parse --verify --quiet "${target_remote}/${local_branch}" || true)" ]]; then
    origin_head="$(git rev-parse --short "${target_remote}/${local_branch}")"
    if [[ "${origin_head}" == "${head_before}" ]]; then
        echo "${target_remote}/${local_branch} = ${origin_head}（与本地 main 一致）"
    elif git merge-base --is-ancestor "${local_branch}" "${target_remote}/${local_branch}"; then
        echo "${target_remote}/${local_branch} = ${origin_head}（超前本地 main）"
    else
        echo "${target_remote}/${local_branch} = ${origin_head}（落后本地 main）"
    fi
fi

# main 必须还是官方的祖先, 也就是"没有被塞进本地提交"。一旦不成立, 说明有人直接在
# main 上开发了 —— 那就不是本脚本能安全处理的情况了, 说清楚而不是硬来。
if ! git merge-base --is-ancestor "${local_branch}" "${upstream_ref}"; then
    cat >&2 <<EOF
ERROR: 本地 main 已经不是官方的纯镜像，无法快进。

main 上多出来的本地提交：
EOF
    git log --oneline "${upstream_ref}..${local_branch}" >&2
    cat >&2 <<EOF

本仓库的约定是 main 只做官方镜像、自定义功能放 feature/* 分支。先把这些提交挪到
分支上，main 再回到官方位：

  git branch my/custom-$(date +%Y%m%d) ${local_branch}
  git reset --hard ${upstream_ref}
  tools/sync-upstream.sh

如果确实打算把 main 当成自己的固件线（另一套模型），见
docs/development/engineering/upstream-sync.zh_CN.md 里的"如果要在 main 上开发"一节。
EOF
    exit 1
fi

if [[ "${behind}" == "0" ]]; then
    echo "官方新增   0 个提交（main 已经是最新）"
else
    echo "官方新增   ${behind} 个提交"
fi

if [[ "${check_only}" == "1" ]]; then
    if [[ "${behind}" != "0" ]]; then
        echo "---"
        git log --oneline "${local_branch}..${upstream_ref}"
    fi
    exit 0
fi

current="$(git rev-parse --abbrev-ref HEAD)"
if [[ "${current}" == "HEAD" ]]; then
    echo "ERROR: 处于游离 HEAD，先切到一个分支再同步。" >&2
    exit 1
fi

if ! git diff-index --quiet HEAD --; then
    echo "ERROR: 工作区有未提交的已跟踪改动，先提交或 stash 再同步。" >&2
    git status --short >&2
    exit 1
fi

if [[ "${behind}" != "0" ]]; then
    echo "快进 main：${head_before} -> ${upstream_head}"
    if [[ "${current}" == "${local_branch}" ]]; then
        git merge --ff-only "${upstream_ref}"
    else
        # 不切分支地快进本地 main：fetch 到本地 ref 默认就是 ff-only, 非快进会被拒。
        git fetch "${upstream_remote}" "${upstream_branch}:${local_branch}"
    fi
fi

if [[ "${push_main}" == "1" ]]; then
    remote_head="$(git rev-parse --verify --quiet "${target_remote}/${local_branch}" || true)"
    if [[ -n "${remote_head}" && "${remote_head}" == "$(git rev-parse "${local_branch}")" ]]; then
        echo "${target_remote}/${local_branch} 已经是最新，跳过推送。"
    else
        echo "推送 main -> ${target_remote}"
        if ! git push "${target_remote}" "${local_branch}:${local_branch}"; then
            cat >&2 <<EOF
ERROR: 推送被拒绝。${target_remote}/${local_branch} 上可能有本地提交或被人强推过。
先看一眼差异再决定：git log --oneline ${local_branch}..${target_remote}/${local_branch}
EOF
            exit 1
        fi
    fi
fi

integrated=0
if [[ "${integration}" == "none" ]]; then
    echo "按 --no-integrate 要求，不动当前分支 ${current}。"
elif [[ "${current}" == "${local_branch}" ]]; then
    echo "当前就在 main 上，没有分支要跟上。"
else
    echo "让 ${current} 跟上 ${local_branch}（${integration}）"
    if [[ "${integration}" == "rebase" ]]; then
        if ! git rebase "${local_branch}"; then
            cat >&2 <<EOF

ERROR: rebase 撞上冲突了。解完冲突后：
  git add <files> && git rebase --continue
想放弃：
  git rebase --abort

撞的通常是这几个文件（官方也在改、我们也在改）：
  .gitignore
  docs/development/README.md, docs/development/README.zh_CN.md
  docs/development/engineering/{build-and-test,firmware-layout}{,.zh_CN}.md
  tools/validate.sh
EOF
            exit 1
        fi
    else
        if ! git merge --no-edit "${local_branch}"; then
            echo "ERROR: merge 有冲突，解决后 git add 并 git commit 完成合并。" >&2
            exit 1
        fi
    fi
    integrated=1
fi

pushed_branch=0
if [[ "${integrated}" == "1" && "${push_branch}" == "1" ]]; then
    echo "推送 ${current} -> ${target_remote}"
    if [[ "${integration}" == "rebase" ]]; then
        git push --force-with-lease "${target_remote}" "${current}"
    else
        git push "${target_remote}" "${current}"
    fi
    pushed_branch=1
fi

cat <<EOF

完成。下一步：
  ./tools/validate.sh --static      # 仓库检查 + 主机侧测试
  ./tools/validate.sh --firmware    # 构建并核对合并镜像（需要先激活 ESP-IDF）
EOF

if [[ "${integrated}" == "1" && "${pushed_branch}" == "0" ]]; then
    if [[ "${integration}" == "rebase" ]]; then
        echo "  git push --force-with-lease ${target_remote} ${current}   # 变基改写了历史，所以要强推"
    else
        echo "  git push ${target_remote} ${current}"
    fi
fi
