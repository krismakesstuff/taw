# Copies taw.exe to taw-mcp.exe beside it, for agents: register taw-mcp.exe --mcp, so the
# bridges Claude Code sessions keep running hold that copy and never taw.exe, which the next
# build has to overwrite. A bridge still running holds the copy too; then it's left as it is
# (the next build with no session open updates it), and the build goes on.
# cmake -DFROM=<taw.exe> -DTO=<taw-mcp.exe> -P CopyBridge.cmake
file(COPY_FILE "${FROM}" "${TO}" RESULT result ONLY_IF_DIFFERENT)

if (NOT result EQUAL 0)
    message("taw-mcp.exe is in use (a Claude Code session's taw bridge), so it's the previous build's; "
            "it's updated by the next build with no session open")
endif()
