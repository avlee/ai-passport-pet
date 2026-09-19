<p align="right">
  <strong>简体中文</strong> · <a href="upstream-sync.md">English</a>
</p>

# 上游同步

这个仓库是**下游分支，不是 fork**：它发布自己的固件，从不向官方仓库提 PR。它需要的
只是官方更新落地时能把改动拿进来。

## 让同步便宜的约定

`main` 是**官方 `main` 的镜像**，不承载任何本地提交；自定义功能都在各自的
`feature/*` 分支上，构建和烧录一律从功能分支走，不从 `main` 走。

这一条约定就是同步便宜的来源：`main` 从不分歧，推进它就只是一次快进，而快进不可能
冲突。冲突只会出现在承载你工作的那条分支上，且只涉及双方都动过的那几个文件。

## 怎么同步

    ./tools/sync-upstream.sh                  # 同步 main，然后把当前分支变基上去
    ./tools/sync-upstream.sh --check          # 只报告，什么都不改
    ./tools/sync-upstream.sh --no-integrate   # 只同步 main，不碰当前分支

脚本按顺序做这几件事：

1. 拉官方远端，报出 `main` 落后多少；
2. 快进 `main` —— 如果 `main` 上有本地提交，直接拒绝往下走；
3. 把 `main` 推到 `origin`；
4. 把你当前所在分支变基到新的 `main` 上（`--merge` 则改用合并）；
5. 打印该分支的强推命令，除非给了 `--push-branch`。

之后跑门禁；固件有变动就上真机确认：

    ./tools/validate.sh --static
    ./tools/validate.sh --firmware

## 为什么没有 CI 任务做这件事

仓库自带 `.github/workflows/sync-main.yml`，那是给 **fork** 用的同步任务，在这里跑
不起来。原因值得记一笔。

GitHub 只从**默认分支**读取 `schedule` 和 `workflow_dispatch` 类型的 workflow，所以
这个文件必须存在于 `main` 上，任务才可能被触发。可它自带
`if: github.event.repository.fork`，在非 fork 仓库上是个死件；而删掉这一行就等于往
`main` 上提交 —— 那一刻 `main` 不再是镜像，这个 workflow 干的硬重置会立刻把它抹掉，
连带那个让它跑起来的改动本身。`main` 上任何其它本地文件（一个 README、一个自定义
workflow）都破坏同一条不变量，都会被同样抹掉。

所以同步只能从 `main` 之外驱动。如果非要做成自动的，可行的形状是把 workflow 放在
非默认分支上：把仓库的默认分支指向一条携带「去掉开关的副本」的自定义分支，让它去重
置 `main`。这是拿一个不太直观的仓库设置换免手动同步。脚本是这笔交换里更简单的一半，
也是本仓库采用的一半。

## 官方远端

GitHub 是主源。Gitee 那个仓库是镜像，落后于它 —— 截至 2026-09-19 落后七个提交，且
不包含任何 GitHub 上没有的东西。从 GitHub 同步，把 Gitee 当备用：

    git remote add upstream https://github.com/FoloToy/ai-passport.git   # 主源
    git remote add gitee    https://gitee.com/folotoy/ai-passport.git    # 镜像

脚本会去找 URL 指向官方仓库的远端，优先取名为 `upstream` 的那个。`UPSTREAM_REMOTE`
与 `UPSTREAM_BRANCH` 可以覆盖这个选择。

## 会撞在哪，怎么解

重叠面很小且稳定。对 2026-09-18 那次官方更新，双方都改过的文件有八个：

- `.gitignore`、`tools/validate.sh`
- `docs/development/README{,.zh_CN}.md`
- `docs/development/engineering/build-and-test{,.zh_CN}.md`
- `docs/development/engineering/firmware-layout{,.zh_CN}.md`

真正冲突的只有两个：`.gitignore` 和 `tools/validate.sh` —— 都是因为两边往同一段末尾
追加了内容，所以解法就是把两半都留下。那几个文档虽然重叠，但改动落在文件的不同位置，
自动合并过去了。

变基会在第一个冲突处停下，所以一次同步是几次小合并，而不是一次大合并。脚本停下时，
解冲突、`git add`、`git rebase --continue`。

## 本分支携带的适配

官方代码在 macOS 上要改过才能构建、并通过它自己的门禁。每一条都是「上游修好之后就该
删掉」的候选：

- **`tools/validate.sh` 探测段回收链接选项。** 官方给 demo 回归测试传的是
  `-Wl,--gc-sections`，只有 GNU ld 认；Mach-O 的链接器直接拒收。把选项去掉同样不算
  修好，因为那几个测试正是靠「链接前先把未引用函数丢掉」才过得去。探测逻辑是：认
  `--gc-sections` 就用它，否则退到 `-Wl,-dead_strip`。

## 如果你还是决定在 `main` 上开发

无冲突快进正是那条镜像约定买来的，所以在 `main` 上开发就是放弃它。具体会变成：

- 脚本会拒绝运行，因为 `main` 不再是官方分支的祖先（`git log --oneline
  upstream/main..main` 会列出多出来的提交）；
- 同步变成 `git merge upstream/main`，每次官方发版都要在 `main` 上解冲突，一直下去；
- `sync-main.yml` 里的 `if: fork` 从无害变成陷阱：万一这个仓库真的被变成 fork，那个
  workflow 会把 `main` 硬重置到官方，本地历史会被抹掉。

如果你要的就是「一条线即产品」，更稳的形状是让 `main` 继续当镜像，另指定一条长期分支
作为发布线。它构建和烧录跟 `main` 完全一样，而每次同步后仍然能干净变基。

## 相关文档

- `docs/development/ci/CI-sync-main.{md,zh_CN.md}`：本仓库用不上的官方 fork 同步
  workflow。
- `docs/fork-guide.{md,zh_CN.md}`：`main` 所遵循的官方约定。
