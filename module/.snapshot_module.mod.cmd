savedcmd_snapshot_module.mod := printf '%s\n'   snapshot_module.o | awk '!x[$$0]++ { print("./"$$0) }' > snapshot_module.mod
